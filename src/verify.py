#!/usr/bin/env python3
"""Crisp's module verification and lemma-strengthening workflow."""

import argparse
import json
import re
import shutil
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

from proofs import Budget, bind_predicate, digest, model_wires, save, solver_attempt


ROOT = Path(__file__).resolve().parents[1]
VEVAL = ROOT / "components/veval/build/veval"


def read(path):
    return json.loads(path.read_text())


def prepare_task(args, directory):
    directory.mkdir(parents=True)
    units, files, inputs = [], [], []
    for index, path in enumerate([*args.design, args.spec]):
        header = path.suffix.lower() in (".svh", ".vh")
        name = path.name if header else f"unit{index}.sv"
        if name in files or not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
            raise ValueError(f"cannot stage header {path} under its include name")
        shutil.copyfile(path, directory / name)
        files.append(name)
        if not header:
            units.append(name)
        inputs.append({"name": path.name, "sha256": digest(directory / name)})
    task = directory / "verify.sby"
    task.write_text(f"[options]\nmode prove\ndepth {args.depth}\n\n[engines]\nsmtbmc boolector\n"
                    f"\n[script]\nread_verilog -formal -sv {' '.join(units)}\nprep -top {args.top}\n"
                    "\n[files]\n" + "\n".join(files) + "\n")
    return task, inputs


def mine(job, scratch, evidence, budget):
    sources = scratch / "rtl"
    sources.mkdir(parents=True)
    files = []
    for item in job["inputs"]:
        if item["role"] in ("design", "harness"):
            path = sources / f"unit{len(files)}.sv"
            shutil.copyfile(item["path"], path)
            if digest(path) != item["sha256"]:
                raise ValueError("prepared RTL changed since veval elaboration")
            files.append(path)
    smart = ROOT / "components/smartVerilog/build/smart"
    if not smart.is_file():
        raise ValueError("SMART is required to mine lemmas. Build it with make smart.")
    work = scratch / "mining"
    command = [smart, *files, "--top", job["target"]["module"], "--hierarchical", "--msa",
               "--final-unbounded", "--simulator", "iverilog", "-j", "4", "--keep-work",
               "--workdir", work, "--output", scratch / "mined.sv"]
    status = budget.run(command, scratch, scratch / "miner-command")
    record = {"status": status, "module": job["target"]["module"], "candidates": []}
    if status == "success":
        events = [json.loads(line) for line in (work / "run-log.jsonl").read_text().splitlines()]
        checks = [event for event in events if event.get("stage") == "check"]
        candidates = (work / "invariants.txt").read_text().splitlines()
        if candidates and (len(checks) != 1 or checks[0]["mode"] != "k-induction"
                           or checks[0]["verified"] != len(candidates)):
            raise ValueError("SMART output lacks its final k-induction check record")
        record.update(candidates=candidates, final_check=checks[0] if checks else None)
    elif (scratch / "miner-command/stdout.log").is_file():
        shutil.copyfile(scratch / "miner-command/stdout.log", evidence / "mining.log")
    save(evidence / "mining.json", record)
    return record


def initial_attempt(result, native, evidence):
    target = result["target"]
    job = read(native / "request.json")
    raw = Path(result["raw"]["record"])
    cell = read(raw) if raw.is_file() else {}
    stages = cell.get("stages", [])
    stage = stages[-1] if stages else {}
    work = Path(stage["work_dir"]) if stage else None
    verdict = result["verdict"] if result["outcome"] == "completed" else result["outcome"]
    record = {"target": target, "verdict": verdict, "job_id": job["job_id"],
              "premises": job["premises"], "setup": job["setup"],
              "reason": result["reason"], "solver_status": stage.get("status"),
              "engine_detail": stage.get("engine_detail")}
    detail = record["engine_detail"] or {}
    wrapper = cell.get("wrapper", {})
    record["lemma_eligible"] = (detail.get("mode") == "prove" and detail.get("basecase_completed")
                                and detail.get("engine") == "smtbmc boolector"
                                and job["setup"]["clock_model"] == "single-declared-clock"
                                and not cell.get("request", {}).get("multiclock")
                                and wrapper.get("liveness") == [])
    if verdict == "refuted":
        traces = result["trace"]["raw"]
        if not traces or not Path(traces[0]).is_file():
            raise ValueError("module refutation lacks its audited base-case waveform")
        shutil.copyfile(traces[0], evidence / "trace.vcd")
        record["trace"] = "trace.vcd"
    model = None
    if work is not None:
        model_dir = work / "model"
        if (model_dir / "design.il").is_file():
            model = evidence / "model.il"
            shutil.copyfile(model_dir / "design.il", model)
            record["model_sha256"] = digest(model)
            if detail.get("mode") == "prove" and detail.get("engine") == "smtbmc boolector":
                (evidence / "initial.sby").write_text(
                    f"[options]\nmode prove\ndepth {stage['depth']}\n\n[engines]\nsmtbmc boolector\n"
                    "\n[script]\nread_rtlil model.il\n\n[files]\nmodel.il\n")
        for phase in ("basecase", "induction"):
            log = work / "engine_0" / f"logfile_{phase}.txt"
            if log.is_file():
                shutil.copyfile(log, evidence / f"initial-{phase}.log")
    save(evidence / "initial.json", record)
    return record, job, work, model


