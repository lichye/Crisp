#!/usr/bin/env python3
"""Replay a matched snapshot of the published PLENA ledger and report."""

import argparse
import datetime as dt
from pathlib import Path
import re
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace


def legacy_refresh(source, root, ledger, report, outcomes, stamp):
    # The original script computes evidence paths from its installation path.
    origin = root / 'experiment/crisp-spec-sol2-high/plena-original-spec/update.py'
    namespace = {'__file__': str(origin), '__name__': 'legacy_update'}
    exec(compile(source.read_bytes(), str(source), 'exec'), namespace)
    frozen = dt.datetime.strptime(stamp, '%Y-%m-%d %H:%M UTC').replace(tzinfo=dt.timezone.utc)

    class FixedDatetime(dt.datetime):
        @classmethod
        def now(cls, tz=None):
            return frozen.astimezone(tz)

    namespace.update(
        LEDGER=ledger, REPORT=report, EVIDENCE=outcomes,
        dt=SimpleNamespace(datetime=FixedDatetime, timezone=dt.timezone),
    )
    namespace['main']()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path, help='Crisp tree with retained PLENA evidence')
    parser.add_argument('--ledger', required=True, type=Path, help='golden ledger TSV')
    parser.add_argument('--report', required=True, type=Path, help='matching golden report Markdown')
    parser.add_argument('--verified-outcomes', required=True, type=Path, help='approved outcomes for this snapshot')
    parser.add_argument('--scratch', required=True, type=Path, help='parent of the isolated replay directory')
    parser.add_argument('--legacy-source', type=Path, help='optional snapshot of the pre-migration generator')
    args = parser.parse_args()
    stamp, = re.findall(r'^更新于 ([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2} UTC)',
                        args.report.read_text(encoding='utf-8'), re.MULTILINE)
    root = args.root.resolve()
    with TemporaryDirectory(prefix='ledger-replay-', dir=args.scratch) as directory:
        out = Path(directory)
        ledger, report = out / args.ledger.name, out / args.report.name
        if args.legacy_source:
            shutil.copyfile(args.ledger, ledger)
            shutil.copyfile(args.report, report)
            shutil.copyfile(args.verified_outcomes, out / 'verified-outcomes.tsv')
            legacy_refresh(args.legacy_source, root, ledger, report, out, stamp)
        else:
            generator = Path(__file__).resolve().parents[2] / 'tools/plena-ledger/update.py'
            subprocess.run([
                sys.executable, str(generator), '--root', str(root), '--ledger', str(args.ledger),
                '--report', str(args.report), '--output-dir', str(out), '--stamp', stamp,
                '--verified-outcomes', str(args.verified_outcomes),
            ], check=True)
        results = []
        for expected, actual in ((args.ledger, ledger), (args.report, report)):
            status = subprocess.run(['cmp', str(expected), str(actual)], check=False).returncode
            print(f'cmp {expected.name}: {status}', flush=True)
            results.append(status)
    return max(results)


if __name__ == '__main__':
    sys.exit(main())
