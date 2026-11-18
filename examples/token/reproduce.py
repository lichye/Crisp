"""Run all four Crisp modes on the same token property and record the comparison."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


EXAMPLE = Path(__file__).resolve().parent
ROOT = EXAMPLE.parents[1]
MODES = [("whole", "off", "off"), ("split", "on", "off"),
         ("split-lemmas", "on", "on"), ("whole-lemmas", "off", "on")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="new comparison directory (default crisp-out/token.*)")
    args = parser.parse_args()
    if args.out:
        out = args.out.resolve()
        if out.exists():
            parser.error(f"output already exists: {out}")
        out.mkdir(parents=True)
    else:
        base = ROOT / "crisp-out"
        base.mkdir(exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="token.", dir=base))

    rows = []
    passed = True
    for name, split, lemmas in MODES:
        directory = out / name
        command = [ROOT / "crisp", "verify", EXAMPLE / "rtl/token.sv", "--spec", EXAMPLE / "spec.sv",
                   "--top", "token_formal", "--split", split, "--lemmas", lemmas,
                   "--budget", "60", "--depth", "5", "--out", directory]
        run = subprocess.run(command, check=False)
        path = directory / "report.json"
        if not path.is_file():
            return run.returncode or 2
        report = json.loads(path.read_text())
        checks = [c for p in report["properties"] for c in p["checks"]]
        proved = report["sources_with_all_selected_checks_proved"]
        rows.append({"mode": name, "split": split, "lemmas": lemmas, "budget_s": report["budget_s"],
                     "depth": report["depth"], "elapsed_s": report["elapsed_s"],
                     "source_count": report["source_count"], "sources_with_all_selected_checks_proved": proved,
                     "report": str(path.relative_to(out))})
        expected = "proved" if lemmas == "on" else "unknown"
        expected_instance = "token_formal.dut.state" if split == "on" else "token_formal.dut"
        ok = (report["status"] == "finished" and report["source_count"] == 1 and len(checks) == 1
              and checks[0]["target"]["instance"] == expected_instance and checks[0]["verdict"] == expected
              and proved == int(lemmas == "on") and run.returncode == (0 if lemmas == "on" else 3))
        if ok and lemmas == "on":
            check = checks[0]
            initial = json.loads((directory / check["initial"]).read_text())
            proofs = [json.loads((directory / receipt).read_text()) for receipt in check["lemmas_used"]]
            retry = json.loads((directory / check["retry"]).read_text())
            ok = (initial["verdict"] == "unknown" and proofs and retry["verdict"] == "proved"
                  and retry["model_sha256"] == initial["model_sha256"]
                  and all(p["role"] == "assert" and p["verdict"] == "proved"
                          and p["model_sha256"] == initial["model_sha256"] for p in proofs))
        if not ok:
            print(f"{name}: expected {expected} at {expected_instance}, see {path}", file=sys.stderr)
            passed = False
    summary = out / "summary.json"
    summary.write_text(json.dumps(rows, indent=2) + "\n")
    print(summary)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