def check_module(args, result, result_path, out, index, budget, assumptions):
    evidence = out / "evidence" / f"check-{index:03d}"
    evidence.mkdir(parents=True)
    scratch = out / "work" / evidence.name
    initial, job, work, model = initial_attempt(result, result_path.parent, evidence)
    entry = {"target": initial["target"], "verdict": initial["verdict"],
             "initial": str((evidence / "initial.json").relative_to(out)), "lemmas_used": []}
    if args.split == "off":
        compiled = {p["source_entry"] for p in job["premises"]} | {job["setup"]["reset"]["source_entry"]}
        missing = assumptions - compiled
        if missing:
            entry.update(verdict="unknown", reason="whole-DUT job omitted assumptions: " + ", ".join(sorted(missing)))
            return entry
    if args.lemmas == "off" or entry["verdict"] != "unknown":
        return entry
    if not initial["lemma_eligible"] or model is None or not (work / "model/design_smt2.smt2").is_file():
        entry["reason"] = "lemma retry requires a single-clock safety check with a completed base case"
        return entry
    widths = model_wires(work / "model/design_smt2.smt2", job["target"]["top"])
    mining = mine(job, scratch, evidence, budget)
    entry["mining"] = str((evidence / "mining.json").relative_to(out))
    if mining["status"] != "success":
        entry["reason"] = "mining " + mining["status"]
        if mining["status"] == "failed":
            entry["error"] = entry["reason"]
        return entry
    proved, receipts = [], []
    solver = {"depth": args.depth, "budget": budget, "model_hash": initial["model_sha256"],
              "tools": result["config"]["effective"]["tool_paths"]}
    entry["candidates"] = []
    for number, expression in enumerate(mining["candidates"], 1):
        candidate = {"expression": expression}
        entry["candidates"].append(candidate)
        try:
            formula, wires = bind_predicate(expression, widths)
        except ValueError as error:
            candidate.update(verdict="unsupported", reason=str(error))
            continue
        predicate = {"expression": expression, "formula": formula, "wires": wires}
        folder = f"lemma-{number:03d}"
        proof = solver_attempt(evidence / folder, scratch / folder, model, [predicate], role="assert", **solver)
        proof_path = str((evidence / folder / "proof.json").relative_to(out))
        candidate.update(verdict=proof["verdict"], proof=proof_path)
        if proof["verdict"] == "proved":
            proved.append(predicate)
            receipts.append(proof_path)
    if proved:
        retry = solver_attempt(evidence / "retry", scratch / "retry", model, proved, role="assume", **solver)
        entry.update(verdict=retry["verdict"], lemmas_used=receipts,
                     retry=str((evidence / "retry/proof.json").relative_to(out)))
        if retry["verdict"] == "refuted":
            entry.update(verdict="error", error="retry contradicts the completed base case at the same depth")
    elif any(c["verdict"] == "error" for c in entry["candidates"]):
        entry["error"] = "lemma proof failed to run; see candidate proof records"
    return entry


