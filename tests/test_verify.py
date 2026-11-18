import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import verify
from proofs import bind_predicate


class VerifyTests(unittest.TestCase):
    def verify_token(self, directory, spec, *options):
        args = ["verify", str(ROOT / "examples/token/rtl/token.sv"), "--top", "token_formal",
                "--spec", str(spec), "--out", str(directory), *options]
        bundled = ROOT / "components/smartVerilog/.deps/oss-cad-suite/bin"
        default = bundled if (bundled / "yosys-config").is_file() else Path.home() / ".local/oss-cad-suite/bin"
        suite = os.environ.get("OSS_CAD_BIN") or str(default)
        path = os.pathsep.join([suite, os.environ["PATH"], str(Path.home() / ".local/spot/bin")])
        env = {"OSS_CAD_BIN": suite, "PATH": path}
        with patch.object(sys, "argv", args), patch.dict(os.environ, env), contextlib.redirect_stdout(io.StringIO()):
            code = verify.main()
        return code, json.loads((directory / "report.json").read_text())

    def test_temporal_and_wide_predicates_are_rejected(self):
        widths = {"u_dut.a": 1, "u_dut.b": 4}
        for expression in ("(a |=> a)", "($past(a) == a)", "(b == a)", "a || a", "missing"):
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                bind_predicate(expression, widths)

    def test_false_candidate_cannot_enter_the_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "out"
            record = {"status": "success", "candidates": ["left"]}
            with patch.object(verify, "mine", return_value=record):
                code, report = self.verify_token(directory, ROOT / "examples/token/spec.sv")
            check, = report["properties"][0]["checks"]
            candidate, = check["candidates"]
            self.assertEqual((code, check["verdict"], candidate["verdict"]), (3, "unknown", "refuted"))
            self.assertEqual(check["lemmas_used"], [])
            self.assertNotIn("retry", check)
            self.assertTrue((directory / candidate["proof"]).with_name("trace.vcd").is_file())

    def test_whole_dut_preserves_input_assertion_and_authored_assumption(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            wrapper = (ROOT / "examples/token/spec.sv").read_text()
            spec = path / "spec.sv"
            spec.write_text(wrapper.replace("endmodule", """
    a_step: assume property (@(posedge clk) step);
    p_no_sample: assert property (@(posedge clk) !sample);
endmodule"""))
            code, report = self.verify_token(path / "out", spec, "--split", "off", "--lemmas", "off")
            checks = {p["source"]: p["checks"][0] for p in report["properties"]}
            conflict = checks["token_formal.p_no_conflict"]
            sample = checks["token_formal.p_no_sample"]
            self.assertEqual((code, report["source_count"], conflict["verdict"], sample["verdict"]),
                             (1, 2, "unknown", "refuted"))
            for check in (conflict, sample):
                initial = json.loads((path / "out" / check["initial"]).read_text())
                self.assertEqual([p["source_entry"] for p in initial["premises"]], ["token_formal.a_step"])
            self.assertTrue((path / "out" / sample["initial"]).with_name("trace.vcd").is_file())


if __name__ == "__main__":
    unittest.main()
