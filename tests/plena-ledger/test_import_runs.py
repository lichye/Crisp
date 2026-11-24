import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/plena-ledger'))
from import_runs import (  # noqa: E402
    NEVER_TRIGGERED, PROGRESS_FIELDS, TRIGGERED, UNCHECKED, apply_annotations,
    import_directory, read_progress, validate_vacuity,
)


PROPERTY = 'plena_isa_chip_fp.s_st_ld_fp__chip_readback'


class RunImportTest(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        self.root = Path(workspace.name).resolve()
        self.run = self.root / 'run'
        self.journal = self.run / 'crisp-out/bmc/progress-example.tsv'
        self.journal.parent.mkdir(parents=True)
        self.row = {
            'verdict': 'passed', 'depth': '40', 'vacuous': '40 拍内未触发',
            'detail': 'original bounded result', 'evidence': 'earlier.tsv',
            'other_evidence': 'review.txt', 'alternative_evidence': 'fallback.txt',
        }
        self.ledger = {PROPERTY: self.row}

    def write(self, path, text):
        path.write_text(text, encoding='utf-8')

    def progress(self, *records, legacy=False):
        fields = PROGRESS_FIELDS[:-1] if legacy else PROGRESS_FIELDS
        text = '\n'.join('\t'.join(row) for row in (fields, *records)) + '\n'
        self.write(self.journal, text)

    def metadata(self, mode='bmc', veval='off'):
        journal = self.journal.relative_to(self.run)
        self.write(
            self.run / 'stdout.log',
            f'  options  mode {mode}, level word, smart off, veval {veval}, undriven seq\n'
            f'  progress  {journal}\n',
        )
        self.write(self.run / 'time.txt', 'wall_s=784.57 peak_RSS_KiB=11545928 rc=0\n')
        self.write(self.run / 'versions.txt', f"crisp 0.1.0\nCrisp {'1' * 40}\nhw-cbmc {'2' * 40}\n")

    def import_run(self):
        import_directory(self.ledger, self.run, self.root)

    def test_exact_native_vacuity_values(self):
        for native, canonical in (
            ('triggered', TRIGGERED), ('not triggered within 56', '56 拍内未触发'),
            ('never triggered', NEVER_TRIGGERED), ('unchecked', UNCHECKED),
        ):
            with self.subTest(native=native):
                self.progress(('final', PROPERTY, '', 'passed', 'bmc depth 56', native))
                event, = read_progress(self.journal)
                self.assertEqual((event.status, event.vacuous), ('passed', canonical))
                validate_vacuity(canonical, 'ledger')

    def test_non_native_vacuity_is_rejected(self):
        for value in ('', 'Triggered', ' checked ', 'unknown', 'never_triggered',
                      'not triggered within 056', 'not triggered within -1', TRIGGERED):
            with self.subTest(value=value):
                self.progress(('final', PROPERTY, '', 'proved', '', value))
                with self.assertRaisesRegex(ValueError, 'invalid Crisp vacuity'):
                    list(read_progress(self.journal))
        for value in ('never triggered', '前件已触发', '56拍内未触发', 'N 拍内未触发', ' 未查'):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'invalid ledger vacuous'):
                validate_vacuity(value, 'ledger')

    def test_historical_five_column_progress(self):
        self.progress(('final', PROPERTY, '', 'passed', 'bmc depth 56'), legacy=True)
        self.metadata()
        self.import_run()
        self.assertEqual((self.row['verdict'], self.row['depth'], self.row['vacuous']),
                         ('passed', '56', '40 拍内未触发'))

    def test_vacuous_is_not_a_verdict(self):
        self.progress(('final', PROPERTY, '', 'vacuous', 'antecedent proved unreachable', 'never triggered'))
        with self.assertRaisesRegex(ValueError, 'unsupported primary-engine status'):
            list(read_progress(self.journal))

    def test_new_bound_and_manual_fields_and_repeat(self):
        self.progress(
            ('local', PROPERTY, 'leaf', 'refuted', 'local waveform', 'triggered'),
            ('final', PROPERTY, '', 'passed', 'bmc depth 56', 'not triggered within 56'),
        )
        self.metadata()
        self.import_run()
        self.assertEqual((self.row['verdict'], self.row['depth'], self.row['vacuous']),
                         ('passed', '56', '56 拍内未触发'))
        self.assertEqual(self.row['evidence'], str(self.journal.relative_to(self.root)))
        self.assertEqual(self.row['peak_kib'], '11545928')
        self.assertEqual(self.row['other_evidence'], 'review.txt')
        self.assertEqual(self.row['alternative_evidence'], 'fallback.txt')
        first = self.row.copy()
        self.import_run()
        self.assertEqual(self.row, first)

    def test_vacuity_and_primary_verdict_merge_independently(self):
        self.progress(('final', PROPERTY, '', 'passed', 'bmc depth 30', 'never triggered'))
        self.metadata()
        self.import_run()
        self.assertEqual((self.row['verdict'], self.row['depth'], self.row['evidence']),
                         ('passed', '40', 'earlier.tsv'))
        self.assertEqual(self.row['vacuous'], NEVER_TRIGGERED)
        self.progress(('final', PROPERTY, '', 'proved', '', 'unchecked'))
        self.metadata(mode='kind')
        self.import_run()
        self.assertEqual((self.row['verdict'], self.row['depth'], self.row['vacuous']),
                         ('proved', '', NEVER_TRIGGERED))

    def test_empty_journal_and_unknown_keep_existing_verdict(self):
        first = self.row.copy()
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.progress()
            self.import_run()
            self.progress(('final', PROPERTY, '', 'unknown', 'proved; query timed out', 'unchecked'))
            self.metadata()
            self.import_run()
        self.assertEqual(self.row, first)
        self.assertIn('no final results', stderr.getvalue())
        self.assertIn('unknown', stderr.getvalue())

    def test_malformed_progress_and_multiple_finals(self):
        result = ('final', PROPERTY, '', 'passed', 'bmc depth 56', 'unchecked')
        for records, message in (
            ((result[:-1],), 'expected 6 fields'),
            ((result, result), 'multiple final records'),
            ((('final', PROPERTY, '', 'unknown', r'bad\q', 'unchecked'),), 'invalid escape'),
            ((('final', PROPERTY, '', 'refuted', 'wave.vcd', 'never triggered'),), 'must carry triggered'),
        ):
            with self.subTest(message=message):
                self.progress(*records)
                with self.assertRaisesRegex(ValueError, message):
                    list(read_progress(self.journal))
        self.progress(result)
        self.journal.write_bytes(self.journal.read_bytes().removesuffix(b'\n'))
        with self.assertRaisesRegex(ValueError, 'unfinished progress record'):
            list(read_progress(self.journal))

    def test_unknown_property_and_missing_or_local_run_metadata(self):
        self.progress(('final', PROPERTY, '', 'passed', 'bmc depth 56', 'unchecked'))
        with self.assertRaisesRegex(ValueError, 'stdout.log'):
            self.import_run()
        self.metadata(veval='only')
        with self.assertRaisesRegex(ValueError, 'expected one matching metadata record'):
            self.import_run()
        self.metadata()
        self.progress(('final', 'local_copy', '', 'proved', '', 'unchecked'))
        with self.assertRaisesRegex(ValueError, 'absent from the PLENA ledger'):
            self.import_run()

    def test_certified_counterexample_requires_matching_waveform(self):
        self.metadata()
        wave = self.journal.parent / f'{PROPERTY}.vcd'
        self.progress(('final', PROPERTY, '', 'refuted', str(wave.relative_to(self.run)), 'triggered'))
        with self.assertRaisesRegex(ValueError, 'expected an evidence file'):
            self.import_run()
        wave.touch()
        self.import_run()
        self.assertEqual(self.row['verdict'], 'refuted')
        self.assertEqual(self.row['waveform'], str(wave.relative_to(self.root)))
        self.assertEqual(self.row['vacuous'], TRIGGERED)
        self.progress(('final', PROPERTY, '', 'proved', '', 'triggered'))
        self.metadata(mode='pdr')
        with self.assertRaisesRegex(ValueError, 'conflicting proof and counterexample'):
            self.import_run()

    def test_bounded_run_does_not_replace_unbounded_proof(self):
        self.row.update(verdict='proved', depth='', vacuous=NEVER_TRIGGERED)
        first = self.row.copy()
        self.progress(('final', PROPERTY, '', 'passed', 'bmc depth 56', 'not triggered within 56'))
        self.metadata()
        self.import_run()
        self.assertEqual(self.row, first)

    def test_annotations_allow_notes_paths_and_canonical_vacuity(self):
        (self.root / 'audit.md').touch()
        annotations = self.root / 'annotations.tsv'
        self.write(annotations, 'property\tother_evidence\talternative_evidence\tdetail\tvacuous\n'
                   f'{PROPERTY}\taudit.md\t\tReviewed against original inputs\t{TRIGGERED}\n')
        apply_annotations(self.ledger, annotations, self.root)
        self.assertEqual(self.row['other_evidence'], 'audit.md')
        self.assertEqual(self.row['alternative_evidence'], 'fallback.txt')
        self.assertEqual(self.row['detail'], 'Reviewed against original inputs')
        self.assertEqual(self.row['vacuous'], TRIGGERED)
        self.assertEqual(self.row['verdict'], 'passed')
        self.write(annotations, f'property\tvacuous\n{PROPERTY}\t{NEVER_TRIGGERED}\n')
        with self.assertRaisesRegex(ValueError, 'conflicting vacuity'):
            apply_annotations(self.ledger, annotations, self.root)

    def test_annotations_reject_noncanonical_values_and_verdicts(self):
        annotations = self.root / 'annotations.tsv'
        self.write(annotations, f'property\tvacuous\n{PROPERTY}\tnever triggered\n')
        with self.assertRaisesRegex(ValueError, 'invalid ledger vacuous'):
            apply_annotations(self.ledger, annotations, self.root)
        self.write(annotations, f'property\tverdict\n{PROPERTY}\tvacuous\n')
        with self.assertRaisesRegex(ValueError, 'supported annotation columns'):
            apply_annotations(self.ledger, annotations, self.root)


if __name__ == '__main__':
    unittest.main()
