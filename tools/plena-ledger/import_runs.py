"""Import original-model PLENA results from Crisp run evidence."""

import csv
from dataclasses import dataclass
from pathlib import Path
import re
import sys


TRIGGERED = '前件已见触发'
NEVER_TRIGGERED = '已证永不触发'
UNCHECKED = '未查'
NUMBER = r'(0|[1-9][0-9]*)'
PROGRESS_FIELDS = ['stage', 'property', 'instance', 'status', 'detail', 'vacuity']
SETTLED = {'proved', 'refuted'}
RANK = {'pending': 0, 'passed': 1, 'proved': 2, 'refuted': 2}
MANUAL_FIELDS = {
    'vacuous', 'other_evidence', 'alternative_evidence', 'detail',
    'next_action', 'eta_utc', 'work_status',
}
ESCAPES = {'\\': '\\', 't': '\t', 'r': '\r', 'n': '\n'}


def bounded_vacuity_depth(value):
    match = re.fullmatch(NUMBER + ' 拍内未触发', value)
    return int(match[1]) if match else None


def validate_vacuity(value, context):
    if value not in (TRIGGERED, NEVER_TRIGGERED, UNCHECKED) and bounded_vacuity_depth(value) is None:
        raise ValueError(f'{context}: invalid ledger vacuous value {value!r}')


def decode_vacuity(value, context):
    native = {'triggered': TRIGGERED, 'never triggered': NEVER_TRIGGERED, 'unchecked': UNCHECKED}
    if value in native:
        return native[value]
    match = re.fullmatch('not triggered within ' + NUMBER, value)
    if match:
        return f'{match[1]} 拍内未触发'
    raise ValueError(f'{context}: invalid Crisp vacuity value {value!r}')


def merge_vacuity(previous, candidate, context):
    if {previous, candidate} == {TRIGGERED, NEVER_TRIGGERED}:
        raise ValueError(f'{context}: conflicting vacuity evidence')
    if candidate == UNCHECKED or previous in (TRIGGERED, NEVER_TRIGGERED):
        return previous
    earlier = bounded_vacuity_depth(previous)
    later = bounded_vacuity_depth(candidate)
    if earlier is not None and later is not None and earlier > later:
        return previous
    return candidate


@dataclass(frozen=True)
class Final:
    property: str
    status: str
    detail: str
    vacuous: str


def decode_field(value, context):
    def decode(match):
        symbol = match[1]
        if symbol not in ESCAPES:
            raise ValueError(f'{context}: invalid escape in Crisp progress field {value!r}')
        return ESCAPES[symbol]

    return re.sub(r'\\(.|$)', decode, value)


def read_progress(path):
    """Read Crisp's escaped TSV. The older five-column journal has no vacuity field."""
    final = {}
    with path.open(encoding='utf-8', newline='') as source:
        header = next(source, '')
        fields = header.removesuffix('\n').removesuffix('\r').split('\t')
        if not header.endswith('\n') or fields not in (PROGRESS_FIELDS, PROGRESS_FIELDS[:-1]):
            raise ValueError(f'{path}: unexpected Crisp progress header')
        for number, line in enumerate(source, 2):
            context = f'{path}:{number}'
            if not line.endswith('\n'):
                raise ValueError(f'{context}: unfinished progress record')
            row = line.removesuffix('\n').removesuffix('\r').split('\t')
            if len(row) != len(fields):
                raise ValueError(f'{context}: expected {len(fields)} fields, got {len(row)}')
            stage, prop, instance, status, detail = row[:5]
            if len(fields) == 6:
                prop = decode_field(prop, context)
                detail = decode_field(detail, context)
                vacuous = decode_vacuity(row[5], context)
            else:
                vacuous = UNCHECKED
            if stage == 'local':
                continue
            if stage != 'final' or instance or not prop:
                raise ValueError(f'{context}: invalid final record')
            if status not in {'proved', 'passed', 'refuted', 'unknown', 'unlowered'}:
                raise ValueError(f'{context}: unsupported primary-engine status {status!r}')
            if len(fields) == 6 and status == 'refuted' and vacuous != TRIGGERED:
                raise ValueError(f'{context}: a Crisp counterexample must carry triggered vacuity')
            if prop in final:
                raise ValueError(f'{context}: multiple final records for {prop}')
            final[prop] = Final(prop, status, detail, vacuous)
    return final.values()


def require_file(path, root):
    path = path.resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError(f'{path}: expected an evidence file within {root}')
    return path


def read_one_match(path, pattern):
    with path.open(encoding='utf-8') as source:
        matches = [match.groups() for line in source if (match := re.fullmatch(pattern, line.rstrip('\n')))]
    if len(matches) != 1:
        raise ValueError(f'{path}: expected one matching metadata record')
    return matches[0]


