"""Verify SMART state predicates on the RTLIL model from a veval proof attempt."""

import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import time
from pathlib import Path


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


class Budget:
    """One wall-clock deadline for preparation, mining and all solver attempts."""

    def __init__(self, seconds, supervisor):
        self.start = time.monotonic()
        self.deadline = self.start + seconds
        self.supervisor = supervisor

    def run(self, command, cwd, log, env=None):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            return "timeout"
        subprocess.run([self.supervisor, "--directory", log, "--cwd", cwd,
                        "--timeout", str(remaining), "--", *command], check=False, env=env)
        receipt = json.loads((log / "command.json").read_text())
        if not receipt.get("cleanup_confirmed"):
            raise RuntimeError(f"command cleanup incomplete in {log}")
        return "timeout" if time.monotonic() >= self.deadline else receipt["status"]


def model_wires(smt_model, top):
    """Read public names and widths from the top of the elaborated SMT model."""
    widths = {}
    module = None
    with smt_model.open() as source:
        for line in source:
            if line.startswith("; yosys-smt2-module "):
                module = line.split(maxsplit=2)[2].strip()
            elif module == top and line.startswith("; yosys-smt2-wire "):
                name, width = line[len("; yosys-smt2-wire "):].rsplit(maxsplit=1)
                widths[name] = int(width)
    return widths


TOKEN = re.compile(r"\s*(\|->|&&|\|\||==|!=|[()!~^&|]|1'[bB][01]|[01]|"
                   r"[A-Za-z_][A-Za-z0-9_$]*(?:\.[A-Za-z_][A-Za-z0-9_$]*)*)")
OPERATORS = {"&&": "and", "&": "and", "||": "or", "|": "or", "^": "xor",
             "|->": "=>", "==": "=", "!=": "distinct"}


def bind_predicate(expression, widths):
    """Translate SMART's fully parenthesized, single-frame, Boolean output."""
    tokens = []
    expression = expression.strip()
    pos = 0
    while pos < len(expression):
        match = TOKEN.match(expression, pos)
        if not match:
            raise ValueError("outside the single-frame Boolean grammar")
        tokens.append(match[1])
        pos = match.end()
    tokens = iter(tokens)
    wires = set()

    def term(token):
        if token == "(":
            token = next(tokens)
            if token in ("!", "~"):
                value = f"(not {term(next(tokens))})"
            else:
                value = term(token)
                operator = next(tokens)
                if operator == ")":
                    return value
                if operator not in OPERATORS:
                    raise ValueError(f"unsupported operator {operator}")
                value = f"({OPERATORS[operator]} {value} {term(next(tokens))})"
            if next(tokens) != ")":
                raise ValueError("unbalanced SMART expression")
            return value
        wire = "u_dut." + token
        if token in ("0", "1'b0", "1'B0", "false") and wire not in widths:
            return "false"
        if token in ("1", "1'b1", "1'B1", "true") and wire not in widths:
            return "true"
        if widths.get(wire) != 1:
            raise ValueError(f"{wire} is absent or is not a one-bit wire in the proof model")
        wires.add(wire)
        return f"[{wire}]"

    try:
        formula = term(next(tokens))
    except StopIteration:
        raise ValueError("incomplete SMART expression") from None
    if next(tokens, None) is not None or not wires:
        raise ValueError("expected one Boolean predicate over DUT signals")
    return formula, sorted(wires)


def solver_attempt(directory, scratch, model, predicates, *, role, depth, budget, model_hash, tools):
    """Only the selected assertion changes between helper proofs and the retry."""
    directory.mkdir()
    rules = directory / "rules.smtc"
    rules.write_text("always\n" + "".join(f"{role} {p['formula']}\n" for p in predicates))
    wires = sorted({w for predicate in predicates for w in predicate["wires"]})
    # Retain the sampled nets even if removing the target cuts their only fanout.
    script = ["read_rtlil model.il", "setattr -set keep 1 " + " ".join("w:" + w for w in wires)]
    if role == "assert":
        if len(predicates) != 1:
            raise ValueError("each lemma needs its own independent assertion check")
        script.append("chformal -assert -remove")
    task = directory / "proof.sby"
    task.write_text(f"[options]\nmode prove\ndepth {depth}\nsmtc rules.smtc\n\n"
                    "[engines]\nsmtbmc boolector\n\n[script]\n" + "\n".join(script)
                    + "\n\n[files]\nmodel.il ../model.il\nrules.smtc\n")
    if digest(model) != model_hash:
        raise ValueError("the original module model changed before the solver attempt")
    rules_hash = digest(rules)
    log = scratch / "command"
    run = scratch / "sby"
    command = [tools["sby"], "--yosys", shlex.quote(tools["yosys"]),
               "--smtbmc", shlex.quote(tools["yosys-smtbmc"]),
               "-d", os.path.relpath(run, directory), task.name]
    env = os.environ.copy()
    env["PATH"] = str(Path(tools["boolector"]).parent) + os.pathsep + env["PATH"]
    status = budget.run(command, directory, log, env)
    record = {"role": role, "model_sha256": model_hash, "predicates": predicates,
              "task_sha256": digest(task), "constraints_sha256": rules_hash,
              "depth": depth, "engine": "smtbmc boolector", "process": status}
    raw_status = (run / "status").read_text().split()[0] if (run / "status").is_file() else None
    record["sby_status"] = raw_status
    logs = {}
    for phase in ("basecase", "induction"):
        source = run / "engine_0" / f"logfile_{phase}.txt"
        if source.is_file():
            shutil.copyfile(source, directory / f"{phase}.log")
            logs[phase] = source.read_text()
    base = logs.get("basecase", "")
    induction = logs.get("induction", "")
    staged = run / "src"
    if raw_status in ("PASS", "FAIL", "UNKNOWN"):
        if digest(staged / "model.il") != model_hash or digest(staged / rules.name) != rules_hash:
            raise ValueError("the solver loaded different model or constraint bytes")
    if (status == "success" and raw_status == "PASS" and "Status: passed" in base
            and "Temporal induction successful" in induction and "Status: passed" in induction):
        record["verdict"] = "proved"
    elif status == "failed" and raw_status == "FAIL" and "Status: failed" in base:
        wave = run / "engine_0/trace.vcd"
        record["verdict"] = "unknown"
        if wave.is_file():
            shutil.copyfile(wave, directory / "trace.vcd")
            record.update(verdict="refuted", trace="trace.vcd")
    elif status == "timeout" or raw_status in ("UNKNOWN", "TIMEOUT"):
        record["verdict"] = "unknown"
    else:
        record["verdict"] = "error"
        if (log / "stdout.log").exists():
            shutil.copyfile(log / "stdout.log", directory / "diagnostic.log")
    save(directory / "proof.json", record)
    return record
