"""Exercise Spec 30 via the CLI in the case suite's private working directory."""
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
crisp = root / "build/crisp"
cases = root / "tests/cases"
header = "stage\tproperty\tinstance\tstatus\tdetail\n"


def records(path):
    data = path.read_bytes().decode()
    assert data.startswith(header) and data.endswith("\n"), data
    rows = [line.split("\t") for line in data[len(header):-1].split("\n") if line]
    assert all(len(row) == 5 for row in rows), rows
    return rows


def progress_path(stdout):
    match = re.search(r"^  progress  (.+)$", stdout, re.M)
    assert match, stdout
    return Path(match[1])


# The second local worker stops in its real SMT backend, after the parent
# has consumed and published the first result. Start a concurrent complete
# run of the same task to check that neither journal clobbers the other.
salt = Path("two-local.salt")
salt.write_text(f"[files]\n{cases}/veval-two.sv\n[design]\ntop two_top\n"
                "[options]\nveval only\nmode bmc\nlevel word\ndepth 5\n[resource]\ntimeout 20\n")
shim = Path("progress-tools").resolve()
shim.mkdir()
solver = shim / "yices-smt2"
solver.write_text('#!/bin/sh\nif [ "${PWD##*/}" = check1 ]; then\n'
                  '  echo "$$" > "$MARKER"\n  exec sleep 30\nfi\nexec "$YICES" "$@"\n')
solver.chmod(0o755)
marker = shim / "blocked"
env = dict(os.environ, PATH=f"{shim}:{os.environ['PATH']}", YICES=shutil.which("yices-smt2"), MARKER=str(marker))
with open("progress-interrupt.out", "w+") as output:
    proc = subprocess.Popen([crisp, salt], stdout=output, start_new_session=True, env=env)
    worker_group = None
    try:
        deadline = time.monotonic() + 10
        while not marker.exists() or not marker.read_text().strip():
            assert proc.poll() is None and time.monotonic() < deadline, "second worker did not reach Yices"
            time.sleep(.01)
        worker_group = os.getpgid(int(marker.read_text()))
        assert worker_group != os.getpgrp()
        journal = progress_path(Path(output.name).read_text())
        before = journal.read_bytes()
        rows = records(journal)
        first = "two_top.u_child.p_one"
        assert [row[:4] for row in rows] == [
            ["local", first, "two_top.u_child", "passed"], ["final", first, "", "passed"]], rows
        complete = subprocess.run([crisp, salt], capture_output=True, text=True, check=True, timeout=10)
        other = progress_path(complete.stdout)
        assert other != journal and journal.read_bytes() == before
        finals = [row[1] for row in records(other) if row[0] == "final"]
        assert finals == [first, "two_top.u_child.p_two"], finals
    finally:
        # Crisp gives its local worker a separate group. Kill just the two
        # groups created by this test, parent first so it cannot synthesize a result.
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=5)
        if worker_group is not None and worker_group != proc.pid:
            os.killpg(worker_group, signal.SIGKILL)
    assert proc.returncode == -signal.SIGKILL and journal.read_bytes() == before


# The child counterexample is real locally and has a published waveform.
# Its ancestor proves the property, which is the sole final CLI verdict.
run = subprocess.run([crisp, cases / "veval.salt", "up"], capture_output=True, text=True, check=True)
rows = records(progress_path(run.stdout))
prop = "up_top.p_small"
assert [row[:4] for row in rows] == [
    ["local", prop, "up_top.u_mid.u_leaf", "refuted"],
    ["local", prop, "up_top.u_mid", "proved"], ["final", prop, "", "proved"]], rows
wave = Path("crisp-out/veval_up/veval/up_top.p_small/up_top.u_mid.u_leaf.vcd")
assert str(wave) in rows[0][4] and wave.is_file()
assert rows[-1][4] == rows[1][4] and rows[-1][4] in run.stdout
assert run.stdout.endswith("1 task, 1 proved, 0 passed, 0 refuted, 0 vacuous, 0 unknown\n")

# The published VCD path includes the unusual SALT stem. Escaping keeps it
# reversible in a single five-field record, including a literal backslash-t.
stem = "escaped\\t\t\r\n"
odd = Path(stem + ".salt")
odd.write_text(f"[files]\n{cases}/veval-up.sv\n[design]\ntop up_top\n"
               "[options]\nveval only\nmode kind\ndepth 5\n")
subprocess.run([crisp, odd], capture_output=True, check=True)
directory = Path("crisp-out") / stem
row = records(next(directory.glob("progress-*.tsv")))[0]
wave = directory / "veval/up_top.p_small/up_top.u_mid.u_leaf.vcd"
encoded = str(wave).replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")
assert wave.is_file() and encoded in row[4], row

# An unusable task output directory reports the file it tried to create.
blocked = Path("blocked.salt")
blocked.write_bytes(salt.read_bytes())
Path("crisp-out/blocked").touch()
failure = subprocess.run([crisp, blocked], capture_output=True, text=True)
assert failure.returncode == 2 and "crisp-out/blocked/progress-" in failure.stderr, failure