def run_metadata(directory, root, journal):
    log = require_file(directory / 'stdout.log', root)
    mode, level = read_one_match(
        log, r'  options  mode ([a-z0-9-]+), level ([a-z0-9-]+), smart off, veval off, .+',
    )
    journal_in_log, = read_one_match(log, r'  progress  (.+)')
    if (directory / journal_in_log).resolve() != journal:
        raise ValueError(f'{log}: reported progress path differs from {journal}')
    versions = require_file(directory / 'versions.txt', root)
    crisp, = read_one_match(versions, r'Crisp ([0-9a-f]{40})')
    ebmc, = read_one_match(versions, r'hw-cbmc ([0-9a-f]{40})')
    timing = require_file(directory / 'time.txt', root)
    wall, peak, _ = read_one_match(timing, r'wall_s=([0-9]+(?:\.[0-9]+)?) peak_RSS_KiB=([0-9]+) rc=([0-9]+)')
    return mode, dict(engine=f'Crisp / EBMC {mode} ({level})', wall_s=wall, peak_kib=peak,
                      resource_scope='运行 GNU time 峰值 RSS（KiB）', crisp_sha=crisp, ebmc_sha=ebmc)


def outcome_fields(final, mode, directory, journal, root):
    depth = ''
    waveform = ''
    if final.status == 'passed':
        bound = re.fullmatch('bmc depth ' + NUMBER, final.detail)
        if mode != 'bmc' or not bound:
            raise ValueError(f'{journal}: {final.property}: missing primary BMC depth')
        depth = bound[1]
    if final.status == 'proved' and mode == 'bmc':
        raise ValueError(f'{journal}: {final.property}: unexpected unbounded verdict from BMC')
    if final.status == 'refuted':
        expected_name = final.property.replace('%', '%25').replace('/', '%2F') + '.vcd'
        trace = (directory / final.detail).resolve()
        if trace != journal.parent / expected_name:
            raise ValueError(f'{journal}: {final.property}: missing certified waveform path')
        waveform = str(require_file(trace, root).relative_to(root))
    return dict(verdict=final.status, depth=depth, detail=final.detail,
                evidence=str(journal.relative_to(root)), waveform=waveform)


def import_directory(rows, directory, root):
    directory = directory.resolve()
    if not directory.is_relative_to(root) or not directory.is_dir():
        raise ValueError(f'{directory}: expected a run evidence directory within {root}')
    journals = sorted([*directory.glob('progress-*.tsv'), *directory.glob('crisp-out/*/progress-*.tsv')])
    if len(journals) != 1:
        raise ValueError(f'{directory}: expected one progress TSV, found {len(journals)}')
    journal = require_file(journals[0], root)
    final_records = list(read_progress(journal))
    if not final_records:
        print(f'{journal}: no final results', file=sys.stderr)
        return
    mode, metadata = run_metadata(directory, root, journal)
    for final in final_records:
        context = f'{journal}: {final.property}'
        if final.property not in rows:
            raise ValueError(f'{context}: property is absent from the PLENA ledger')
        row = rows[final.property]
        if final.status in ('unknown', 'unlowered'):
            print(f'{context}: {final.status}; preserving the ledger verdict', file=sys.stderr)
        else:
            fields = outcome_fields(final, mode, directory, journal, root)
            current = row['verdict']
            if {current, final.status} == SETTLED:
                raise ValueError(f'{context}: conflicting proof and counterexample')
            if RANK[final.status] > RANK[current] or (
                current == final.status == 'passed' and int(fields['depth']) > int(row['depth'])
            ):
                row.update(fields, **metadata)
        row['vacuous'] = merge_vacuity(row['vacuous'], final.vacuous, context)


def apply_annotations(rows, path, root):
    seen = set()
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader(source, delimiter='\t', strict=True)
        fields = reader.fieldnames or []
        if 'property' not in fields or len(set(fields)) != len(fields) or set(fields) - MANUAL_FIELDS - {'property'}:
            raise ValueError(f'{path}: expected property and supported annotation columns')
        for annotated in reader:
            context = f'{path}:{reader.line_num}'
            if None in annotated or None in annotated.values():
                raise ValueError(f'{context}: annotation row does not match its header')
            prop = annotated.pop('property')
            if prop not in rows or prop in seen:
                raise ValueError(f'{context}: unknown or repeated property {prop!r}')
            seen.add(prop)
            row = rows[prop]
            for field, value in annotated.items():
                if not value:
                    continue
                if field in ('other_evidence', 'alternative_evidence'):
                    value = str(require_file(root / value, root).relative_to(root))
                if field == 'vacuous':
                    validate_vacuity(value, context)
                    value = merge_vacuity(row[field], value, context)
                row[field] = value