def collect(args, out, report, budget):
    work = out / "work"
    native = work / "veval"
    task, report["inputs"] = prepare_task(args, work / "input")
    strategy, placement = ("top", "root") if args.split == "off" else ("auto", "split")
    command = [VEVAL, "check", task, "--out", native, "--lowering", work / "lowering",
               "--strategy", strategy, "--placement", placement, "--keep-work"]
    status = budget.run(command, work, work / "veval-command")
    report["veval"] = status
    database = native / "check.sqlite"
    if not (native / "check.json").is_file() or not database.is_file():
        report["status"] = "timeout" if status == "timeout" else "error"
        report["diagnostic"] = "work/veval-command/stdout.log"
        return
    check = read(native / "check.json")
    properties = {row["entry"]: {"source": row["entry"], "checks": [], "skipped": row["skipped"]}
                  for row in check["properties"] if row["kind"] == "assert"}
    assumptions = {row["entry"] for row in check["properties"] if row["kind"] == "assume"}
    report["source_assumptions"] = sorted(assumptions)
    report["source_count"] = len(properties)
    report["properties"] = list(properties.values())
    with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True) as db:
        origins = dict(db.execute("SELECT DISTINCT derived_entry,source_property FROM symeval_records "
                                  "WHERE derived_entry IS NOT NULL AND derived_entry != ''"))
        rows = db.execute("SELECT source_entry,instance,module,status,result_path FROM problems "
                          "WHERE status != 'not-selected' ORDER BY source_entry,instance,job_id").fetchall()
    for index, (source, instance, module, state, path) in enumerate(rows, 1):
        prop = properties[origins.get(source, source)]
        if path:
            result_path = Path(path)
            local = check_module(args, read(result_path), result_path, out, index, budget, assumptions)
        else:
            local = {"target": {"source_entry": source, "instance": instance, "module": module},
                     "verdict": "unknown", "reason": state}
        prop["checks"].append(local)
    if status != "success" or check["status"] != "finished":
        report["status"] = "timeout" if status == "timeout" else "error"
        report["diagnostic"] = "work/veval-command/stdout.log"
    else:
        report["status"] = "finished"


def main():
    parser = argparse.ArgumentParser(prog="crisp verify",
                                     description="Check a formal wrapper and its RTL with k-induction.")
    parser.add_argument("design", type=Path, nargs="+", help="RTL sources and included .vh/.svh headers")
    parser.add_argument("--top", required=True, help="name of the specification wrapper module")
    parser.add_argument("--spec", type=Path, required=True, help="SystemVerilog wrapper with named properties")
    parser.add_argument("--split", choices=("on", "off"), default="on")
    parser.add_argument("--lemmas", choices=("on", "off"), default="on")
    parser.add_argument("--depth", type=int, default=5, help="common induction depth (default 5)")
    parser.add_argument("--budget", type=int, default=60, help="overall wall-clock seconds (default 60)")
    parser.add_argument("--out", type=Path, help="new output directory (default crisp-out/run.*)")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9$]*", args.top):
        parser.error("--top needs a simple Verilog module identifier")
    if args.budget <= 0 or not 1 <= args.depth <= 1_000_000:
        parser.error("--budget must be positive and --depth must be between 1 and 1000000")
    if not all(p.is_file() for p in [*args.design, args.spec]):
        parser.error("every RTL and specification input must be a regular file")
    if not VEVAL.is_file():
        parser.error("missing veval build. Run make veval.")
    if args.out is None:
        Path("crisp-out").mkdir(exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="run.", dir="crisp-out")).resolve()
    else:
        out = args.out.resolve()
        try:
            out.mkdir(parents=True)
        except FileExistsError:
            parser.error(f"output already exists: {out}")
    budget = Budget(args.budget, VEVAL.with_name("consflow_process"))
    report = {"scope": "module", "top": args.top, "split": args.split, "lemmas": args.lemmas,
              "budget_s": args.budget, "depth": args.depth, "engine": "smtbmc boolector",
              "source_count": None, "properties": []}
    try:
        collect(args, out, report, budget)
    except (OSError, ValueError, KeyError, RuntimeError, sqlite3.Error) as error:
        report.update(status="error", diagnostic=str(error))
    report["elapsed_s"] = round(time.monotonic() - budget.start, 4)
    if report["status"] == "finished" and time.monotonic() >= budget.deadline:
        report["status"] = "timeout"
    properties = report["properties"]
    checks = [c for p in properties for c in p["checks"]]
    covered = sum(bool(p["checks"]) and all(c["verdict"] == "proved" for c in p["checks"])
                  for p in properties)
    report["sources_with_all_selected_checks_proved"] = covered if report["source_count"] is not None else None
    save(out / "report.json", report)
    for check in checks:
        print(f"{check['target']['source_entry']} @ {check['target']['instance']}: {check['verdict']}")
    if not checks:
        print(f"{report['status']}: no selected module result", file=sys.stderr)
    print(out / "report.json")
    if report["status"] == "error" or any(c.get("error") or c["verdict"] == "error" for c in checks):
        print(report.get("diagnostic", "see report for the failed proof stage"), file=sys.stderr)
        return 2
    if any(c["verdict"] == "refuted" for c in checks):
        return 1
    return 0 if report["status"] == "finished" and properties and covered == len(properties) else 3


if __name__ == "__main__":
    sys.exit(main())
