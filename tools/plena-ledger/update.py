#!/usr/bin/env python3
"""Generate the PLENA-specific 87-property ledger and daily report.

Run an import with --root CRISP --evidence-dir RUN --output-dir PREVIEW.
Each selected original-model run provides one progress-*.tsv either in RUN
or in RUN/crisp-out/TASK, plus stdout.log, versions.txt and time.txt in RUN.
The log records the journal path and options, versions.txt records the full
Crisp and hw-cbmc SHAs, and time.txt records wall_s, peak_RSS_KiB and rc.
The waveform named in a refuted final must be present alongside its journal.

Use --manual-updates TSV with a property column and any of vacuous,
other_evidence, alternative_evidence, detail, next_action, eta_utc or
work_status. Blank annotation cells leave the field as generated.
Use --verified-outcomes TSV for a batch of previously reviewed conclusions.
"""

import argparse
import csv
import datetime as dt
import re
from collections import Counter, defaultdict
from contextlib import ExitStack
from fnmatch import fnmatchcase
from hashlib import sha256
from pathlib import Path
from tempfile import NamedTemporaryFile

from import_runs import (
    NEVER_TRIGGERED, RANK, TRIGGERED, UNCHECKED, apply_annotations, import_directory, merge_vacuity, validate_vacuity,
)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, help='Crisp tree holding the PLENA inputs and retained evidence')
    parser.add_argument('--ledger', type=Path, help='baseline TSV, defaults to docs/ref/plena-original-spec.tsv')
    parser.add_argument('--report', type=Path, help='report path, defaults to ~/docs/report/crisp-plena-original-spec.md')
    parser.add_argument('--stamp', help='fixed report timestamp for a replay, YYYY-MM-DD HH:MM UTC')
    parser.add_argument('--csr-kind', '--csr-reset-kind', action='store_true',
                        help='refresh the reset_no_* and csr_c_set_* rows from retained proofs')
    parser.add_argument('--evidence-dir', type=Path, action='append', default=[],
                        help='completed, reviewed original PLENA run to import; repeat for multiple runs')
    parser.add_argument('--manual-updates', type=Path, help='optional TSV of reviewed ledger annotations')
    parser.add_argument('--verified-outcomes', type=Path, help='explicit approved outcomes TSV for refresh or replay')
    output = parser.add_mutually_exclusive_group()
    output.add_argument('--output-dir', type=Path, help='write candidate files to this existing directory')
    output.add_argument('--apply', action='store_true', help='write imported updates to the ledger and report paths')
    args = parser.parse_args()
    if args.evidence_dir or args.manual_updates:
        if args.csr_kind:
            parser.error('--csr-kind cannot be combined with imported updates')
        if not (args.output_dir or args.apply):
            parser.error('imported updates require --output-dir or --apply')
    if args.stamp:
        try:
            parsed = dt.datetime.strptime(args.stamp, '%Y-%m-%d %H:%M UTC')
            if parsed.strftime('%Y-%m-%d %H:%M UTC') != args.stamp:
                raise ValueError(args.stamp)
        except ValueError:
            parser.error('--stamp must be YYYY-MM-DD HH:MM UTC')
    return parser, args


PARSER, ARGS = arguments() if __name__ == '__main__' else (None, None)
ROOT = ARGS.root.resolve() if ARGS and ARGS.root else Path(__file__).resolve().parents[2]
REF = ROOT / 'docs/ref'
LEGACY_EVIDENCE = ROOT / 'experiment/crisp-spec-sol2-high/plena-original-spec'
NEON = REF / 'evidence/spec27/c624bdb4-fe6f8ac1-5b05cad2214d'
CHIP = ROOT / 'bench/hard/plena-rtl-chip'
SOL = ROOT / 'experiment/crisp-engineer-sol1-high'
INT_MEMORY_PDR = SOL / 'spec44-gitlink/pdr-replay/int_memory/run-fd5ef342'
INT_MEMORY_OFFICIAL = SOL / 'spec43-gitlink/pdr-replay/int_memory/run-83e914af'
PRIORITY_PDR = SOL / 'spec44-gitlink/pdr-replay/priority-34'
VECTOR_QUEUE = SOL / 'spec44-gitlink/pdr-replay/vector-quantifiers-d10/manifest.tsv'
VECTOR_RUNS = SOL / 'spec43-gitlink/pdr-replay/vector-quantifiers-d10'
R4_RUNS = SOL / 'spec43-gitlink/pdr-replay/ext-r4-bmc-d10'
R4_PROP = 'ext_20260801_spec.ext_r4_mask_op_not_issued'
R4_AUDIT = REF / 'evidence/spec27/ext-r4-mask-op-vcd-opus-audit.md'
R4_SAMPLES = ROOT / 'experiment/crisp-engineer-opus2-high/ext-r4/vcd-steps.tsv'
SPEC44 = ROOT / 'docs/spec/44-plena-original-spec-completion.md'
SPEC43_TRIAL = ROOT / 'experiment/ebmc-engineer-sol2-high/spec43-disabled-sequence/whole-audit-20g/summary.tsv'
INT_MEMORY = 'plena_isa_chip_int_memory.s_st_ld_int__chip_readback'
INT_STARTUP_GROUPS = {
    'plena_isa_chip_int_memory', 'plena_isa_chip_int_preserve_g1', 'plena_isa_chip_int_effects',
}
PROD = ROOT / 'experiment/crisp-engineer-astra2-xhigh/s21-final-evidence/final-111ac072'
OLD = ROOT / 'experiment/crisp-engineer-opus1-xhigh/spec20-chip'
HISTORY = ROOT / 'experiment/crisp-spec-astra1-xhigh/spec20-chip/crisp-vs-ebmc.tsv'
EVIDENCE = REF / 'evidence/plena-original-spec'
VERIFIED_OUTCOMES = ARGS.verified_outcomes if ARGS and ARGS.verified_outcomes else EVIDENCE / 'verified-outcomes.tsv'
PROJECT_DECISIONS = EVIDENCE / 'decisions.tsv'
DECISION7_BATCH = SOL / 'decision7-batch/verified-outcomes.tsv'
DECISION8_BATCH = SOL / 'decision8-batch/verified-outcomes.tsv'
LOCAL_PDR_QUEUE = EVIDENCE / 'local-pdr-queue.tsv'
LOCAL_PDR_ATTEMPTS = EVIDENCE / 'local-pdr-attempts.tsv'
LOCAL_PDR_STARTS = EVIDENCE / 'local-pdr-starts.tsv'
FP0_CUT_REPORT = REF / 'spec27-fp0-read-zero-cut.md'
FP0_CUT_PROGRESS = REF / 'evidence/spec27/fp0-read-zero-094d7e4d/evidence/progress-0vhNgg.tsv'
FP0_BMC = REF / 'evidence/spec27/fp0-bmc-d5-pr57'
GP0_BMC = REF / 'evidence/spec27/gp0-bmc-d5-pr57'
GP0_CUT_RUN = SOL / 'spec44-gp0-cut.75JgZ7/gp0_cut'
GP0_CUT_REPORT = REF / 'spec44-gp0-read-zero-cut.md'
GP0_PROGRAM = ROOT / 'experiment/crisp-engineer-sol2-high/s27-int-second/gp0-program'
GP0_PROGRAM_AUDIT = REF / 'evidence/spec27/gp0-program-independent-audit.md'
STORE_CUT_REPORT = REF / 'spec27-int-store-cut.md'
STORE_CUT_RUN = ROOT / 'experiment/crisp-engineer-sol2-high/s27-int-second/store-cut/evidence'
STORE_OBLIGATIONS_REPORT = REF / 'spec27-int-store-obligations.md'
STORE_OBLIGATIONS_RUN = STORE_CUT_RUN.parents[1] / 'store-obligations'
STORE_PROGRAM = STORE_CUT_RUN.parents[1] / 'store-program'
STORE_CLOSURE = ROOT / 'experiment/crisp-engineer-neon1-xhigh/spec44/store-closure/NATIVE_EBMC_CRITERIA.txt'
STORE_SCALE = REF / 'evidence/spec27/store-yices-scale-83e914af/SUMMARY.txt'
STORE_FIXED = STORE_CUT_RUN.parents[1] / 'fixed-store-68-69'
STORE_TOTALITY_TRIAGE = REF / 'ebmc-store-totality-triage.md'
STORE_SPEC16_REPORT = REF / 'ebmc-spec16-store-fixed-edge-solver.md'
INT_MEMORY_LOCAL_REPORT = REF / 'spec44-int-memory-local-obligations.md'
INT_MEMORY_LIFT_AUDIT = REF / 'evidence/spec27/s-st-ld-int-readback-opus-audit.md'
INT_MEMORY_FIRST_FILL = STORE_CUT_RUN.parents[1] / 'int-memory-first-fill'
INT_MEMORY_LOCAL_INPUTS = INT_MEMORY_FIRST_FILL.with_name('int-memory-obligations')
INT_MEMORY_LOCAL_RESULTS = INT_MEMORY_LOCAL_INPUTS / 'proof-results.tsv'
G1_PROGRAMS = SOL / 'spec44-g1-program.SZDRgZ'
G1_OBLIGATIONS = SOL / 'spec44-g1-obligations.2Ulh37'
G1_OBLIGATIONS_REPORT = REF / 'spec44-g1-obligations.md'
G1_LIFT_AUDIT = REF / 'evidence/spec27/g1-add-lui-obligations-audit.md'
SUB_OBLIGATIONS = STORE_CUT_RUN.parents[1] / 'sub-obligations'
ADDI_OBLIGATIONS = SUB_OBLIGATIONS.with_name('addi-obligations')
ADDI_LIFT_AUDIT = REF / 'evidence/spec27/g2-addi-obligations-audit.md'
ADDI_OPUS_AUDIT = REF / 'evidence/spec27/g2-addi-obligations-opus-audit.md'
LOOP_START_OBLIGATIONS = SUB_OBLIGATIONS.with_name('loop-start-obligations')
LOOP_START_LIFT_AUDIT = REF / 'evidence/spec27/g2-loop-start-obligations-audit.md'
LOOP_END_OBLIGATIONS = SUB_OBLIGATIONS.with_name('loop-end-obligations')
LOOP_END_LIFT_AUDIT = REF / 'evidence/spec27/g2-loop-end-obligations-opus-audit.md'
LOOP_END_LAYOUT_AUDIT = REF / 'evidence/spec27/g2-loop-end-layout-candidate-opus.md'
LOAD_OBLIGATIONS = SUB_OBLIGATIONS.with_name('load-obligations')
LOAD_LIFT_AUDIT = REF / 'evidence/spec27/s-ld-int-obligations-opus-audit.md'
ADD_FORWARD_AUDIT = REF / 'evidence/spec27/s-add-int-dependent-forwarding-opus-audit.md'
ADD_FORWARD_RESULTS = SUB_OBLIGATIONS.with_name('add-forward-obligations')
ADD_FORWARD_LIFT_AUDIT = REF / 'evidence/spec27/add-forward-obligations-opus-audit.md'
ADD_INT_CHIP_AUDIT = REF / 'evidence/spec27/s-add-int-result-chip-refuted-opus.md'
ADD_INT_CHIP_RUN = ROOT / 'experiment/crisp-engineer-opus2-high/s-add-int-chip'
SCALAR_GP0_SIX_AUDIT = REF / 'evidence/spec27/scalar-int-gp0-forward-six-opus.md'
SCALAR_GP0_SIX_RUN = ADD_INT_CHIP_RUN.with_name('s-int-gp0')
SCALAR_LIFT_FINAL = REF / 'evidence/spec27/scalar-three-lift-final-review-opus.md'
SCALAR_LIFT_INDEPENDENT = REF / 'evidence/spec27/scalar-three-split-independent-review-sol1.md'
SCALAR_CHIP_WITNESS = REF / 'evidence/spec27/scalar-three-chip-witness-opus.md'
SCALAR_LOOP_END_WITNESS = REF / 'evidence/spec27/c-loop-end-result-skid-witness-opus.md'
COMP_ADDR_WITNESS = REF / 'evidence/spec27/comp-addr-split-review-and-witness-opus.md'
G2_LOOP_END_WITNESS = REF / 'evidence/spec27/g2-loop-end-other-register-chip-witness-opus.md'
ADDR2_FIRST_FILL = SUB_OBLIGATIONS.with_name('addr2-first-fill')
ADDR2_OBLIGATIONS = SUB_OBLIGATIONS.with_name('addr2-obligations')
ADDR2_LIFT_AUDIT = REF / 'evidence/spec27/addr2-obligations-opus-audit.md'
PASS_ADDR2_AUDIT = REF / 'evidence/spec27/pass-addr-2-chip-reads-opus-audit.md'
COMP_ADDR2_AUDIT = REF / 'evidence/spec27/comp-addr-2-address-data-opus-audit.md'
VECTOR_SUB_VV = 'plena_isa_chip_vector_fp12.v_sub_vv__chip_result_2'
VECTOR_SUB_REPLAY = SOL / 'spec44-vector-sub-vv-replay'
VECTOR_SUB_AUDIT = REF / 'evidence/spec27/v-sub-vv-replay-neon-audit.md'
VECTOR_SUB_VF_AUDIT = REF / 'evidence/spec27/v-sub-vf-chip-result-replay-opus.md'
VECTOR_SUB_VF_RUNS = ROOT / 'experiment/crisp-engineer-opus2-high'
VECTOR_ADD_MUL_RUNS = SOL / 'spec44-vector-add-mul-replay'
VECTOR_ADD_MUL_REPORT = REF / 'spec44-vector-add-mul-top-replay.md'
VECTOR_ADD_MUL_CASES = (
    ('add-vv', '1', '82', '84'),
    ('add-vf', '1', '82', '84'),
    ('mul-vv', '3', '80', '82'),
    ('mul-vf', '3', '80', '82'),
)
VECTOR_SWEEP_AUDIT = REF / 'evidence/spec27/vector-six-operand-sweep-opus.md'
VECTOR_SWEEP_RUN = VECTOR_SUB_VF_RUNS / 'vec-sweep'
VECTOR_SWEEP_CASES = (
    'v_add_vv__chip_result_1', 'v_add_vf__chip_result_1', 'v_mul_vv__chip_result_3',
    'v_mul_vf__chip_result_3', 'v_sub_vf__chip_result_2', 'v_sub_vf__chip_result_9',
)
VECTOR_RECI = 'plena_isa_chip_vector_fp12.v_reci_v__chip_result_5'
RECI_LEAF_AUDIT = REF / 'evidence/spec27/v-reci-v-undriven-audit-neon.md'
RECI_LEAF = ROOT / 'experiment/crisp-engineer-neon1-xhigh/spec44/reci-leaf-diag/out'
RECI_CHIP_D72 = RECI_LEAF.parent.with_name('reci-chip-d72') / 'out'
RECI_CHIP_D72_AUDIT = REF / 'evidence/spec27/v-reci-v-chip-d72-run-neon.md'
RECI_FIXED_D72 = SOL / 'spec44-vector-reci-fixed-d72'
RECI_UNDRIVEN_AUDIT = REF / 'evidence/spec27/v-reci-v-undriven-refuted-opus.md'
RECI_UNDRIVEN_RUN = ROOT / 'experiment/crisp-engineer-opus2-high/reci-undriven-chip'
MUL_OBLIGATIONS = SUB_OBLIGATIONS.with_name('mul-obligations')
MUL_LIFT_AUDIT = REF / 'evidence/spec27/g2-mul-obligations-audit.md'
SUB_PROGRAM = SUB_OBLIGATIONS.with_name('sub-program')
SUB_LIFT_AUDIT = REF / 'evidence/spec27/g2-sub-obligations-audit.md'
FIRST_FILL = SUB_OBLIGATIONS.with_name('first-fill')
FIRST_FILL_STORE_SUB_AUDIT = REF / 'evidence/spec27/first-fill-store-sub-audit.md'
FIRST_FILL_G1 = FIRST_FILL.with_name('first-fill-g1')
FIRST_FILL_G2 = FIRST_FILL.with_name('first-fill-g2')
FIRST_FILL_LOOP_START = FIRST_FILL.with_name('first-fill-loop-start')
FIRST_FILL_LOOP_END = FIRST_FILL.with_name('first-fill-loop-end')
FIRST_FILL_LOOP_END_A = FIRST_FILL.with_name('first-fill-loop-end-a')
FIRST_FILL_MUL = FIRST_FILL.with_name('first-fill-mul')
FIRST_FILL_LOAD = FIRST_FILL.with_name('first-fill-load')
FIRST_FILL_ADD_FORWARD = FIRST_FILL.with_name('first-fill-add-forward')
EXP_REPEAT_REPORT = REF / 'ebmc-spec12-abortable-repeat.md'
EXP_SPOT_WINDOW = ROOT / 'experiment/ebmc-engineer-sol2-high/spec14-fp-exp-spot/frozen-prototype'
EXP_SPOT_RELEASE = REF / 'ebmc-spec14-fp-exp-spot.md'
FP_EXP_ISOLATION_REPORT = REF / 'spec44-fp-exp-isolation.md'
FP_EXP_ISOLATION_RUN = SOL / 'spec44-fp-exp-isolation.aiOCkK'
FP_EXP_INDUCTION_REPORT = REF / 'spec44-fp-exp-induction.md'
FP_EXP_INDUCTION_RUN = SOL / 'spec44-fp-exp-induction.k5cDBC'
FP_EXP_MIN_REPORT = REF / 'spec44-fp-exp-min-control.md'
FP_EXP_DECOMPOSITION = REF / 'spec44-fp-exp-control-decomposition.md'
FP_EXP_C21 = SOL / 'spec44-fp-exp-c21/c21/stdout.txt'
FP_EXP_MIN_RUN = SOL / 'spec44-fp-exp-min-control.powb3F/fp_exp_minimum'
FP_PRESERVE_PREFIX = SUB_OBLIGATIONS.with_name('fp-preserve-prefix')
FP_PRESERVE_RECEIVED = SUB_OBLIGATIONS.with_name('fp-preserve-prefix-received')
FP_PRESERVE_FP_STALL = SUB_OBLIGATIONS.with_name('fp-preserve-prefix-fp-stall')
FP_PRESERVE_MATRIX_CW = SUB_OBLIGATIONS.with_name('fp-preserve-prefix-matrix-cw')
FP_PRESERVE_WRITE_REDUCTION = SUB_OBLIGATIONS.with_name('fp-preserve-prefix-write-reduction')
FP_PRESERVE_PREFIX_AUDIT = REF / 'evidence/spec27/fp-preserve-d24-witness-opus-audit.md'
FP_PRESERVE_RECEIVED_AUDIT = REF / 'evidence/spec27/fp-preserve-d24-received-witness-opus-audit.md'
FP_PRESERVE_MATRIX_CW_AUDIT = REF / 'evidence/spec27/fp-preserve-d24-matrix-cw-witness-opus-audit.md'
FP_PRESERVE_WRITE_REDUCTION_AUDIT = REF / 'evidence/spec27/fp-write-request-reduction-gating-opus-audit.md'
FP_PRESERVE_LONG_PDR = SUB_OBLIGATIONS.with_name('fp-preserve-prefix-unbounded') / 'common-pdr-1800'
FP_MAP_READBACK_LONG_PDR = SUB_OBLIGATIONS.with_name('fp-map-readback-bad') / 'pdr-1800'
FP_GATED_PREFIX = SUB_OBLIGATIONS.with_name('fp-gate-ag')
FP_STALE_DRAIN_AUDIT = REF / 'evidence/spec27/fp-stale-drain-lemma-opus.md'
LOOP_ZERO_PROGRAM = STORE_CUT_RUN.parents[1] / 'loop-zero-program'
LOOP_ZERO_REPORT = REF / 'spec44-loop-zero-program.md'
LOOP_ZERO_ALIGNED = LOOP_ZERO_PROGRAM / 'loop-back-refill-gap/window.tsv'
LOOP_ZERO_REFUTED = 'plena_isa_chip_int_effects.c_loop_end__chip_zero_stays_zero'
LOOP_ZERO_REFUTED_AUDIT = REF / 'evidence/spec27/loop-zero-ext-cut-chip-refuted-opus.md'
LOOP_ZERO_REFUTED_RUN = ROOT / 'experiment/crisp-engineer-opus2-high/loop-zero-ext/chip-replay-nostall2'
SPEC27_LIFT_REVIEW = EVIDENCE / 'spec27-lift-review.tsv'
SPEC27_LIFT_REPORT = REF / 'spec27-local-proof-lift-review.md'
SCALAR_WINDOWS = EVIDENCE / 'scalar-readback-window.tsv'
CHIP_REFRESH = REF / 'evidence/spec44-chip'
CSR_RESET_CONTRAST = CHIP_REFRESH / 'csr-2cycle-reset-contrast'
CSR_KIND_RUN = SOL / 'csr-reset-unbounded-20261123'
CSR_STABLE_KIND = SOL / 'csr-stable-unbounded-071da178/outcomes.tsv'
CHIP_SPECIAL = ROOT / 'experiment/crisp-engineer-sol2-high/s44'
FP_WINDOWS = CHIP_SPECIAL / 'fp-window-review/windows.tsv'
FP_WINDOWS_REPORT = REF / 'spec44-fp-antecedent-windows.md'
INT_COVER_QUEUE = CHIP_SPECIAL / 'int-covers/plan.tsv'
INT_COMMON_COVER = CHIP_SPECIAL / 'int-covers/runs/common/try-001/result.tsv'
INT_COVER_PAUSE = CHIP_SPECIAL / 'int-covers/pause.tsv'
INT_WINDOWS = CHIP_SPECIAL / 'int-covers/windows.tsv'
INT_WINDOWS_REPORT = REF / 'spec44-int-antecedent-windows.md'
INT_G2_WINDOWS = CHIP_SPECIAL / 'int-g2-interface-review/windows.tsv'
INT_G2_REPORT = REF / 'spec44-int-g2-interface-antecedents.md'
VECTOR_EXP = 'plena_isa_chip_vector_fp12.v_exp_v__chip_result_4'
VECTOR_EXP_CUT_REPORT = REF / 'spec44-vector-exp-local-safety.md'
VECTOR_EXP_G5 = SOL / 'spec44-vector-exp-cut/g5-word/g5.jstXZJmg'
VECTOR_EXP_TOP_REPORT = REF / 'spec44-vector-exp-top-replay.md'
VECTOR_EXP_TOP_AUDIT = REF / 'evidence/spec27/v-exp-v-chip-result-4-wiring-audit.md'
VECTOR_EXP_TOP_EXACT = SOL / 'spec44-vector-exp-top-g5'
VECTOR_EXP_TOP_RESULTS = VECTOR_EXP_TOP_EXACT / 'results.tsv'
VECTOR_REDUCTION_MASKED = SOL / 'spec44-vector-reduction-masked'
VECTOR_REDUCTION_MASKED_REVIEW = VECTOR_REDUCTION_MASKED / 'independent-review.tsv'
VECTOR_REDUCTION_MASKED_REPORT = REF / 'spec44-vector-reduction-masked-replay.md'
VECTOR_SPEC13_EXP = ROOT / 'experiment/ebmc-engineer-sol1-high/spec13-yices-packed-past/chip-exp'
VECTOR_SPEC13_GATE = VECTOR_SPEC13_EXP.with_name('release-final') / 'log/audit.log'
VECTOR_SPEC13_REBASED_GATE = VECTOR_SPEC13_EXP.with_name('release-rebased') / 'log/audit.log'
VECTOR_SPEC13_SUM = VECTOR_SPEC13_EXP.with_name('chip-sum')
VECTOR_SPEC13_MAX = VECTOR_SPEC13_EXP.with_name('chip-max')
VECTOR_SPEC13_RELEASE = REF / 'ebmc-spec13-yices-packed-past.md'
VECTOR_SPEC13_REBASE = VECTOR_SPEC13_EXP.with_name('log') / 'rebase-range-diff'
VECTOR_LATE_QUANTIFIER = {
    VECTOR_EXP,
    'plena_isa_chip_vector_fp12.v_red_sum__chip_reduction_result',
    'plena_isa_chip_vector_fp12.v_red_max__chip_reduction_result',
}
KINDS_RESET_CASES = {
    'plena_kinds_chip_l1_spec.pm_prefetch_valid_reset',
    'plena_kinds_chip_l1_spec.pv_prefetch_valid_reset',
}
KINDS_ACTIVE = ROOT / (
    'experiment/crisp-engineer-neon1-xhigh/spec44-chip/runs/'
    '8c9c1135-fe6f8ac1-63d083f9f46e/plena_kinds_chip_l1_spec__rem2'
)
KINDS_ARCHIVE = CHIP_REFRESH / '8c9c1135-fe6f8ac1-63d083f9f46e/logs/interrupted'
KINDS_INTERRUPTED = KINDS_ARCHIVE.parents[1] / 'interrupted.tsv'
TAIL_EVIDENCE = KINDS_INTERRUPTED.parent / 'tail-evidence'
KINDS_TAIL = TAIL_EVIDENCE / 'kinds_l1_rem2'
TAIL_CONSOLIDATION = TAIL_EVIDENCE / 'tail_witness_consolidation.tsv'
PM_PDR_TRIAL = REF / 'evidence/spec27/pr55-83e914af-pm-prefetch-short-trial'
PM_CONE_REPORT = REF / 'spec44-pm-reset-cone.md'
PM_CONE_ARCHIVE = EVIDENCE / 'pm-reset-cone-094d7e4d'
PM_LASSO_TRIAL = ROOT / 'experiment/crisp-engineer-neon1-xhigh/spec27/pm-lasso-diag'
PM_LASSO_ARCHIVE = REF / 'evidence/spec27/pm-lasso8-diag-83e914af/SUMMARY.txt'
RST_LASSO = REF / 'evidence/spec27/pm-lasso1-b0-83e914af'
RST_LASSO_RUN = PM_LASSO_TRIAL.with_name('pm-lasso1-b0-diag')
PWRITE_LASSO = REF / 'evidence/spec27/pwrite-ready-lasso1-b0-83e914af'
PWRITE_LASSO_RUN = PM_LASSO_TRIAL.with_name('pwrite-ready-lasso1-b0')
EXT_R8_LASSO = REF / 'evidence/spec27/ext-r8-avalid-lasso1-b0-83e914af'
EXT_R8_LASSO_RUN = PM_LASSO_TRIAL.with_name('ext-r8-avalid-lasso1-b0')
EXT_ATOMIC_PROOF = LEGACY_EVIDENCE / 'ext-r8-atomic-proof'
EXT_ATOMIC_REPORT = REF / 'spec44-ext-r8-atomic-safety.md'
EXT_ATOMIC_TIMEOUT = REF / 'evidence/spec27/ext-r8-accept-lasso2-b0-83e914af/SUMMARY.txt'
EXT_ATOMIC_LOOP1_TIMEOUT = REF / 'evidence/spec27/ext-r8-accept-loop1-replay-83e914af/SUMMARY.txt'
EXT_ATOMIC_BMC = REF / 'evidence/spec27/ext-r8-accept-bmc-d2-pr57'
PV_CONE_RUN = LEGACY_EVIDENCE / 'pv-reset-cone'
PV_CONE_REPORT = REF / 'spec44-pv-prefetch-reset-cone.md'
PV_CONE_AUDIT = REF / 'evidence/spec27/pv-prefetch-reset-cone-audit.md'
SCALAR_STARTUP = 'plena_isa_chip_scalar'
ADD_INT_CHIP = SCALAR_STARTUP + '.s_add_int__chip_result'
FLOAT_BATCH = 'plena_isa_chip_fp'
FLOAT_ZERO = FLOAT_BATCH + '.fp0__chip_zero'
FLOAT_MAP = FLOAT_BATCH + '.s_map_v_fp__chip_first_element'
FLOAT_READBACK = FLOAT_BATCH + '.s_st_ld_fp__chip_readback'
FLOAT_MAX_PRESERVE = 'plena_isa_chip_fp_preserve.s_max_fp__chip_other_register'
FLOAT_MAX_RESULT = FLOAT_BATCH + '.s_max_fp__chip_result'
FLOAT_SQRT_RESULT = FLOAT_BATCH + '.s_sqrt_fp__chip_result'
FLOAT_CASE_B_PRESERVE = tuple(
    f'plena_isa_chip_fp_preserve.s_{op}_fp__chip_other_register'
    for op in ('add', 'sub', 'mul', 'exp', 'reci')
)
FLOAT_COMBINATION_RUNS = Path('experiment/crisp-engineer-opus2-high/fp16-caseB/proof/run')
FLOAT_FWD_LOGS = {
    f'{FLOAT_BATCH}.s_{op}_fp__chip_result': FLOAT_COMBINATION_RUNS / f'fp16n-eq-{op}/out.log'
    for op in ('add', 'sub', 'mul', 'exp', 'reci')
}
FLOAT_FWD_LOGS['plena_isa_chip_fp_preserve.s_sqrt_fp__chip_other_register'] = (
    FLOAT_COMBINATION_RUNS / 'fp16b-eq-sqrt/out.log'
)
FLOAT_CONSEQUENT_RUNS = Path('experiment/crisp-engineer-opus3-high/fp16/run')
FLOAT_FWD_LOGS.update({
    'plena_isa_chip_fp_preserve.s_ld_fp__chip_other_register': FLOAT_CONSEQUENT_RUNS / 'eqcons-ld/out.log',
    'plena_isa_chip_fp_preserve.s_st_fp__chip_registers_unchanged': FLOAT_CONSEQUENT_RUNS / 'eqcons-st/out.log',
})
FP_LONG_PDR_CASES = (
    ('九条 FP 保值公共 c2', FP_PRESERVE_LONG_PDR,
     tuple(f'plena_isa_chip_fp_preserve.s_{op}_fp__chip_other_register'
           for op in ('add', 'sub', 'max', 'mul', 'exp', 'reci', 'sqrt', 'ld'))
     + ('plena_isa_chip_fp_preserve.s_st_fp__chip_registers_unchanged',)),
    ('MAP_V 首元素与 FP STORE/LOAD 回读公共 t0–t5', FP_MAP_READBACK_LONG_PDR,
     (FLOAT_MAP, FLOAT_READBACK)),
)
NEON_WRAPPER_TOTALS = {
    'ext_20260801_spec': 1,
    'plena_isa_chip_int_memory': 1,
    'plena_isa_chip_int_preserve_g1': 2,
    'plena_isa_chip_int_effects': 3,
    'plena_kinds_chip_l1_spec': 2,
    'plena_isa_chip_int_preserve_g2': 5,
    'plena_isa_chip_interface_effects': 5,
    SCALAR_STARTUP: 8,
    'plena_isa_chip_fp_preserve': 9,
    FLOAT_BATCH: 10,
}
VECTOR_BITWUZLA = LEGACY_EVIDENCE / 'vector-exp-bitwuzla-fd5ef342-d27'
LEDGER = ARGS.ledger.resolve() if ARGS and ARGS.ledger else REF / 'plena-original-spec.tsv'
REPORT = (ARGS.report.resolve() if ARGS and ARGS.report
          else Path.home() / 'docs/report/crisp-plena-original-spec.md')
FULL_EBMC = 'fe6f8ac1fde706dd0b404f875e9d8ec6cb976ec7'
CRISP_40 = 'c624bdb4c9eab1f9db5a6204e89884eaeea3d568'
CRISP_48 = '111ac07219bb3a056e57c37cc66b884bacae6c25'
CSR = 'plena_csr_reset_spec'
CSR_RESET_NO = f'{CSR}.reset_no_'
CSR_SET = f'{CSR}.csr_c_set_'
RESET = {f'{CSR}.reset_{name}_stable' for name in ('hbm_addr_bank', 'scale_reg', 'stride_reg')}
LATE = {
    'plena_isa_chip_fp.s_exp_fp__chip_result': (31, 32),
    'plena_isa_chip_fp_preserve.s_exp_fp__chip_other_register': (29, 30),
    'plena_isa_chip_vector_fp12.v_exp_v__chip_result_4': (26, 27),
    'plena_isa_chip_vector_fp12.v_red_sum__chip_reduction_result': (34, 35),
    'plena_isa_chip_vector_fp12.v_red_max__chip_reduction_result': (34, 35),
}
LABELS = {
    'proved': '无界证明',
    'passed': '有界通过',
    'refuted': '反例',
    'pending': '待结算',
}
DECISION7_NOTE = '第 7 条批准（11-23）'
DEADLINE = '2026-11-27'


def rows(path):
    with path.open(encoding='utf-8', newline='') as source:
        yield from csv.DictReader(source, delimiter='\t')


def rel(path):
    path = Path(path)
    return str(path.relative_to(ROOT)) if path.is_absolute() else str(path)


def source_line(path, prop):
    label = prop.rsplit('.', 1)[1]
    pattern = re.compile(r'\b' + re.escape(label) + r'\s*:\s*assert\b')
    lines = [lineno for lineno, line in enumerate(path.read_text().splitlines(), 1) if pattern.search(line)]
    assert len(lines) == 1, (path, prop, lines)
    return lines[0]


def new_row(scope, prop, source, salt, reset):
    return dict(
        scope=scope, property=prop, source=rel(source), source_line=source_line(source, prop),
        salt=rel(salt), reset=reset, verdict='pending', depth='', vacuous=UNCHECKED, historical_cover_sample='',
        engine='', wall_s='', peak_kib='', resource_scope='', crisp_sha='', ebmc_sha='',
        detail='', evidence='', other_evidence='', waveform='', historical='',
        next_action='', eta_utc='', work_status='已排', alternative_evidence='',
    )


def measurement(verdict, engine, depth, wall, peak, basis, crisp, ebmc, detail, evidence, other=''):
    return dict(verdict=verdict, engine=engine, depth=str(depth), wall_s=str(wall), peak_kib=str(peak),
                resource_scope=basis, crisp_sha=crisp, ebmc_sha=ebmc, detail=detail,
                evidence=rel(evidence), other_evidence=rel(other) if other else '')


def final_rows(path):
    return [row for row in rows(path) if row['stage'] == 'final']


def record_pr57_bounded(row, run, depth, detail):
    summary = run / 'SUMMARY.txt'
    if not summary.is_file():
        return
    journal, = run.glob('progress-*.tsv')
    event, = final_rows(journal)
    expected = f'bmc depth {depth}'
    assert (event['property'], event['status'], event['detail']) == (row['property'], 'passed', expected), event
    cli = (run / 'stdout.log').read_text()
    match = re.search(rf"{re.escape(row['property'])}\s+passed\s+(\d+(?:\.\d+)?) s\s+{expected}\s*$", cli, re.M)
    assert match is not None and '0 vacuous, 0 unknown' in cli
    row.update(measurement(
        'passed', 'Crisp / EBMC 字级 BMC · Yices', depth, match[1],
        int((run / 'memory.peak').read_text()) // 1024, '整片单目标 cgroup 峰值',
        'b16ba9f5aa76da90e58838fb18a97af14681a666', '01f535662700fa1351b5ba0155b1280aaed805d7',
        detail, journal, summary,
    ))
    row['work_status'] = '正式有界已取证'


def spec27():
    by_property = defaultdict(list)
    for run in rows(NEON / 'results.tsv'):
        path = NEON / run['progress_tsv']
        if not path.is_file():
            continue
        for goal in final_rows(path):
            by_property[goal['property']].append((run, goal, path))
    return by_property


def chip_rows():
    manifest = REF / 'evidence/plena-chip-smt-d20/manifest_87.tsv'
    history = defaultdict(list)
    for event in rows(HISTORY):
        history[event['property']].append(event)
    old_costs = {entry['run']: entry for entry in rows(OLD / 'results.tsv')}
    production = {}
    batch = PROD / 'chip-production'
    for runtime in rows(batch / 'verified.tsv'):
        name = runtime['task']
        if name == 'plena_csr_reset_named':
            continue
        task_directory = batch / name
        [progress] = task_directory.glob('crisp-out/*/progress-*.tsv')
        for prop in final_rows(progress):
            production[prop['property']] = runtime, prop, progress
    local = spec27()
    for entry in rows(manifest):
        prop = entry['property']
        row = new_row('chip', prop, CHIP / entry['spec_file'], CHIP / entry['salt_file'],
                      '无显式复位' if entry['reset_flag'] == 'NONE' else entry['reset_flag'] + ' high')
        row['next_action'] = '在固定后端复核局部 PDR，待修复时补同版本 Crisp 的字级有界结论'
        row['eta_utc'] = '11-22 首批，11-24 补齐重跑，11-26 收口'
        observations = sorted(history[prop], key=lambda item: int(item['depth']), reverse=True)
        historic = []
        for observation in observations:
            run = observation['crisp_run']
            historic.append(f"Crisp c82f61c depth {observation['depth']} {observation['crisp_group_verdict']}")
            if observation['crisp_group_verdict'] == 'refuted':
                wave = OLD / 'runs' / run / observation['crisp_detail']
                if wave.is_file():
                    historic.append(f'旧版有限反例波形 {rel(wave)}，待用严格后端重放')
            if observation['crisp_group_verdict'] != 'passed' or row['evidence']:
                continue
            cost = old_costs[run]
            row.update(measurement(
                'pending', 'Crisp 字级 BMC（历史）', observation['depth'], cost['wall_s'],
                cost['peak_rss_kib'], '历史包装级总墙钟和进程树 RSS', 'c82f61c', '6f7a08ca',
                '有历史有界通过，待按正式 Crisp 版本复核前件和有界结论', OLD / 'runs' / run / 'out', HISTORY))
        for solver in ('yices', 'bitwuzla'):
            legacy_result = entry[f'hist_{solver}_d20_verdict']
            if legacy_result:
                historic.append(f'裸 EBMC {solver} depth 20 {legacy_result}')
        if not row['evidence']:
            row['evidence'] = rel(HISTORY)
            row['detail'] = '正式有界结论待补，历史运行见本行记录'
        if any(event['crisp_group_verdict'] == 'refuted' for event in observations):
            row['next_action'] = '优先在严格后端重放旧版反例，再决定是否接受及附波形'
            row['eta_utc'] = '11-22 首批'
        for run, outcome, path in local[prop]:
            if run['mode'] == 'whole_pdr':
                row['other_evidence'] = rel(path)
                if outcome['status'] == 'unknown':
                    historic.append('正式整设计 PDR ' + outcome['detail'])
            if run['mode'] in ('whole_pdr', 'veval_pdr') and outcome['status'] == 'proved':
                related = path.read_text()
                assert f'final\t{prop}\t' in related
                row.update(measurement(
                    'proved', 'EBMC new IC3/PDR' + (' · veval' if run['mode'] == 'veval_pdr' else ''),
                    '', run['wall_s'], run['peak_tree_kib'], '包装级总墙钟和进程树 RSS',
                    run['crisp_head'], run['ebmc_gitlink'], outcome['detail'], path, NEON / 'results.tsv'))
                row['next_action'], row['eta_utc'] = '保留原包装、局部落点和假设供核验', '已取证'
        if prop in production and row['verdict'] != 'proved':
            runtime, outcome, path = production[prop]
            v = outcome['status']
            if prop in RESET:
                assert v == 'vacuous'
                v = 'passed'
                depth = 10
                detail = 'Crisp 原模型 BMC 有界通过到 d10。单拍冷启动模型只在 #0 拉高 rst，rst ##1 rst 永不触发'
                row.update(vacuous=NEVER_TRIGGERED, next_action='保留单拍冷启动模型和原断言', eta_utc='有界已取证')
            elif prop in LATE:
                assert v == 'vacuous'
                v = 'pending'
                hit, bound = LATE[prop]
                depth = 20
                row['historical_cover_sample'] = str(hit)
                row['vacuous'] = f'{depth} 拍内未触发'
                detail = f'depth 20 vacuous。历史 cover 在第 {hit} 拍采到改写后件，现有界未覆盖'
                row['next_action'] = f'保留原 SVA 和复位，单选 BMC depth {bound}，并核对前件命中'
                row['eta_utc'] = '11-22 到 11-24'
            elif v == 'passed':
                depth = 10
                row['vacuous'] = TRIGGERED
                detail = 'Crisp depth 10，前件已达。整设计 PDR 当前不支持相应转换或转移约束'
                row['next_action'], row['eta_utc'] = '随 Spec 39/43 在固定后端重放无界', '11-24 首次复核'
            else:
                assert v == 'unknown'
                historic.append('正式仅解原断言 depth 10 passed，未核实前件')
                v = 'pending'
                depth = 10
                detail = '原断言有界通过，正式整组前件查询超时'
                row['next_action'], row['eta_utc'] = 'Spec 42 取证后单选或小组查询原前件', '11-22 到 11-24'
            row.update(measurement(v, 'Crisp 字级 BMC/Yices', depth, runtime['wall_s'],
                                   round(float(runtime['peak_MiB']) * 1024), '正式包装级进程树 RSS',
                                   CRISP_48, FULL_EBMC, detail, path, path.parents[2] / 'cli.log'))
        elif row['verdict'] == 'pending':
            if prop.startswith('plena_isa_chip_vector_fp12.'):
                if not any(obs['depth'] == '20' and obs['crisp_group_verdict'] == 'passed' for obs in observations):
                    row['next_action'] = '先补正式 Crisp depth 10，再按后端能力提高深度'
            elif prop == 'ext_20260801_spec.ext_r8_accept_atomic':
                row['next_action'] = '单选正式 Crisp/Yices depth 20，保留原假设'
        if prop == INT_MEMORY:
            fresh = INT_MEMORY_PDR / 'stdout'
            if fresh.is_file() and 'sva_non_overlapped_implication not convertible' in fresh.read_text():
                historic.append('Crisp #54 / fd5ef342 原样 PDR 10.31 秒、1651512 KiB、退出 2，SVA 转换不支持')
                row['alternative_evidence'] = rel(fresh)
                if row['verdict'] == 'pending':
                    row['next_action'] = '先补原样字级 BMC，Spec 43 发布后核对整设计无界'
        row['historical'] = ' / '.join(historic)
        yield row


def chip_observations():
    for table in sorted(CHIP_REFRESH.glob('*/per_property_results.tsv')):
        for item in rows(table):
            journal = table.parent / item['progress_tsv']
            if journal.is_file():
                yield (
                    item['property'], item['timestamp_utc'],
                    measurement(item['status'], 'Crisp 字级 BMC/Yices', item['depth'], item['wrapper_wall_s'],
                                item['wrapper_peak_tree_kib'], '正式包装级进程树 RSS',
                                item['crisp_head'], item['ebmc_gitlink'], item['detail'], journal,
                                table.parent / item['log_stdout']),
                )
    for table in sorted(CHIP_SPECIAL.glob('runs/*/try-*/result.tsv')):
        for item in rows(table):
            if not item['progress'] or not item['status']:
                continue
            yield (
                item['target'], item['ended_utc'],
                measurement(item['status'], 'Crisp 字级 BMC/Yices', item['depth'], item['wall_seconds'],
                            round(float(item['peak_tree_MiB']) * 1024), '正式单目标进程树 RSS',
                            item['crisp'], item['ebmc'], item['detail'], item['progress'], table.parent / 'stdout'),
            )


def apply_chip_observations(by_property, observed):
    for prop, _, result in observed:
        update = result.copy()
        row = by_property[prop]
        status, depth = update['verdict'], int(update['depth'])
        path = ROOT / update['evidence']
        finals = [r for r in final_rows(path) if r['property'] == prop]
        if not finals and status == 'unknown' and update['detail'].startswith('timeout after '):
            cli = ROOT / update['other_evidence']
            assert any(line.split(maxsplit=4)[:2] == [prop, status] and line.endswith(update['detail'])
                       for line in cli.read_text().splitlines()), (prop, cli)
            update['detail'] += '，CLI 为 unknown，增量 TSV 无 final'
            row['alternative_evidence'] = rel(cli)
        else:
            assert any(r['status'] == status for r in finals), (prop, path)
        if status == 'passed':
            row['vacuous'] = TRIGGERED
        elif status == 'vacuous' and row['vacuous'] not in (TRIGGERED, NEVER_TRIGGERED):
            previous = re.match(r'(\d+) 拍内未触发', row['vacuous'])
            if previous is None or depth >= int(previous[1]):
                row['vacuous'] = f'{depth} 拍内未触发'
        if row['verdict'] in ('proved', 'refuted') or prop in RESET:
            continue
        if prop in LATE and depth < LATE[prop][1]:
            continue
        if status == 'passed':
            if row['verdict'] == 'passed' and depth < int(row['depth']):
                continue
            update['detail'] += '。无界待新后端重放，原版整设计 PDR 拒绝约束或 SVA 转换'
            row.update(update)
            row['next_action'] = '保留当前有界结论，使用新后端重放无界'
            row['eta_utc'] = '11-22 首轮重放'
            continue
        if row['verdict'] != 'pending':
            continue
        update['verdict'] = 'pending'
        if status == 'vacuous':
            if prop in LATE:
                update['detail'] = f'正式 depth {depth} 仍为 vacuous，待解释历史局部 cover 与整芯片前提的差异'
                row['next_action'] = '用整芯片原前件重放并对照历史局部 cover 的约束和拍序'
                if prop == 'plena_isa_chip_fp_preserve.s_exp_fp__chip_other_register':
                    context = REF / 'spec44-preserve-witness-context.md'
                    if context.is_file():
                        update['detail'] += '。历史局部见证在第 1 拍需要 v_s_out_valid=1，整芯片冷启动同拍只能为 0'
                        row['alternative_evidence'] = rel(context)
                        row['next_action'] = '原局部见证已排除，继续核对完整前件在原整芯片上的可达界'
            else:
                update['detail'] = f'正式 depth {depth} vacuous，待核对前件为何不可达'
                row['next_action'] = '核对前件、复位和可达时间，确定可解释的 vacuous 结论'
                if prop == INT_MEMORY:
                    update['detail'] = f'正式 depth {depth} vacuous，原 store/load 触发序列在该界内未命中'
                    row['next_action'] = '核对更深界的原前件，原样整设计 PDR 单独重放'
                elif prop.split('.', 1)[0] in INT_STARTUP_GROUPS:
                    update['detail'] = f'正式 depth {depth} vacuous，冷启动后原多拍指令序列前件未命中'
                    row['next_action'] = '核对原指令序列的最早可能到达界，并单查完整前件'
        elif status == 'refuted':
            update['detail'] = '正式反例待重放和波形归档。' + update['detail']
            row['next_action'] = '核验见证并归档对应波形'
        else:
            update['detail'] = '正式补测尚无结论。' + update['detail']
            if prop == FLOAT_ZERO and 'vacuity check timed out' in update['detail']:
                row['next_action'] = '独立核对原完整前件的递进界 cover 和完整 d20 原断言'
                row['work_status'] = '正式 d20 前件超时，独立取证中'
            output = ROOT / update['other_evidence']
            stderr = output.with_name('stderr') if output.name == 'stdout' else output.with_suffix('.stderr')
            if stderr.is_file() and 'quantifiers are not allowed in logic QF_AUFBV' in stderr.read_text(errors='replace'):
                update['detail'] += '。Yices 拒绝 QF_AUFBV 中的量词'
                update['other_evidence'] = rel(stderr)
                row['next_action'] = '与 EBMC 核对量词支持路径，在固定版本上重跑'
                if prop in VECTOR_LATE_QUANTIFIER:
                    update['detail'] += '，来源是历史不足时两维 packed 向量的 $past 默认全零数组 array_of.299'
        row.update(update)

def vector_crosscheck(by_property):
    start = VECTOR_BITWUZLA / 'assertion/start.tsv'
    if not start.is_file():
        return
    row = by_property[VECTOR_EXP]
    versions = VECTOR_BITWUZLA / 'VERSIONS.txt'
    row['alternative_evidence'] = rel(versions)
    result_table = VECTOR_BITWUZLA / 'outcomes.tsv'
    if not result_table.is_file():
        if row['verdict'] == 'pending':
            row['work_status'] = 'Bitwuzla 运行中'
            row['next_action'] = '已启动原样整芯片 fd5ef342/Bitwuzla depth 27，等原断言及前件证据'
            row['eta_utc'] = '11-21 19:56 前首轮原断言'
        return
    results = {r['phase']: r for r in rows(result_table)}
    assertion = results['assertion']
    row['alternative_evidence'] = rel(result_table)
    if row['verdict'] != 'pending':
        return
    antecedent = results.get('antecedent')
    verdict = assertion['result']
    row['historical'] += ' / 裸 EBMC fd5ef342/Bitwuzla depth 27 ' + verdict
    if 'PROVED up to bound 27' in verdict:
        row['next_action'] = '核对同配置的前件命中，记录 Crisp 当前 Yices 限制'
    elif 'REFUTED' in verdict:
        row['next_action'] = '重放整芯片前件与后件，核验波形及后端见证'
    elif verdict == 'timeout':
        row['work_status'] = 'Bitwuzla 首轮超时'
        row['detail'] += f"。裸 EBMC/Bitwuzla 原样 depth 27 到限，实耗 {assertion['wall_s']} 秒，树峰值 {assertion['peak_tree_kib']} KiB"
        row['next_action'] = '先核对同一整芯片原前件的可达性，再排后件复验'
        row['eta_utc'] = '11-22 复核界限与求解器'
    else:
        row['next_action'] = '核对 Bitwuzla 停留阶段与 EBMC 量词诊断，安排后继补测'
    if antecedent:
        row['historical'] += ' / 同界前件 cover ' + antecedent['result']

def priority_pdr_queue(by_property):
    path = PRIORITY_PDR / 'wrapper-queue.tsv'
    if not path.is_file():
        return
    for job in rows(path):
        for prop in job['properties'].split(','):
            row = by_property[prop]
            if row['verdict'] not in ('pending', 'passed'):
                continue
            if job['state'].startswith('running'):
                row['work_status'] = '整设计 PDR 运行中'
                row['next_action'] += '。fd5ef342 整设计 PDR 已启动'
            elif job['state'].startswith('interrupted'):
                timeout_path = PRIORITY_PDR / 'runs/001-ext_20260801_spec/attempt-status.tsv'
                row['work_status'] = 'PDR 让出内存，待重跑'
                row['next_action'] += '。整设计 PDR 在共享内存达到预算前停止，待新尝试'
                if timeout_path.is_file():
                    row['alternative_evidence'] = rel(timeout_path)
            elif job['state'] == 'queued':
                row['next_action'] += '。已排 fd5ef342 整设计 PDR'
            if not row['alternative_evidence']:
                row['alternative_evidence'] = rel(path)

def prepared_vector_queue(by_property):
    if not VECTOR_QUEUE.is_file():
        return
    results = VECTOR_RUNS / 'results.tsv'
    completed = {item['property']: item for item in rows(results)} if results.is_file() else {}
    for entry in rows(VECTOR_QUEUE):
        row = by_property['plena_isa_chip_vector_fp12.' + entry['property']]
        if row['verdict'] != 'pending':
            continue
        result = completed.get(entry['property'])
        if result and result['status'] == 'vacuous (antecedent not reached)':
            run = VECTOR_RUNS / 'runs' / result['progress']
            journal, = run.glob('crisp-out/*/progress-*.tsv')
            assert any(item['property'] == row['property'] and item['status'] == 'vacuous'
                       for item in final_rows(journal)), journal
            row.update(measurement(
                'pending', 'Crisp 字级 BMC/Yices', result['depth'], result['elapsed_s'], result['peak_rss_KiB'],
                '单属性 GNU time 最大 RSS', '2a6f83ff6cec0a7c4ffede1c5eb208b6721961cd',
                '83e914af6d26da76b84559d1665cede1efc44e99',
                '正式 depth 10 vacuous，原前件在该界内未匹配。待核对有效触发界和原模型证据', journal, run / 'stdout',
            ))
            row['next_action'] = '核对原前件有效窗口，待第 5 条决定或有成本更低的取证方法'
            row['work_status'] = 'd10 vacuous，完整前件待取证'
            row['alternative_evidence'] = rel(results)
            continue
        row['next_action'] = f"原样单条 depth {entry['depth']} 已备好，待正式 PDR 让出资源"
        row['eta_utc'] = '11-22 首轮'
        if not row['alternative_evidence']:
            row['alternative_evidence'] = rel(VECTOR_QUEUE)


def record_r4_interrupt(by_property):
    attempts = R4_RUNS / 'attempts.tsv'
    if not attempts.is_file():
        return
    row = by_property[R4_PROP]
    if row['verdict'] != 'pending':
        return
    last = next((item for item in rows(attempts) if item['attempt'].startswith('010-')), None)
    if last and last['status'].startswith('OOM-killed solver'):
        row['detail'] += (
            f"。按原无复位字级 d10 单查，{last['elapsed_s']} 秒后 Yices 被内核 OOM 终止，"
            f"systemd 留存峰值 {int(last['peak_cgroup_bytes']):,} 字节，TSV 无 final"
        )
        kernel_oom = R4_RUNS / ('attempt-' + last['attempt']) / 'kernel-oom.txt'
        if kernel_oom.is_file() and 'usage 23068672kB, limit 23068672kB' in kernel_oom.read_text():
            row['detail'] += '。内核日志记录触及该单元的 22 GiB 硬上限'
        row['next_action'] = '保留历史反例候选，待更有效的严格见证复验方案'
        row['work_status'] = '原配置复验中断，证据待取'
        row['alternative_evidence'] = rel(attempts)


def record_r4_vcd(by_property, choices):
    if choices.get('6') != 'a' or not R4_AUDIT.is_file() or not R4_SAMPLES.is_file():
        return
    assert any(f'`{R4_PROP.split(".", 1)[1]}`' in line and '按原模型收为有限反例' in line
               for line in SPEC44.read_text().splitlines())
    row = by_property[R4_PROP]
    assert row['reset'] == '无显式复位'
    assert not re.search(r'(?m)^\s*reset\s', (ROOT / row['salt']).read_text())
    old_run = OLD / 'runs/ext_20260801_spec.d10'
    old_salt = OLD / 'ext_20260801_spec.d10.salt'
    assert not re.search(r'(?m)^\s*reset\s', old_salt.read_text())
    log = old_run / 'out'
    refuted = re.search(rf'(?m)^\s+{re.escape(R4_PROP)}\s+refuted\s+(\d+\.\d+) s\s+(\S+\.vcd)$', log.read_text())
    assert refuted
    waveform = old_run / refuted[2]
    assert sha256(waveform.read_bytes()).hexdigest() == '89967f5e6e4a0a1877e3688ba3264aee4b51698e30fde8b558c72752c65afa21'
    samples = {entry['signal']: (entry['step0'], entry['step1']) for entry in rows(R4_SAMPLES)}
    assert samples['rst'] == ('0', '0') and samples['issued_c_valid'] == ('0', '1')
    assert samples['issued_c_op'][1] == '0x2e' and samples['u_dut.decoder_init.decode_dispatch_fire'][1] == '1'
    assert samples['u_dut.decoder_init.active_decode_instruction_type'][1] == '0x5'
    assert samples['u_dut.decoder_init.stall_for_read_rd'][1] == '0'
    assumptions = re.findall(r'\b(\w+)\s*:\s*assume\s+property', (ROOT / row['source']).read_text())
    assert assumptions == ['ext_r8_no_unsolicited_first']
    attempt, = (item for item in rows(R4_RUNS / 'attempts.tsv') if item['attempt'].startswith('010-'))
    assert attempt['status'].startswith('OOM-killed solver')
    strict_progress, = (R4_RUNS / ('attempt-' + attempt['attempt'])).glob('crisp-out/*/progress-*.tsv')
    assert not list(final_rows(strict_progress))
    detail = (
        '按 Spec 44 第 6 条 a，以 Crisp c82f61c 在原无显式复位、任意初态模型的历史整片 VCD 收为有限反例。'
        '两个样本 rst 均为 0，唯一原假设 $rose(rst) |=> !instr_mem_tl_d_valid_i 成立。'
        'step1 issued_c_valid=1、issued_c_op=0x2e，命中禁止发射的原条件。'
        f"现版原配置严格重放在 {attempt['elapsed_s']} 秒被 OOM 终止，"
        f"记录峰值 {int(attempt['peak_cgroup_bytes']):,} B，未得 final，严格无限延拓未取得。"
    )
    row.update(measurement('refuted', 'Crisp c82f61c · 整片字级 BMC 历史波形', 10, refuted[1], '',
                           '', 'c82f61c', '', detail, log, strict_progress))
    row.update(historical_cover_sample='1', waveform=rel(waveform), alternative_evidence=rel(R4_AUDIT),
               work_status='按原无显式复位模型收账', eta_utc='已结算', next_action='保留历史有限反例和严格重放 OOM 记录')


def record_spec43_trial(by_property):
    if not SPEC43_TRIAL.is_file():
        return
    trial = {r['field']: r['value'] for r in rows(SPEC43_TRIAL)}
    assert trial['ic3'] == 'property check entered' and trial['oom_kill'] == '0'
    row = by_property[INT_MEMORY]
    note = (
        f"EBMC Spec 43 候选 {trial['run_revision'][:8]} 原样 PDR 已进入 new IC3。"
        f"{trial['timeout_s']} 秒限时下用时 {trial['wall_s']} 秒，峰值 RSS {trial['max_rss_kib']} KiB，"
        f"退出 {trial['rc']}，无 OOM，无性质判定"
    )
    row['historical'] += ' / ' + note
    if row['verdict'] in ('pending', 'passed'):
        row['detail'] += '。' + note
        row['next_action'] = '核对正式 PDR 结果，并单查更深界的原前件'
        row['alternative_evidence'] = rel(SPEC43_TRIAL)
        if (INT_MEMORY_OFFICIAL / 'start-utc.txt').is_file() and not (INT_MEMORY_OFFICIAL / 'returncode.txt').is_file():
            row['work_status'] = '正式整设计 PDR 运行中'

def official_int_memory():
    end = INT_MEMORY_OFFICIAL / 'returncode.txt'
    if not end.is_file():
        return None
    journals = list(INT_MEMORY_OFFICIAL.glob('crisp-out/*/progress-*.tsv'))
    if len(journals) != 1:
        return None
    [journal] = journals
    outcomes = [row for row in final_rows(journal) if row['property'] == INT_MEMORY]
    totals = (INT_MEMORY_OFFICIAL / 'time.txt').read_text().splitlines()
    if not totals or len(totals[-1].split()) != 3:
        return None
    wall, peak, _ = totals[-1].split()
    cli = INT_MEMORY_OFFICIAL / 'stdout'
    target = re.escape(INT_MEMORY)
    reported = re.search(rf'^ +{target} +(\w+) +(\d+\.\d+) s(?: +(.*))?$', cli.read_text(), re.M)
    if reported:
        wall = reported[2]
    status = outcomes[0]['status'] if outcomes else '无判定'
    detail = outcomes[0]['detail'] if outcomes else '增量 TSV 无 final'
    if not outcomes and reported:
        status = f'CLI {reported[1]}'
        detail = f'{reported[3] or ""}，增量 TSV 无 final'
    memory_after = INT_MEMORY_OFFICIAL / 'memory-events-after.txt'
    if memory_after.is_file() and re.search(r'^oom_kill 0$', memory_after.read_text(), re.M):
        detail += '，无 OOM kill'
    return dict(verdict=status, detail=detail, wall=wall, peak=peak,
                exit=end.read_text().strip(), journal=journal, cli=cli)

def record_official_int_memory(by_property, replay):
    if replay is None:
        return
    row = by_property[INT_MEMORY]
    note = (
        f"正式 Crisp #55 / 83e914af 整设计 PDR 为 {replay['verdict']}，实耗 {replay['wall']} 秒，"
        f"峰值 RSS {replay['peak']} KiB，退出 {replay['exit']}。{replay['detail']}"
    )
    row['historical'] += ' / ' + note
    if replay['verdict'] == 'proved' and replay['exit'] == '0':
        row.update(measurement(
            'proved', 'Crisp / EBMC new IC3/PDR', '', replay['wall'], replay['peak'],
            '正式整设计 PDR GNU time 峰值 RSS', '2a6f83ff6cec0a7c4ffede1c5eb208b6721961cd',
            '83e914af6d26da76b84559d1665cede1efc44e99', replay['detail'],
            replay['journal'], INT_MEMORY_OFFICIAL / 'command.txt'))
        row['next_action'], row['eta_utc'] = '保留正式原样无界证明证据', '已取证'
        row['work_status'] = '已取证'
    elif row['verdict'] in ('pending', 'passed'):
        row['detail'] += '。' + note
        row['work_status'] = '正式 PDR 已结束，待收账'
        row['next_action'] = '复核正式 PDR 诊断，按深界 cover 进度安排原式补测'
    row['alternative_evidence'] = rel(replay['journal'] if replay['verdict'] == 'proved' else replay['cli'])


def int_cover_timeouts():
    if not INT_COVER_QUEUE.is_file():
        return {}
    completed = {}
    for item in rows(INT_COVER_QUEUE):
        if item['case'] == 'common':
            continue
        runs = INT_COVER_QUEUE.parent / 'runs' / item['case']
        latest = max(runs.glob('try-*/result.tsv'), default=None)
        if latest is None:
            continue
        result = next(rows(latest))
        assert (result['cover'], result['depth']) == (item['cover'], item['depth']), latest
        if result['stopped'] == 'cover timeout':
            completed[item['assertion']] = (result, latest)
    return completed


def record_int_windows(by_property, common_cover, cover_timeouts):
    if not INT_WINDOWS.is_file():
        return
    for item in rows(INT_WINDOWS):
        row = by_property[item['assertion']]
        if row['verdict'] != 'pending' or row['depth'] != '20' or 'vacuous' not in row['detail']:
            continue
        start = item['min_post_reset_pass']
        last = item['max_pass_d20_antecedent']
        lower = item['first_possible_new_pass_lower_bound']
        match = item['first_possible_match_lower_bound']
        row['detail'] += f'。深度 20 要求首个 PASS_ADDR 在 #{start}–{last}，原 RTL 的保守下界为 #{lower}，完整前件不早于 #{match}'
        if common_cover:
            row['detail'] += '。公共前缀 d44 查询超时，深界可达性仍待取证'
        if row['property'] != INT_MEMORY:
            row['next_action'] = '排队单查原完整前件 d48，保留已解释的 d20 vacuous'
            row['alternative_evidence'] = rel(INT_WINDOWS_REPORT)
            row['work_status'] = '有界 vacuous 已解释，深界已排'
        if row['property'] in cover_timeouts:
            result, evidence = cover_timeouts[row['property']]
            row['detail'] += (
                f"。完整前件 d{result['depth']} 查询 {result['wall_seconds']} 秒超时，"
                f"进程树峰值 {result['peak_tree_MiB']} MiB，未得到 SAT 或 UNSAT"
            )
            row['next_action'] = '收集余项深界结果，按同一原配置安排后续取证'
            row['work_status'] = 'd20 vacuous 已解释，完整前件深界查询超时'
            if row['property'] != INT_MEMORY:
                row['alternative_evidence'] = rel(evidence)
    if INT_COVER_PAUSE.is_file():
        planned = {item['case']: item['assertion'] for item in rows(INT_COVER_QUEUE)}
        for item in rows(INT_COVER_PAUSE):
            row = by_property[planned[item['case']]]
            if row['verdict'] != 'pending':
                continue
            in_flight = item['state'] == 'existing attempt preserved'
            row['detail'] += ('。d48 已暂停，刚启动的查询没有判定' if in_flight else
                              '。d48 前件查询在启动前暂停')
            row['next_action'] = '等第 5 条决策或更有效的同模型查询方案'
            row['eta_utc'] = '暂停'
            row['work_status'] = '深界前件查询暂停'
            if row['property'] != INT_MEMORY:
                row['alternative_evidence'] = rel(INT_COVER_PAUSE)
    if INT_G2_WINDOWS.is_file():
        for item in rows(INT_G2_WINDOWS):
            row = by_property[item['assertion']]
            if row['verdict'] != 'pending' or row['depth'] != '20' or 'vacuous' not in row['detail']:
                continue
            start = item['first_pass_min']
            last = item['d20_first_pass_max']
            match = item['first_antecedent_lower_bound']
            row['detail'] += f'。首个 PASS_ADDR 须在 #{start}–{last}，原 RTL 保守下界为 #33，完整前件不早于 #{match}'
            row['next_action'] = (
                '参考六条 d48 查询，再单查本条操作与索引的完整前件'
                if common_cover else '公共 d44 取证后，再单查本条操作与索引的完整前件'
            )
            row['alternative_evidence'] = rel(INT_G2_REPORT)
            row['work_status'] = '有界 vacuous 已解释，深界待排'
    if SCALAR_WINDOWS.is_file():
        for item in rows(SCALAR_WINDOWS):
            row = by_property[item['property']]
            if row['verdict'] != 'pending' or row['depth'] != '20' or 'vacuous' not in row['detail']:
                continue
            first = item['first_possible_readback_during_d20']
            bound = item['rtl_readback_lower_bound']
            row['detail'] += f'。前件末拍必须以 PASS_ADDR 回读，深度 20 要求落在 #{first}–20，原接线下的保守下界为 #{bound}'
            row['next_action'] = '按实际启动窗口安排完整前件 cover 和回读后两拍'
            row['alternative_evidence'] = rel(SCALAR_WINDOWS)
            row['work_status'] = '有界 vacuous 已解释，深界待排'

def record_fp_windows(by_property):
    if not FP_WINDOWS.is_file():
        return
    for item in rows(FP_WINDOWS):
        row = by_property[item['property']]
        if row['verdict'] != 'pending' or 'vacuous' not in row['detail']:
            continue
        end, consequent = int(item['antecedent_end_min']), int(item['consequent_min'])
        if end > 20:
            row['detail'] += f'。源式最短前件到第 {end} 拍，后件在第 {consequent} 拍，两者均超出深度 20'
            row['work_status'] = 'd20 vacuous 由长度解释，深界仍待取证'
        elif row['depth'] == '20':
            row['detail'] += (
                f"。语法允许首个执行级 LD_OUT 在 #{item['first_ld_out_min']}–{item['ld_out_max_d20']}。"
                '新取指产生的码保守不早于 #37，执行级旧值和停顿恢复仍须按原模型核对'
            )
            row['work_status'] = 'd20 vacuous，整机路径原因待证'
            row['next_action'] = '保留执行级复位及停顿恢复，取原完整前件的整机证据'
        if not row['alternative_evidence']:
            row['alternative_evidence'] = rel(FP_WINDOWS_REPORT)

def record_kinds_handoff(by_property):
    if not KINDS_ACTIVE.is_dir():
        return
    audit = KINDS_TAIL / 'SUMMARY.txt'
    assertions = KINDS_TAIL / 'orig_d20.stdout.tail.txt'
    cover = KINDS_TAIL / 'cov_d1.stdout.tail.txt'
    for prop in KINDS_RESET_CASES:
        row = by_property[prop]
        if row['verdict'] != 'pending':
            continue
        row['next_action'] = '移到整芯片 BMC 队尾，原模型单复核 rst 前件'
        row['eta_utc'] = '其余五包完成后'
        row['work_status'] = '后移，保留原前件求解证据'
        if KINDS_INTERRUPTED.is_file():
            row['detail'] += '。本轮 d20 的前件查询经人工中断，增量 TSV 没有最终判定'
            row['alternative_evidence'] = rel(KINDS_INTERRUPTED)
        if all(path.is_file() for path in (audit, assertions, cover)):
            assert any(line.startswith(f'[{prop}]') and line.endswith('PROVED up to bound 20')
                       for line in assertions.read_text().splitlines()), prop
            row.update(measurement(
                'pending', 'EBMC 字级 BMC/Yices · 独立原断言', 20, 48.73, 4131240,
                '双目标 BMC，GNU time 最大 RSS', '', 'fe6f8ac1fde706dd0b404f875e9d8ec6cb976ec7',
                '原断言深度 20 无反例。原模型 d1 波形已证 rst 在第 0 拍为 1，两路 valid 在第 1 拍为 0。'
                '非 vacuous 判定仍需同一前缀能延伸到第 20 拍的证据', assertions, cover,
            ))
            row['alternative_evidence'] = rel(audit)
            row['next_action'] = '按原无显式复位配置，在深度 20 验证第 0 拍 rst 前缀的延伸'
            row['work_status'] = 'd20 原断言已通过，待同界前件见证'
            row['eta_utc'] = '队尾资源释放后，首轮限时 300 秒'
            if (KINDS_TAIL / 'cov_d20.time').is_file() and 'result=TIMEOUT_300s' in audit.read_text():
                row['detail'] += '。定点 d20 Yices 查询在 300.45 秒超时，峰值 4,128,200 KiB'
                row['next_action'] = '用同模型字级 Z3 续查定点 d20 见证'


def record_short_tail_witnesses(by_property):
    cases = (
        (SCALAR_STARTUP + '.gp0__chip_zero', 'scalar_gp0', '前件第 1 拍匹配，第 3 拍核对后件'),
        (FLOAT_ZERO, 'fp_fp0', '前件第 3–4 拍匹配，第 5 拍核对后件'),
    )
    for prop, case, matching in cases:
        bundle = TAIL_EVIDENCE / case
        audit, assertions = bundle / 'SUMMARY.txt', bundle / 'orig_d20.stdout.tail.txt'
        row = by_property[prop]
        if row['verdict'] != 'pending' or not audit.is_file() or not assertions.is_file():
            continue
        assert any(line.startswith(f'[{prop}]') and line.endswith('PROVED up to bound 20')
                   for line in assertions.read_text().splitlines()), prop
        row['detail'] += f'。独立 EBMC 查询确认原断言 d20 无反例，原完整前件的 d5 波形中{matching}。待同模型 d20 见证'
        row['alternative_evidence'] = rel(audit)
        row['next_action'] = '保留原冷启动复位，字级 Z3 查询原前件匹配后延伸到第 20 拍'
        row['work_status'] = 'd20 原断言已通过，待同界前件见证'
        row['eta_utc'] = '队尾资源释放后，首轮限时 300 秒'

    atomic = by_property['ext_20260801_spec.ext_r8_accept_atomic']
    if atomic['verdict'] == 'pending' and 'vacuity check timed out' in atomic['detail']:
        atomic['next_action'] = '原无显式复位，带原时序假设在 d20 单查 A 通道握手前件'


def record_tail_closeout(by_property):
    if not TAIL_CONSOLIDATION.is_file():
        return
    outcomes = {
        '#12_kinds_l1_rem2': 'd20 的 Yices、固定输入复放、Z3、Bitwuzla 均超时，AIG 触发 16 GiB 护栏',
        '#11_scalar_gp0': '原前件定点 d20 Z3 查询在 300.65 秒超时',
        '#10_fp_fp0': '原前件定点 d20 Z3 查询在 300.65 秒超时',
        '#01_ext_r8_atomic': '带原时序假设的原断言 d20 无反例。握手前件的同界 Z3 查询于 122.02 秒触发 20 GiB 护栏',
    }
    for item in rows(TAIL_CONSOLIDATION):
        group = item['group_slot']
        if group not in outcomes:
            continue
        assert item['standalone_orig_d20_yices'].startswith('PROVED_up_to_bound_20'), item
        for prop in item['properties'].split(','):
            row = by_property[prop]
            if row['verdict'] != 'pending':
                continue
            row['detail'] += '。' + outcomes[group]
            row['alternative_evidence'] = rel(item['summary_path'])
            row['work_status'] = 'd20 原断言已通过，待同界前件见证'
            row['next_action'] = '保留前件证据，等待同模型的新取证路径'
            row['eta_utc'] = '待下一路线'
            if prop == 'plena_kinds_chip_l1_spec.pm_prefetch_valid_reset':
                row['next_action'] = '以 #55 对原 veval 落点做一轮短时 PDR 复核'
                row['eta_utc'] = '11-22 夜班短试，限 120 秒'


def record_pm_pdr_trial(by_property):
    summary = PM_PDR_TRIAL / 'VERSIONS_AND_RESULT.txt'
    if not summary.is_file():
        return
    row = by_property['plena_kinds_chip_l1_spec.pm_prefetch_valid_reset']
    if row['verdict'] != 'pending':
        return
    journal, = PM_PDR_TRIAL.glob('progress/progress-*.tsv')
    final, = final_rows(journal)
    assert (final['property'], final['status']) == (row['property'], 'unknown'), final
    assert 'exhausted available memory' in final['detail'], final
    result = dict(line.split('=', 1) for line in summary.read_text().splitlines() if '=' in line)
    row['detail'] += (
        f"。#55 veval PDR 落在整个 u_dut，{result['wall_s']} 秒遇 8 GiB 内存限额，"
        f"cgroup 峰值 {result['cgroup_memory_peak_mib']} MiB，final unknown"
    )
    row['next_action'] = '优先核对更小复位路径，保留原 u_dut 试验证据'
    row['eta_utc'] = '待独立复核'
    if PM_CONE_REPORT.is_file():
        if (PM_CONE_ARCHIVE / 'VERSIONS.txt').is_file():
            row['detail'] += '。手工锥投影 PDR 与独立接线复核可提升无界安全性，原整片触发的无限延拓待证'
            row['work_status'] = '整片无界安全性已证，前件无限延拓待证'
            row['next_action'] = '核对原模型的总性推导或可延拓触发见证'
            row['eta_utc'] = '待独立延拓证据'
        else:
            row['detail'] += '。保留原组件与连接的复位路径 PDR 已证，待核对是否可提升到原整片性质'
        row['alternative_evidence'] = rel(PM_CONE_REPORT)
    else:
        row['alternative_evidence'] = rel(summary)

def record_split_handoffs(by_property, observed):
    if not KINDS_INTERRUPTED.is_file():
        return
    refreshed = {prop for prop, _, result in observed if CHIP_REFRESH in (ROOT / result['evidence']).parents}
    for attempt in rows(KINDS_INTERRUPTED):
        wrapper = attempt['wrapper']
        if wrapper not in (SCALAR_STARTUP, FLOAT_BATCH) or attempt['status'] != 'interrupted':
            continue
        patterns = [p.strip() for p in attempt['properties'].split(',')]
        for prop, row in by_property.items():
            if not any(fnmatchcase(prop, pattern) for pattern in patterns):
                continue
            if row['verdict'] != 'pending' or prop in refreshed:
                continue
            row['work_status'] = '原包已中断，等待拆分复测'
            row['alternative_evidence'] = rel(KINDS_INTERRUPTED)
            if wrapper == FLOAT_BATCH:
                row['detail'] += '。原十目标 d20 在主断言合查 B221 人工停止，未进入前件检查，增量 TSV 无 final'
                row['eta_utc'] = 'scalar 七条完成后排 FP'
                if prop == FLOAT_MAP:
                    row['next_action'] = '单选原 s_map_v_fp，移至队尾，首轮限时 1,800 秒'
                elif prop == FLOAT_ZERO:
                    row['next_action'] = '单选原 fp0，若前件查询阻塞则独立取完整前件和原断言证据'
                else:
                    row['next_action'] = '以原包装的八条长前件选集独立重放深度 20'
            else:
                row['detail'] += '。原八目标 d20 查询因 gp0 前件耗时人工停止，未发布 final'
                row['eta_utc'] = '按拆分后的队列'
                if prop.endswith('.gp0__chip_zero'):
                    row['next_action'] = '原整芯片条件单选 gp0，独立取完整前件和原断言证据'
                else:
                    row['next_action'] = '优先原包装单列其余七条，重放正式 d20'


def other_results(by_property):
    updated = VERIFIED_OUTCOMES
    if not updated.is_file():
        if ARGS and ARGS.verified_outcomes:
            raise ValueError(f'{updated}: expected an approved outcomes TSV')
        return
    for number, update in enumerate(rows(updated), 2):
        property_name = update['property']
        row = by_property[property_name]
        status = update['verdict']
        if status not in ('proved', 'passed', 'refuted'):
            raise ValueError(f'{updated}:{number}: invalid ledger verdict {status!r}')
        if {update['verdict'], row['verdict']} == {'proved', 'refuted'}:
            raise ValueError(f"Conflicting proof and counterexample for {property_name}")
        if 'vacuous' in update:
            context = f'{updated}:{number}: {property_name}'
            validate_vacuity(update['vacuous'], context)
            update['vacuous'] = row['vacuous'] = merge_vacuity(row['vacuous'], update['vacuous'], context)
        if RANK[status] < RANK[row['verdict']]:
            continue
        if update['verdict'] == row['verdict'] == 'passed' and int(update['depth']) < int(row['depth']):
            continue
        assert update['engine'] and update['wall_s'] and update['peak_kib'], update
        assert all((ROOT / update[key]).is_file() for key in ('evidence', 'other_evidence') if update[key]), update
        if update['verdict'] == 'refuted':
            assert update['waveform'] and (ROOT / update['waveform']).is_file(), update
        row.update((key, value) for key, value in update.items() if key in row and key != 'property')
        if row['verdict'] == 'proved':
            row['work_status'] = '整片无界已证'
        if row['verdict'] in ('proved', 'refuted'):
            row['next_action'], row['eta_utc'] = '保留结果与复验命令', '已取证'


def record_spec27_lift_review(by_property):
    if not SPEC27_LIFT_REVIEW.is_file():
        return
    for audit in rows(SPEC27_LIFT_REVIEW):
        row = by_property[audit['property']]
        assert row['verdict'] == 'proved' and row['crisp_sha'] == CRISP_40, row
        assert any(item['property'] == row['property'] and item['status'] == 'proved'
                   for item in final_rows(ROOT / row['evidence'])), row
        row['detail'] += '。独立接线复核确认整片无界安全性可提升'
        row['alternative_evidence'] = rel(SPEC27_LIFT_REPORT)
        if audit['witness_state'] == '无需见证':
            row['detail'] += '，原性质无前件'
            continue
        row['verdict'] = 'pending'
        row['detail'] += f"。原整片前件 {audit['original_premise']} 的无限延拓待证"
        row['next_action'] = audit['next_action']
        row['work_status'] = '整片无界安全性已证，前件无限延拓待证'


def apply_project_decisions(by_property):
    choices = {row['item']: row['choice'] for row in rows(PROJECT_DECISIONS)} if PROJECT_DECISIONS.is_file() else {}
    if choices.get('4') == 'a':
        for prop in RESET:
            row = by_property[prop]
            assert row['verdict'] == 'passed', row
            row['detail'] += '。仅覆盖原单拍冷启动配置，双拍保持时的稳定性另作对照'
            row['next_action'] = '保留原性质和冷启动配置。Neon 另做两拍复位对照'
            row['eta_utc'] = '已结算'
            row['work_status'] = '原模型有界已取证'
            row['alternative_evidence'] = rel(PROJECT_DECISIONS)
    if choices.get('5') == 'b':
        for row in by_property.values():
            if row['scope'] != 'chip' or row['verdict'] != 'pending':
                continue
            if row['work_status'] == '整片无界安全性已证，前件无限延拓待证':
                continue
            if row['property'] == 'ext_20260801_spec.ext_r4_mask_op_not_issued':
                row['next_action'] = '核对可提升局部落点，旧反例候选须在原模型验证'
                continue
            row['next_action'] = '按 Spec 27 核对局部落点及接口前提，排可提升的无界证明'
        if LOCAL_PDR_QUEUE.is_file():
            for item in rows(LOCAL_PDR_QUEUE):
                row = by_property[item['property']]
                if row['verdict'] != 'pending':
                    continue
                readiness = '已派发' if item['state'] == '派发' else '首条审阅后'
                row['next_action'] = (
                    f"{readiness}。核对 veval 接线与前提，PDR 本轮限 {item['timeout_s']} 秒、{item['memory_gib']} GiB"
                )
    return choices


def record_reset_contrast(by_property):
    summary = CSR_RESET_CONTRAST / 'VERSIONS_AND_RESULT.txt'
    if not summary.is_file():
        return
    journal, = CSR_RESET_CONTRAST.glob('progress/progress-*.tsv')
    outcomes = {event['property']: (event['status'], event['detail']) for event in final_rows(journal)}
    assert set(outcomes) == RESET and set(outcomes.values()) == {('passed', 'bmc depth 4')}, journal
    for prop in RESET:
        row = by_property[prop]
        row['detail'] += '。独立两拍复位对照 d4 通过，前件第 0–1 拍，后件第 2 拍'
        row['next_action'] = '保留原单拍结果和独立两拍对照'
        row['alternative_evidence'] = rel(journal)


def record_approved_bookings(by_property):
    decision7 = {row['property']: row for row in rows(DECISION7_BATCH)}
    decision8 = {row['property']: row for row in rows(DECISION8_BATCH)}
    fp_prefixes = (FLOAT_BATCH + '.', 'plena_isa_chip_fp_preserve.')
    assert len(decision7) == 26 and not decision7.keys() & decision8.keys()
    assert len(decision8) == 18 and decision8.keys() == {
        prop for prop in by_property if prop.startswith(fp_prefixes) and prop != FLOAT_ZERO
    }
    ext_atomic = 'ext_20260801_spec.ext_r8_accept_atomic'
    assert {FLOAT_ZERO, ext_atomic} <= decision7.keys()

    for prop, result in decision7.items():
        row = by_property[prop]
        assert result['verdict'] == 'proved' and not result['depth'] and row['verdict'] != 'refuted', result
        assert all(result[key] and (ROOT / result[key]).is_file()
                   for key in ('evidence', 'other_evidence', 'waveform')), result
        prior_detail = row['detail']
        if prop in (FLOAT_ZERO, ext_atomic) and row['verdict'] == 'passed':
            row['alternative_evidence'] = row['evidence']
        row.update((key, value) for key, value in result.items() if key in row and key != 'property')
        detail = result['detail'].replace('前件为有限见证，严格无限延拓证书未取得。', '').replace(
            '两态零初值整片仿真全状态 lasso，原生严格无限延拓证书未取得。', ''
        ).replace('需 Leiqi 单独确认（原 assume、无显式复位）。', '')
        if prop == ext_atomic:
            detail += '原配置无显式复位，唯一原 assume 在见证中成立。'
        elif prop == FLOAT_ZERO:
            _, separator, note = prior_detail.partition('veval 11-15 ')
            assert separator, prop
            note = note.split('。' + DECISION7_NOTE, 1)[0].replace('与本表 d5', '与本表此前 d5')
            detail += 'veval 11-15 ' + note.rstrip('。') + '。'
        elif prop == SCALAR_STARTUP + '.c_loop_end__chip_result':
            v2 = ROOT / 'experiment/crisp-engineer-opus2-high/loop-end-skid/V2_loopstart/replay.vcd'
            assert v2.is_file(), v2
            row['waveform'] = rel(v2)
            detail += '配对 LOOP_START 的 V2 经 skid 路径在 #65–#68 命中，#70 实际及参照均为 0。'
        row.update(detail=detail + DECISION7_NOTE + '。', vacuous=TRIGGERED,
                   work_status=DECISION7_NOTE, next_action='保留提升复核和原片证据', eta_utc='已结算')

    for prop, result in decision8.items():
        row = by_property[prop]
        depth = result['depth']
        assert result['verdict'] == 'vacuous' and depth.isdecimal(), result
        outcome, = (item for item in final_rows(ROOT / result['evidence']) if item['property'] == prop)
        assert (outcome['status'], outcome['detail']) == (
            'vacuous', f'antecedent not reached within depth {depth}',
        ), outcome
        if row['verdict'] in ('proved', 'refuted') or (row['verdict'] == 'passed' and int(row['depth']) > int(depth)):
            continue
        vacuous = row.get('vacuous')
        row.update((key, value) for key, value in result.items() if key in row and key != 'property')
        row.update(verdict='passed', vacuous=TRIGGERED if vacuous == TRIGGERED else f'{depth} 拍内未触发',
                   detail=f'原模型 BMC {depth} 拍内无反例。第 8 条批准（11-23）。',
                   alternative_evidence='', work_status='原模型有界已取证',
                   next_action='补原模型无界证明或反例', eta_utc='有界已取证')

    cases = {row['property']: row for row in rows(CSR_STABLE_KIND)}
    assert cases.keys() == RESET, CSR_STABLE_KIND
    for prop, result in cases.items():
        row = by_property[prop]
        prior_bmc = Path(result['previous_evidence'])
        assert (result['engine'], result['max_k'], result['unbounded_safety'], result['crisp_final'], result['detail']) == (
            'kind', '4', 'proved', 'vacuous', 'antecedent proved unreachable',
        ), result
        outcome, = (item for item in final_rows(prior_bmc) if item['property'] == prop)
        assert (outcome['status'], outcome['detail']) == (
            'vacuous', 'antecedent not reached within depth 10',
        ), outcome
        row.update(measurement(
            'proved', 'Crisp / EBMC 整设计字级 k-induction · Yices', '', result['wall_s'],
            int(result['peak_bytes']) // 1024, '三条共享运行 cgroup 峰值',
            '071da178d35f464bfbdef434fbc728705a4eb275', 'ce671bf63ade068f5e7e97da9cd72e906ac5fe85',
            '原模型 word kind k=4 证出无界安全。kind 辅助查询证出前件不可达', CSR_STABLE_KIND, prior_bmc,
        ))
        row.update(vacuous=NEVER_TRIGGERED, work_status='整设计无界已证', eta_utc='已结算')
    record_reset_contrast(by_property)


def record_retained_triggers(by_property):
    # Apart from CSR, these are the pre-booking proofs with certified original-chip antecedents.
    prefixes = (CSR_RESET_NO, CSR_SET)
    original_triggers = {
        'ext_20260801_spec.ext_r8_ready_when_requesting',
        'plena_kinds_chip_l1_spec.pv_ele_a_reset',
        'plena_kinds_chip_l1_spec.pm_scale_d_ready_reset',
        'plena_kinds_chip_l1_spec.pv_prefetch_valid_reset',
        'plena_kinds_chip_l1_spec.pm_prefetch_valid_reset',
        'plena_kinds_chip_l1_spec.pwrite_ready_scale',
    }
    for prop, row in by_property.items():
        if row['verdict'] == 'refuted' or (row['verdict'] == 'proved'
                                          and (prop.startswith(prefixes) or prop in original_triggers)):
            row['vacuous'] = TRIGGERED
        else:
            row.setdefault('vacuous', UNCHECKED)


def record_csr_kind(by_property):
    verification = (CSR_KIND_RUN / 'verification.log').read_text()
    versions = dict(re.findall(r'(?m)^(Crisp|hw-cbmc) ([0-9a-f]{40})$', verification))
    batches = (
        (CSR_RESET_NO, 11, 'reset-kind-k2', '2', 'proved; vacuity check: out of memory',
         '整设计 k-induction k=2 证出无界安全；本次 vacuous 检查 OOM，非 vacuous 依据为同 RTL 同 .salt 的 d10 前件命中（rst 为顶层输入）'),
        (CSR_SET, 10, 'csr-kind-k4', '4', 'proved; vacuity check timed out',
         '整设计 k-induction k=4 证出无界安全；本次 vacuous 检查超时，非 vacuous 依据为同 RTL 同 .salt 的 d10 运行判为 passed（非 vacuous）'),
    )
    for prefix, count, run, k, final_detail, detail in batches:
        evidence = CSR_KIND_RUN / run / 'outcomes.tsv'
        outcomes = list(rows(evidence))
        assert len(outcomes) == count and {item['property'] for item in outcomes} == {
            prop for prop in by_property if prop.startswith(prefix)
        }, evidence
        progress, = {Path(item['progress']) for item in outcomes}
        finals = {event['property']: event for event in final_rows(progress)}
        for item in outcomes:
            prop = item['property']
            original_bmc = Path(item['original_bmc'])
            prior, = (event for event in final_rows(original_bmc) if event['property'] == prop)
            assert (prior['status'], prior['detail']) == ('passed', 'bmc depth 10'), prior
            assert (item['engine'], item['k'], item['safety_proved'], item['crisp_final'], item['detail']) == (
                'kind', k, 'yes', 'unknown', final_detail,
            ), item
            assert (finals[prop]['status'], finals[prop]['detail']) == ('unknown', final_detail), progress
            row = by_property[prop]
            assert (row['verdict'], row['depth'], row['evidence']) in (
                ('passed', '10', rel(original_bmc)), ('proved', '', rel(evidence)),
            ), row
            row.update(measurement(
                'proved', 'Crisp / EBMC 字级 k-induction · Yices', '', item['wall_s'],
                int(item['memory_peak_bytes']) // 1024, f'{count} 条共享运行 cgroup 峰值',
                versions['Crisp'], versions['hw-cbmc'], detail, evidence, original_bmc,
            ))
            row.update(vacuous=TRIGGERED, next_action='保留整设计归纳结果和 d10 前件证据', eta_utc='已结算',
                       work_status='整设计无界已证', alternative_evidence='')


def update_review_schedule(report_rows, now, choices):
    next_day = min(now.date() + dt.timedelta(days=1), dt.date.fromisoformat(DEADLINE))
    checkpoint = f'{next_day:%m-%d} 08:00 UTC 复核'
    for row in report_rows:
        if INT_COVER_PAUSE.is_file() and row['verdict'] == 'pending':
            if row['work_status'] == '整片无界安全性已证，前件无限延拓待证':
                dependency = '待原模型延拓证据'
            elif row['work_status'] in ('局部安全义务已证，提升及原片闭环待证', '原片闭环和切点 PDR 已证，待提升复核'):
                dependency = row['eta_utc']
            elif row['work_status'] == 'd20 原断言已通过，待同界前件见证':
                dependency = '待同界前件证据或局部无界结论'
            else:
                dependency = '待局部无界证明与提升复核' if choices.get('5') == 'b' else '待第 5 条或新版可复验方案'
            row['eta_utc'] = f'{dependency}，{checkpoint}'
        elif INT_COVER_PAUSE.is_file() and row['verdict'] == 'passed':
            if row['work_status'] in ('正式有界已取证，局部反例待原片验证', '原片仿真已见失配，严格认证待证'):
                row['eta_utc'] = f'有界已取证，原片可达性 {checkpoint}'
            else:
                row['next_action'] = '保留已核验的有界结果，无界复核沿用 Spec 27 的提升标准'
                row['eta_utc'] = '有界已取证，无界待排'
    return checkpoint


def record_local_pdr_attempts(by_property):
    if not LOCAL_PDR_ATTEMPTS.is_file():
        return
    for attempt in rows(LOCAL_PDR_ATTEMPTS):
        prop = attempt['property']
        row = by_property[prop]
        if row['verdict'] != 'pending':
            continue
        journal = ROOT / attempt['progress']
        events = [event for event in rows(journal) if event['property'] == prop]
        assert any(event['stage'] == 'local' and event['instance'] == attempt['instance']
                   and event['status'] == attempt['local_status'] for event in events), journal
        finals = [event for event in events if event['stage'] == 'final']
        if finals:
            assert any(event['status'] == attempt['final_status'] for event in finals), journal
        else:
            cli = ROOT / attempt['cli']
            assert attempt['final_status'] == 'unknown'
            assert any(line.split(maxsplit=2)[:2] == [prop, 'unknown'] and 'timeout after ' in line
                       for line in cli.read_text().splitlines()), cli
        assert (ROOT / attempt['versions']).is_file()
        instance = attempt['instance'].removeprefix(prop.split('.', 1)[0] + '.')
        row['detail'] += (
            f"。#55 局部试跑落在 {instance}，{attempt['wall_s']} 秒，cgroup 峰值 {attempt['cgroup_peak_mib']} MiB。"
            + attempt['detail']
        )
        row['alternative_evidence'] = rel(journal)
        row['work_status'] = f"局部落点已核，{attempt['phase']} 阻塞"
        row['next_action'] = '保留原下放条件，等 EBMC 核对时序转换阻塞'
        if attempt['phase'] == 'PDR 限时':
            row['work_status'] = '局部落点已核，PDR 超时'
            if prop == FLOAT_ZERO:
                row['next_action'] = '核对原读口 2 和零地址旁路，用小投影证明局部安全性'
            elif prop.endswith('.c_loop_end__chip_zero_stays_zero'):
                row['next_action'] = '用原整片定向程序核查回读零的减一嫌疑'
                if LOOP_ZERO_REPORT.is_file():
                    assert not list(rows(LOOP_ZERO_PROGRAM / 'antecedent-hits.tsv'))
                    window = {event['relative']: event for event in rows(LOOP_ZERO_PROGRAM / 'window.tsv')}
                    assert (window['c8']['actual_op'], window['c9']['actual_op']) == ('0', '8'), window
                    row['detail'] += '。原片定向仿真 #149 对 gp1 零值执行 LOOP_DEC，#155 回读全 1。原窗要求 #152 发 PASS，实际 #153 发，240 拍无完整前件'
                    row['next_action'] = '调整原取指对齐，使第二次 PASS 落在 LOOP_DEC 后三拍，再核原后件'
                    row['work_status'] = '原片减一线索已复现，原前件尚未匹配'
                    row['alternative_evidence'] = rel(LOOP_ZERO_REPORT)
                    if LOOP_ZERO_ALIGNED.is_file():
                        for name, c3, c8 in [('loop-back-max', 161, 166), ('loop-back-refill-gap', 174, 179)]:
                            trial = LOOP_ZERO_PROGRAM / name
                            events_by_tick = {int(event['actual']): event for event in rows(trial / 'window.tsv')}
                            assert events_by_tick[c3]['actual_op'] == '0' and events_by_tick[c8]['actual_op'] == '8'
                            assert not list(rows(trial / 'antecedent-hits.tsv'))
                        row['detail'] += '。两次结构化回跳已让第二次 PASS 准时落在 c8，FP 停顿使原 c3 均为 idle，完整触发仍为 0'
                        row['next_action'] = '保留 c8 回跳对齐，后续需让首次同索引 PASS 在 c3 实际发射'
            else:
                row['next_action'] = '保留 LOAD 两级请求和目的地址，核对可复用的局部写回切点'


def record_local_pdr_starts(by_property, now):
    if not LOCAL_PDR_STARTS.is_file():
        return
    for start in rows(LOCAL_PDR_STARTS):
        row = by_property[start['property']]
        if row['verdict'] != 'pending':
            continue
        events = {event['event']: event for event in rows(ROOT / start['timing'])}
        assert events['crisp_pid']['detail'] == start['pid'], start
        assert (ROOT / start['progress']).is_file(), start
        began = dt.datetime.fromisoformat(events['start']['utc'])
        deadline = began + dt.timedelta(seconds=int(start['limit_s']))
        if 'end' in events or now >= deadline:
            continue
        row['work_status'] = '第二批局部 PDR 已启动'
        row['next_action'] = f"{began:%H:%M:%S} UTC 启动。按阶段核对下放、原断言和前件"
        row['eta_utc'] = f'{deadline:%m-%d %H:%M:%S} UTC 任务上限'
        row['alternative_evidence'] = start['progress']
        stage_log = (ROOT / start['timing']).with_name('worker-stage.log')
        if stage_log.is_file() and 'ic3_solvert::analyze_initial_state' in stage_log.read_text():
            row['detail'] += '。新版下放后已进入 new IC3 初态 SAT 求解'
            row['work_status'] = '第二批局部 PDR 已进入 IC3'


def record_fp0_cut(by_property):
    row = by_property[FLOAT_ZERO]
    if row['verdict'] != 'pending' or not FP0_CUT_REPORT.is_file() or not FP0_CUT_PROGRESS.is_file():
        return
    event, = final_rows(FP0_CUT_PROGRESS)
    assert (event['property'], event['status']) == ('fp0_cut.fp0__chip_zero', 'proved'), event
    row['detail'] += '。原寄存器组读零小投影 PDR 通过，0.064 秒，21.69 MiB。独立接线复核确认安全性可提升，原片无限延拓待证'
    row['next_action'] = '保留已复核的整片无界安全性，补原片前件无限延拓'
    row['work_status'] = '整片无界安全性已证，前件无限延拓待证'
    row['alternative_evidence'] = rel(FP0_CUT_REPORT)
    record_pr57_bounded(
        row, FP0_BMC, 5,
        '原完整 RTL、单拍冷复位、零假设下正式同界非 vacuous 通过，#3/#4 命中前件，#5 观察原后件。'
        '保留 d20 前件查询超时与已提升无界安全性。原片无限延拓待证。',
    )


def record_gp0_evidence(by_property):
    row = by_property[SCALAR_STARTUP + '.gp0__chip_zero']
    if row['verdict'] == 'pending':
        record_pr57_bounded(
            row, GP0_BMC, 5,
            '原完整 RTL、单拍冷复位、零假设下正式同界非 vacuous 通过，既有原片波形 #1 命中前件、#3 观察后件。'
            '保留独立原断言 d20 无反例，原片无限延拓待证。',
        )
    replay = GP0_CUT_RUN / 'replay.txt'
    if row['verdict'] == 'passed' and replay.is_file():
        assert 'antecedent=#44, mux=#45, first_bad=#46, inner=True, outer=True' in replay.read_text()
        assert re.search(r'gp0_cut\.gp0__chip_zero\s+refuted\s+', (GP0_CUT_RUN / 'stdout').read_text())
        row['detail'] += '。控制切点 #43 发 LD_INT 写 gp0，#44 发 PASS_ADDR 读 gp0，#45 的自由非零 SRAM 返回经真实前递使 #46 局部失配。原片发射和数据可达性另证'
        row.update(work_status='正式有界已取证，局部反例待原片验证',
                   alternative_evidence=rel(GP0_CUT_REPORT if GP0_CUT_REPORT.is_file() else replay),
                   next_action='核对生成零目标、非零写回和下一拍 PASS 的实际译码，确认局部反例在原片是否可达')
        audit = REF / 'evidence/spec27/gp0-cut-refuted-boundary-audit.md'
        if audit.is_file():
            row['detail'] += '。独立静态复核确认原译码允许零目标写回后紧接 PASS_ADDR，原片实际坏拍与延拓待核'
            row.update(other_evidence=rel(audit),
                       next_action='用原片顶层取指复放 LUI 写 gp0 后接 PASS_ADDR 读 gp0，核对坏拍和尾段')
    if row['verdict'] == 'passed' and (GP0_PROGRAM / 'property.tsv').is_file():
        sample, = list(rows(GP0_PROGRAM / 'property.tsv'))
        assert (sample['antecedent_sample'], sample['consequent_sample'], sample['observed_gp_out_1_hex'], sample['result']) == (
            '43', '45', '00001000', 'finite_mismatch',
        )
        row['detail'] += '。原整芯片定向仿真 #43 命中前件，#45 采到 0x1000，#64→#65 为观测状态回边候选，严格认证待证'
        row.update(work_status='原片仿真已见失配，严格认证待证',
                   other_evidence=rel(GP0_PROGRAM / 'README.md'),
                   next_action='独立复核原性质及尾段，再评估单一回边的原模型严格认证')
        if GP0_PROGRAM_AUDIT.is_file():
            assert '原片有限失配证据独立核验完全通过（CONFIRMED）' in GP0_PROGRAM_AUDIT.read_text()
            row.update(measurement(
                'refuted', 'Verilator 5.037 · 原片定向仿真', '', '0.09',
                int((GP0_PROGRAM / 'memory.peak').read_text()) // 1024, '跟踪构建及仿真 cgroup 峰值', '', '',
                '原单拍冷复位与顶层 TileLink 输入下，#43 命中前件；#45 读到 0x1000，原后件要求零。独立核对通过，按 Leiqi 确定的逐条验收口径收账。历史 d5 通过保留',
                GP0_PROGRAM / 'property.tsv', GP0_BMC / 'SUMMARY.txt',
            ))
            row.update(alternative_evidence=rel(GP0_PROGRAM_AUDIT), waveform=rel(GP0_PROGRAM / 'gp0.vcd'),
                       next_action='保留原片反例波形和复验输入', eta_utc='已取证', work_status='反例已收账')


def record_store_cut(by_property):
    row = by_property['plena_isa_chip_int_effects.s_st_int__chip_registers_unchanged']
    if row['verdict'] != 'pending' or not STORE_CUT_REPORT.is_file():
        return
    journal, = STORE_CUT_RUN.glob('crisp-out/*/progress-*.tsv')
    assert not list(rows(journal)), journal
    assert 'unknown  29.004 s  timeout after 29 s' in (STORE_CUT_RUN / 'stdout.log').read_text()
    row['detail'] += '。STORE 切点保留原写使能、地址和前递，四根结果数据自由。局部 PDR 29.004 秒超时，采样峰值 89.38 MiB。TSV 只有表头，采集竞争漏记 Crisp 退出码'
    row['next_action'] = '分别证明排空后的写入与前递条件，再核对两次读回相等'
    row['work_status'] = 'STORE 切点已映射，局部 PDR 限时'
    row['alternative_evidence'] = rel(STORE_CUT_REPORT)
    if STORE_OBLIGATIONS_REPORT.is_file():
        control, = STORE_OBLIGATIONS_RUN.glob('control/crisp-out/*/progress-*.tsv')
        read, = STORE_OBLIGATIONS_RUN.glob('read/crisp-out/*/progress-*.tsv')
        control_result, = final_rows(control)
        read_result, = final_rows(read)
        assert control_result['status'] == 'proved', control
        assert (read_result['status'], read_result['detail']) == ('unknown', 'proved; vacuity check timed out'), read
        row['detail'] += '。两项同事件义务中，GP 排空与前递 9.730 秒 PDR 已证，回读隔离字级归纳已证安全性，44.011 秒在前件认证超时'
        row['next_action'] = '独立复核两义务的同一事件。用具体 STORE 程序取原片触发，再验证尾部闭环'
        row['work_status'] = '两个局部安全义务已证，提升和原片无限触发待核'
        row['alternative_evidence'] = rel(STORE_OBLIGATIONS_REPORT)
    if (STORE_PROGRAM / 'match.tsv').is_file():
        match = {int(item['sample']): item for item in rows(STORE_PROGRAM / 'match.tsv')}
        assert [int(match[i]['assigned_int_op'], 16) for i in range(144, 155)] == [0, 0, 0, 8, 0, 7, 0, 0, 0, 8, 0]
        assert match[147]['rs1'] == match[153]['rs1'] and match[149]['gp_out_1'] == match[155]['gp_out_1']
        row['detail'] += '。两义务经独立复核可提升整片安全性。原片定向程序在 #144–154 命中完整前件，#155 读回 145，原模型无限延拓待证'
        row['next_action'] = '重放已命中的顶层输入，核验完整原片尾部回边'
        row['work_status'] = '整片无界安全性已证，前件无限延拓待证'
    if STORE_CLOSURE.is_file():
        row['detail'] += '。全层次仿真观察到 #175→#176 候选自环，待原模型正式认证'
        row['other_evidence'] = rel(STORE_CLOSURE)
    if STORE_SCALE.is_file():
        row['detail'] += '。Yices 6/21 帧实测峰值 1.81/4.99 GiB，按实测斜率估计 155/177 帧超过 32 GiB'
        row['next_action'] = '先将具体程序压入首个取指突发，再选择缩短后的原片闭环认证界'
        row['eta_utc'] = '待短程序命中及资源复核'
        row['other_evidence'] = rel(STORE_SCALE)
    if (FIRST_FILL / 'prediction-vs-observation.tsv').is_file():
        check_first_fill('store')
        row['detail'] += '。单次取指填充的原片仿真在 #37–47 触发，#48 后件相等，#68→#69 为更短仿真回边候选'
        row.update(next_action='以首块原模型输入准备 #68→#69 闭环认证，等待整片资源',
                   other_evidence=rel(FIRST_FILL / 'README.md'), waveform=rel(FIRST_FILL / 'store/trace.vcd'))
        if FIRST_FILL_STORE_SUB_AUDIT.is_file():
            row['detail'] += '。Neon 独立核对 STORE 有限触发及 #68→#69 仿真候选'
            row['other_evidence'] = rel(FIRST_FILL_STORE_SUB_AUDIT)
    if (STORE_FIXED / 'result.tsv').is_file():
        result, = rows(STORE_FIXED / 'result.tsv')
        row['detail'] += '。' + result['detail']
        row.update(next_action=result['next_action'], work_status=result['work_status'],
                   eta_utc='待固定回边试验复核', other_evidence=rel(STORE_FIXED / 'README.md'))
    if STORE_TOTALITY_TRIAGE.is_file():
        row['detail'] += '。EBMC 独立复现 packed struct/bv、member、index/with 和组合环的原生判据边界，整片总性待证'
        row.update(next_action='保留原始查询和总性分诊，同形整片回边及总性试跑暂停',
                   eta_utc='待后端证据及原模型严格延拓')
    if STORE_SPEC16_REPORT.is_file():
        report = STORE_SPEC16_REPORT.read_text()
        assert '0.52780%' in report and '33,966 个自由符号' in report
        row['detail'] += ('。EBMC Spec16 静态缩减固定回边查询，check-sat 前缀减少 0.52780%，'
                          '保留 21,624 条边界等式。原生 footer 漏读 33,966 个自由符号，补读表已生成。'
                          '原模型严格延拓仍待证，整片试跑暂停')
        row.update(next_action='保留原查询、总性分诊和 Spec16 补读表，整片试跑暂停',
                   other_evidence=rel(STORE_SPEC16_REPORT))


def record_scalar_memory_and_forwarding(by_property):
    row = by_property[INT_MEMORY]
    if row['verdict'] == 'pending' and (INT_MEMORY_FIRST_FILL / 'results.tsv').is_file():
        observed, = rows(INT_MEMORY_FIRST_FILL / 'results.tsv')
        assert tuple(observed[key] for key in (
            'complete_original_hits', 'store_addr_at_43', 'load_addr_at_45',
            'actual_gp_write_addr_at_46', 'all_checks_pass', 'stable_from', 'next_stable_sample',
        )) == ('1', '5', '5', '3', '1', '64', '65'), observed
        row['detail'] += ('。原片首块 #37–47 完整前件命中一次，#43 写与 #45 读均为 SRAM 物理字 5，'
                          '#46 两级 LOAD 地址对齐并发生 RF 口 1 同拍写穿，#48 比较相等。'
                          '所有数据均为零，仅记有限轨迹，#64→#65 为仿真回边候选')
        row.update(work_status='原片有限前件已核，A/B/C 局部义务及严格延拓待证',
                   next_action='等待 A/B/C 的独立映射审阅，再排局部义务',
                   eta_utc='待独立映射审阅及有限诊断复核', alternative_evidence=rel(INT_MEMORY_LOCAL_REPORT),
                   other_evidence=rel(INT_MEMORY_FIRST_FILL / 'README.md'),
                   waveform=rel(INT_MEMORY_FIRST_FILL / 'int-memory/trace.vcd'))
        if INT_MEMORY_LIFT_AUDIT.is_file():
            row['detail'] += ('。Opus 第 5 节独立重采样确认完整前件命中一次，全零数据无法区分后件。'
                              'Opus 另核对 A/B/C 的逐拍原映射，原片严格延拓待证')
            row.update(alternative_evidence=rel(INT_MEMORY_LIFT_AUDIT), next_action='按独立核对的映射安排三项局部义务',
                       eta_utc='待三项局部结果及严格延拓')
        if INT_MEMORY_LOCAL_RESULTS.is_file():
            local = list(rows(INT_MEMORY_LOCAL_RESULTS))
            assert [(r['phase'], r['main_safety'], r['antecedent'], r['final'], r['wall_s']) for r in local] == [
                ('A', 'proved', 'timeout', 'unknown', '119.002'),
                ('B', 'proved', 'certified', 'proved', '2.402'),
                ('C', 'proved', 'timeout', 'unknown', '119.001'),
            ], local
            assert all(r['oom_kill'] == '0' and (INT_MEMORY_LOCAL_INPUTS / r['progress']).is_file() for r in local)
            row['detail'] += ('。A/B/C 主安全性均 proved，按已审逐拍交接组合为整片无界主安全性。'
                              'A 前件认证 119.002 秒到限，C 前件认证 119.001 秒到限，两项 final unknown。'
                              'B 2.402 秒 certified，final proved。整片严格延拓待证')
            row.update(work_status='整片无界主安全性已证，严格延拓待证', next_action='保留三份原 final 与组合证据，原片严格延拓待证',
                       eta_utc='待原片严格延拓', other_evidence=rel(INT_MEMORY_LOCAL_INPUTS / 'README.md'))

    row = by_property['plena_isa_chip_interface_effects.s_add_int__chip_dependent_forwarding']
    if row['verdict'] == 'pending' and (FIRST_FILL_ADD_FORWARD / 'results.tsv').is_file():
        observed, = rows(FIRST_FILL_ADD_FORWARD / 'results.tsv')
        assert tuple(observed[key] for key in ('complete_original_hits', 'all_checks_pass', 'stable_from',
                                               'next_stable_sample')) == ('1', '1', '64', '65'), observed
        row['detail'] += ('。候选 F 在 #37–46 命中原片完整前件，#44 原 ADD 结果写 gp3 并前递到口 1，'
                          '口 2 保持 gp1，#45 原 ADD 结果写 gp4 并同拍 RF 写穿。#47 后件相等，'
                          '全零数据仅记有限轨迹，#64→#65 为仿真回边候选')
        if ADD_FORWARD_AUDIT.is_file():
            row['detail'] += '。Opus 第 5 节独立重采样确认完整前件命中一次，全零数据无法区分后件，原片严格延拓待证'
        row.update(work_status='原片有限前件已核，局部安全性及严格延拓待证',
                   next_action='局部数据义务保留原口 2、前递及 ADD 结果寄存器', eta_utc='待局部证明及严格延拓',
                   alternative_evidence=rel(ADD_FORWARD_AUDIT), other_evidence=rel(FIRST_FILL_ADD_FORWARD / 'README.md'),
                   waveform=rel(FIRST_FILL_ADD_FORWARD / 'add-forward/trace.vcd'))
        if (ADD_FORWARD_RESULTS / 'proof-results.tsv').is_file():
            control, data = rows(ADD_FORWARD_RESULTS / 'proof-results.tsv')
            assert (control['phase'], data['phase']) == ('control', 'data')
            if all(r['main_safety'] == 'proved' for r in (control, data)):
                row['detail'] += '。控制和数据局部主安全性均 proved，按已审 38 项交接可组合为原片无界主安全性'
                row.update(work_status='整片无界主安全性已证，严格延拓待证', eta_utc='待原模型严格延拓',
                           next_action='保留两份原 final 与组合证据，原片严格延拓待证')
            for label, result in (('控制', control), ('数据', data)):
                timing = f"，前件认证 {result['wall_s']} 秒到限" if result['antecedent'] == 'timeout' else ''
                row['detail'] += f"。{label}主安全 {result['main_safety']}，final {result['final']}{timing}"
            row['other_evidence'] = rel(ADD_FORWARD_RESULTS / 'README.md')


def record_load_preserve(by_property):
    row = by_property['plena_isa_chip_int_effects.s_ld_int__chip_other_register']
    report = LOAD_OBLIGATIONS / 'README.md'
    if row['verdict'] != 'pending' or not report.is_file():
        return
    control, = (LOAD_OBLIGATIONS / 'control').glob('crisp-out/*/progress-*.tsv')
    read, = (LOAD_OBLIGATIONS / 'read').glob('crisp-out/*/progress-*.tsv')
    control_event, = final_rows(control)
    read_event, = final_rows(read)
    assert (control_event['property'], control_event['status']) == ('g2_load_control.writeback_isolation', 'proved')
    assert (read_event['property'], read_event['status'], read_event['detail']) == (
        'g2_load_read.readback_isolation', 'unknown', 'proved; vacuity check timed out',
    )
    assert '119.001 s  proved; vacuity check timed out' in (LOAD_OBLIGATIONS / 'read/stdout').read_text()
    record_single_fill(row, FIRST_FILL_LOAD, 'load')
    assert '23/23' in LOAD_LIFT_AUDIT.read_text()
    row['detail'] += ('。#43 原 SRAM 读物理字 5，未初始化 SRAM 的仿真零值只用于有限波形。'
                      '局部写回 final proved，读回主安全 proved、前件认证 119.001 秒到限、final unknown。'
                      'Opus 独立核对 23/23 交接可提升及原片有限 VCD，整片严格无限延拓待证')
    row.update(work_status='整片无界安全性已证，前件无限延拓待证',
               next_action='保留首块有限前件和独立提升证据，原片严格延拓待证', eta_utc='待原模型严格延拓',
               alternative_evidence=rel(LOAD_LIFT_AUDIT), other_evidence=rel(report))


def record_addr2_first_fill(by_property):
    for layout, targets, audit, out1, start, end in (
        ('Q', ('pass_addr_2__chip_reads',), PASS_ADDR2_AUDIT, '0', '66', '67'),
        ('M', ('comp_addr_2__chip_address', 'comp_addr_2__chip_data'), COMP_ADDR2_AUDIT, '5', '68', '69'),
    ):
        run = ADDR2_FIRST_FILL / layout
        evidence = run / 'results.tsv'
        if not evidence.is_file():
            continue
        result = list(rows(evidence))
        assert [sample['property'] for sample in result] == list(targets), result
        for sample in result:
            assert tuple(sample[key] for key in ('complete_original_hits', 'post_equal_at_48', 'first_stall',
                                                  'gp_out_1_at_48', 'gp_out_2_at_48', 'stable_from',
                                                  'next_stable_sample')) == ('1', '1', '46', out1, '7', start, end), sample
            row = by_property['plena_isa_chip_interface_effects.' + sample['property']]
            if row['verdict'] != 'pending':
                continue
            row['detail'] += (f'。候选 {layout} 经 Opus 第 4 节独立重采样，原 #41–47 完整前件命中一次，'
                              f'首次停顿 #46，#48 输出 {out1}/7，原后件成立。#{start}→#{end} 为仿真回边候选，'
                              '原片严格延拓待证')
            row.update(work_status='原片有限前件已核，严格延拓待证', next_action='保留原片有限波形和逐拍独立核对',
                       eta_utc='待局部证明与原模型严格延拓', alternative_evidence=rel(audit),
                       other_evidence=rel(evidence), waveform=rel(run / 'trace.vcd'))

    results_path = ADDR2_OBLIGATIONS / 'proof-results.tsv'
    if not results_path.is_file() or not ADDR2_LIFT_AUDIT.is_file():
        return
    results = list(rows(results_path))
    expected = (
        ('pass', 'pass_addr_2__chip_reads', '119.002'),
        ('comp_address', 'comp_addr_2__chip_address', '119.001'),
        ('comp_data', 'comp_addr_2__chip_data', '119.001'),
    )
    assert len(results) == len(expected), results
    for result, (phase, prop, wall) in zip(results, expected):
        local_prop = f'addr2_projection.{prop}'
        assert tuple(result[key] for key in ('phase', 'property', 'main_safety', 'antecedent', 'final',
                                             'wall_s', 'oom_kill')) == (
            phase, local_prop, 'proved', 'timeout', 'unknown', wall, '0'
        ), result
        progress = ADDR2_OBLIGATIONS / result['progress']
        final, = final_rows(progress)
        assert (final['property'], final['status'], final['detail']) == (
            local_prop, 'unknown', 'proved; vacuity check timed out'
        ), final
        row = by_property[f'plena_isa_chip_interface_effects.{prop}']
        if row['verdict'] == 'pending':
            row['detail'] += (f'。局部无界主安全 proved，经 Opus 独立核对可提升为原式无界主安全。'
                              f'局部前件认证 {wall} 秒到限，final unknown，无 OOM。原模型严格延拓待证')
            row.update(work_status='整片无界主安全性已证，严格延拓待证',
                       next_action='保留 Opus 映射、Q/M 有限波形和原 final，原片严格延拓待证',
                       eta_utc='待原模型严格延拓', other_evidence=rel(progress))


def record_vector_sub_vf(by_property):
    if not VECTOR_SUB_VF_AUDIT.is_file():
        return
    conditions = ('rst', 'ele', 'red', 'a_valid', 'b_valid', 's_valid')

    def sampled(row):
        return tuple(row[key] for key in conditions)

    for opcode, directory, lane in (('2', 'vsubvf-sub2', 'be0'), ('9', 'vsubvf-rsub9', '3e0')):
        name = f'plena_isa_chip_vector_fp12.v_sub_vf__chip_result_{opcode}'
        row = by_property[name]
        if row['verdict'] != 'pending':
            continue
        run = VECTOR_SUB_VF_RUNS / directory
        for line in (run / 'evidence.sha256').read_text().splitlines():
            digest, filename = line.split()
            assert sha256((run / filename).read_bytes()).hexdigest() == digest, filename
        samples = run / 'samples.tsv'
        observed = {int(item['sample']): item for item in rows(samples)}
        assert [i for i, item in observed.items() if item['rst'] == '1'] == [0]
        assert all(sampled(observed[i]) == ('0',) * 6 for i in (*range(1, 69), 70, *range(72, 84)))
        assert [i for i, item in observed.items()
                if sampled(item) == ('0', opcode, '0', '0', '0', '0') and item['broadcast'] == '1'] == [69]
        assert [i for i, item in observed.items()
                if sampled(item) == ('0', '0', '0', '1', '0', '1')] == [71]
        assert observed[71]['a'] == '0' * 21 + '3e0' and observed[82]['write_req'] == '1'
        # The reused sampler's reference column is for VV. Use the audited VF reference.
        assert observed[84]['actual'] == lane * 7 + '000'
        row['detail'] += (f'。Opus 原片有限正例，完整前件命中一次，#69 执行、#71 取样、#82 写请求为 1。'
                          f'#84 实际与原 VF 参考相等，lane0 为 0x000，lane1–7 为 0x{lane}。'
                          '原模型严格延拓及整片结论待证')
        row.update(work_status='原片有限正例已核，整片待结算',
                   next_action='保留原片有限正例与原参考重算，严格延拓待证',
                   eta_utc='待原模型严格延拓', alternative_evidence=rel(VECTOR_SUB_VF_AUDIT),
                   other_evidence=rel(samples), waveform=rel(run / 'replay.vcd'))


def record_vector_add_mul(by_property):
    for directory, opcode, write_sample, compare in VECTOR_ADD_MUL_CASES:
        run = VECTOR_ADD_MUL_RUNS / directory
        report = run / 'REPORT.md'
        if not report.is_file():
            continue
        name = f"plena_isa_chip_vector_fp12.v_{directory.replace('-', '_')}__chip_result_{opcode}"
        row = by_property[name]
        if row['verdict'] != 'pending':
            continue
        for line in (run / 'evidence.sha256').read_text().splitlines():
            digest, filename = line.split()
            assert sha256((run / filename).read_bytes()).hexdigest() == digest, filename
        result, = rows(run / 'result.tsv')
        assert tuple(result[key] for key in ('property', 'status', 'anchor', 'command', 'packet',
                                             'write_sample', 'write_req', 'compare', 'native_strict_certification')) == (
            name, 'finite_match', '1', '69', '71', write_sample, '1', compare, 'not_obtained'
        ), result
        samples = run / 'property-window.tsv'
        observed = {item['sample']: item for item in rows(samples)}
        compare_row = observed[compare]
        assert observed[write_sample]['write_req'] == compare_row['original_write_request'] == '1'
        assert compare_row['actual'] == compare_row['original_reference'] == result['actual'] == result['reference']
        waveform = run / 'replay.vcd'
        assert sha256(waveform.read_bytes()).hexdigest() == result['waveform_sha256']
        row['detail'] += (f"。sol1 冻结原片复放的完整前件命中一次，#69 发射、#71 取样、#{write_sample} 写请求为 1。"
                          f"#{compare} 实际与原参照均为 0x{result['actual']}。原生严格认证与无限延拓待证")
        row.update(work_status='原片有限正例已核，整片待结算',
                   next_action='保留原片有限正例与原参照，严格延拓待证',
                   eta_utc='待原模型严格延拓', alternative_evidence=rel(report),
                   other_evidence=rel(samples), waveform=rel(waveform))


def record_vector_operand_sweep(by_property):
    if not VECTOR_SWEEP_AUDIT.is_file():
        return
    checksums = {}
    for line in (VECTOR_SWEEP_RUN / 'evidence.sha256').read_text().splitlines():
        digest, filename = line.split(maxsplit=1)
        checksums[filename] = digest
    for name in VECTOR_SWEEP_CASES:
        row = by_property[f'plena_isa_chip_vector_fp12.{name}']
        if row['verdict'] != 'pending':
            continue
        filename = f'cases/{name}/sweep.tsv'
        table = VECTOR_SWEEP_RUN / filename
        assert sha256(table.read_bytes()).hexdigest() == checksums[filename], filename
        sweep = list(rows(table))
        assert [int(item['case']) for item in sweep] == list(range(32)), filename
        for item in sweep:
            assert tuple(item[key] for key in ('rc', 'debug_ok', 'hits', 'write_req', 'ok')) == (
                '0', 'True', '1', '1', 'True'
            ) and item['actual'] == item['reference'], (name, item)
        row['detail'] += '。Opus 原片操作数扫查 32/32 完整命中、32/32 后件成立、0 失配'
        row['other_evidence'] = rel(table)


def record_add_int_chip_refutation(by_property, choices):
    if choices.get('6') != 'a' or not ADD_INT_CHIP_AUDIT.is_file():
        return
    run = ADD_INT_CHIP_RUN
    checksums = dict(reversed(line.split(maxsplit=1)) for line in (run / 'evidence.sha256').read_text().splitlines())
    for filename in ('window.tsv', 'original-property.sv.txt', 'replay.vcd', 'top-inputs.tsv'):
        assert sha256((run / filename).read_bytes()).hexdigest() == checksums[filename], filename
    observed = {int(item['sample']): item for item in rows(run / 'window-all.tsv')}
    assert all(observed[cycle]['rst'] == '0' for cycle in range(51, 58))
    assert observed[51]['op'] != '4'
    first, idle, add, last = (observed[cycle] for cycle in range(52, 56))
    assert (first['op'], first['rs1'], first['rs2']) == ('8', '0', '5')
    assert idle['rd'] not in (first['rs1'], first['rs2'])
    assert add['op'] == '1' and add['rd'] != '0' and (add['rs1'], add['rs2']) == (first['rs1'], first['rs2'])
    assert last['op'] == '8' and last['rs1'] == add['rd']
    actual = int(observed[57]['o1'], 16)
    reference = (int(add['o1'], 16) + int(add['o2'], 16)) & 0xffffffff
    assert (actual, reference) == (0x00000007, 0x0000012a)
    wall = re.search(r'walltime ([\d.]+) s', (run / 'sim.log').read_text())[1]
    peak = int((run / 'memory-peak.txt').read_text()) // 1024
    row = by_property[ADD_INT_CHIP]
    detail = (f'Opus 原片复放在 #52–#55 命中完整原前件，#57 实际 0x{actual:08x}、原参照 0x{reference:08x}。'
              '按 Spec 44 第 6 条 a 收为有限反例，原生严格认证未取得。历史整片求解记录。' + row['detail'])
    row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', wall, peak,
                           '仿真墙钟及 cgroup 峰值', '', '', detail, run / 'window.tsv', run / 'sim.log'))
    row.update(work_status='按第 6 条 a 收账', next_action='保留原片有限反例及严格认证状态',
               eta_utc='已结算', alternative_evidence=rel(ADD_INT_CHIP_AUDIT), waveform=rel(run / 'replay.vcd'))


def record_scalar_gp0_forwarding(by_property, choices):
    if choices.get('6') != 'a' or not SCALAR_GP0_SIX_AUDIT.is_file():
        return
    peak = int((SCALAR_GP0_SIX_RUN / 'memory-peak.txt').read_text()) // 1024
    for op, compare, expected in (
        ('s_addi_int', 57, (0x00000045, 0x00000168)),
        ('s_sub_int', 57, (0xfffffff9, 0x0000011c)),
        ('s_mul_int', 58, (0x00000000, 0x000007f5)),
    ):
        run = SCALAR_GP0_SIX_RUN / op
        for line in (run / 'evidence.sha256').read_text().splitlines():
            digest, filename = line.split(maxsplit=1)
            if filename in ('window.tsv', 'replay.vcd', 'original-property.sv.txt', 'top-inputs.tsv'):
                assert sha256((run / filename).read_bytes()).hexdigest() == digest, filename
        window = run / 'window.tsv'
        observed = {int(item['sample'][1:]): item for item in rows(window)}
        assert all(observed[cycle]['rst'] == '0' for cycle in range(51, compare + 1))
        assert all(observed[cycle]['atom_holds'] == 'True' for cycle in range(52, compare - 1))
        assert observed[51]['assigned_int_op'] == '2' and observed[52]['s_rs1'] == '0'
        packet = observed[54]
        lhs, rhs = int(packet['gp_out_1'], 16), int(packet['gp_out_2'], 16)
        reference = {'s_addi_int': lhs + int(packet['s_imm'], 16),
                     's_sub_int': lhs - rhs, 's_mul_int': lhs * rhs}[op] & 0xffffffff
        actual = int(observed[compare]['gp_out_1'], 16)
        assert (actual, reference) == expected, (op, actual, reference)
        wall = re.search(r'walltime ([\d.]+) s', (run / 'sim.log').read_text())[1]
        row = by_property[f'{SCALAR_STARTUP}.{op}__chip_result']
        detail = (f'Opus 原片复放在 #52–#{compare - 2} 命中完整原前件，'
                  f'#{compare} 实际 0x{actual:08x}、原参照 0x{reference:08x}。'
                  '反例触发 gp0 前递，按 Spec 44 第 6 条 a 收账，原生严格认证未取得。历史整片求解记录。'
                  + row['detail'])
        row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', wall, peak,
                               '仿真墙钟及三条共享 cgroup 峰值', '', '', detail, window, run / 'sim.log'))
        row.update(work_status='按第 6 条 a 收账', next_action='保留原片有限反例及严格认证状态',
                   eta_utc='已结算', alternative_evidence=rel(SCALAR_GP0_SIX_AUDIT), waveform=rel(run / 'replay.vcd'))
    for op, explanation in (
        ('s_lui_int', 't+1 的 PASS 读 rd@t，原式要求 rd@t 非零，窗口内无 gp0 读取'),
        ('c_loop_start', 't+1 的 PASS 读 rd@t，原式要求 rd@t 非零，窗口内无 gp0 读取'),
        ('c_loop_end', '原 decoder 在 t+2 置 rs1=rd，原式要求 rs1@t=rs1@t+2 且 rd@t+2 非零，故 t 拍 PASS 读址非零'),
    ):
        row = by_property[f'{SCALAR_STARTUP}.{op}__chip_result']
        if row['verdict'] == 'pending':
            row['detail'] += '。Opus 核对 gp0 前递机制无法满足该前件。' + explanation
            row['alternative_evidence'] = rel(SCALAR_GP0_SIX_AUDIT)


def record_loop_zero_refutation(by_property, choices):
    if choices.get('6') != 'a' or not LOOP_ZERO_REFUTED_AUDIT.is_file():
        return
    run = LOOP_ZERO_REFUTED_RUN
    for line in (run.parent / 'evidence.sha256').read_text().splitlines():
        digest, filename = line.split(maxsplit=1)
        if filename.startswith(run.name + '/'):
            assert sha256((run.parent / filename).read_bytes()).hexdigest() == digest, filename
    window = run / 'window-all.tsv'
    samples = {int(item['sample']): item for item in rows(window)}
    assert [samples[cycle]['op'] for cycle in range(49, 59)] == ['0', '0', '0', '8', '0', 'd', '0', '0', '8', '0']
    assert all(samples[cycle]['rst'] == samples[cycle]['stall'] == samples[cycle]['brk'] == '0'
               for cycle in range(49, 60))
    assert samples[52]['rs1'] == samples[54]['rs1'] == samples[54]['rd'] == samples[57]['rs1'] == '1'
    assert samples[54]['o1'] == '0' and samples[59]['o1'] == 'ffffffff'
    log = run / 'sim.log'
    wall = re.search(r'walltime ([\d.]+) s', log.read_text())[1]
    row = by_property[LOOP_ZERO_REFUTED]
    detail = ('Opus 原片 #49–#58 命中完整原前件，#59 实际 gp_out_1=0xffffffff，原后件要求 0。'
              '按第 6 条 a 收为有限反例，600 拍全状态未见回边，严格无限延拓证书未取得。历史整片求解记录。'
              + row['detail'])
    row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', wall, '',
                           'Verilator 仿真墙钟', '', '', detail, window, log))
    row.update(work_status='按第 6 条 a 收账', next_action='保留有限反例与严格认证状态',
               eta_utc='已结算', alternative_evidence=rel(LOOP_ZERO_REFUTED_AUDIT), waveform=rel(run / 'replay.vcd'))


def record_decision_seven_witnesses(by_property):
    opus = ADD_INT_CHIP_RUN.parent
    scalar = SCALAR_STARTUP + '.'
    split = '局部控制与数据 k-induction 证成，提升终审及独立复核通过。'
    cases = (
        (scalar + 's_lui_int__chip_result', SCALAR_CHIP_WITNESS, SCALAR_LIFT_INDEPENDENT,
         opus / 's-int-witness/s_lui_int',
         split + '原片完整前件有限见证，#54 后件成立（0x12345000）。'),
        (scalar + 'c_loop_start__chip_result', SCALAR_CHIP_WITNESS, SCALAR_LIFT_INDEPENDENT,
         opus / 's-int-witness/c_loop_start',
         split + '原片完整前件有限见证，#54 后件成立（5）。'),
        (scalar + 'c_loop_end__chip_result', SCALAR_LOOP_END_WITNESS, SCALAR_LIFT_INDEPENDENT,
         opus / 'loop-end-skid/V2_loopstart',
         '局部安全已证并独立复核。原片 #65–#68 完整前件有限见证'
         '（经 skid 路径，V2 带配对 LOOP_START），#70 后件成立。'),
        ('plena_isa_chip_interface_effects.comp_addr__chip_effective_address', COMP_ADDR_WITNESS, None,
         opus / 'comp-addr-chip/C_startup',
         '局部控制与数据 k=16 证成，独立复核通过。原片 #37–#43 完整前件有限见证，#44 后件成立。'),
        ('plena_isa_chip_int_preserve_g2.c_loop_end__chip_other_register', G2_LOOP_END_WITNESS, LOOP_END_LIFT_AUDIT,
         opus / 'g2-loop-end-chip/targeted/t_PEMP',
         '原片 #37–#47 完整前件有限见证，#48 后件成立（gp1=0，未配对 LOOP_START）。安全已提升并独立复核。'),
    )
    for name, audit, independent, run, detail in cases:
        row = by_property[name]
        if row['verdict'] != 'pending':
            continue
        row['detail'] += '。' + detail + '原片严格延拓证书未取得。待 decisions 第 7 条'
        row.update(work_status='安全已提升，原片有有限正例', next_action='等 decisions 第 7 条',
                   eta_utc='待第 7 条决定', alternative_evidence=rel(audit), waveform=rel(run / 'replay.vcd'),
                   other_evidence=rel(independent or run / 'window.tsv'))


def record_vector_sub_and_reci(by_property, choices):
    row = by_property[VECTOR_SUB_VV]
    if choices.get('6') == 'a' and VECTOR_SUB_AUDIT.is_file():
        wave = VECTOR_SUB_REPLAY / 'replay.vcd'
        samples = VECTOR_SUB_REPLAY / 'samples.tsv'
        observed = {sample['sample']: sample for sample in rows(samples)}
        assert sha256(wave.read_bytes()).hexdigest() == '1701369d903d4427df43ac28de39b859e720c619fbbd3cc177be9e5e0fce2615'
        assert observed['82']['write_req'] == '1' and observed['84']['actual'] == '0' * 21 + '3e0'
        assert observed['84']['reference'] == '0' * 21 + 'be0'
        assert 'exactly 1 antecedent match' in VECTOR_SUB_AUDIT.read_text()
        wall = re.search(r'walltime ([\d.]+) s', (VECTOR_SUB_REPLAY / 'sim.log').read_text())[1]
        peak = int((VECTOR_SUB_REPLAY / 'memory-peak.txt').read_text()) // 1024
        detail = ('Neon 独立核对原整片有限反例，按 Spec 44 第 6 条 a 收账。完整前件命中一次，'
                  '#84 实际 0x3e0，原参照 0xbe0，比较前两拍 #82 wreq=1。'
                  '原生 SVA 严格认证和无限延拓均未取得。历史整片结果。' + row['detail'])
        row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', wall, peak,
                               '仿真墙钟及构建和仿真 cgroup 峰值', '', '', detail, samples, VECTOR_SUB_REPLAY / 'sim.log'))
        row.update(waveform=rel(wave), alternative_evidence=rel(VECTOR_SUB_AUDIT), work_status='按第 6 条 a 收账',
                   next_action='保留原片有限反例及严格认证状态', eta_utc='已结算')

    row = by_property[VECTOR_RECI]
    if row['verdict'] == 'pending' and RECI_LEAF_AUDIT.is_file():
        leaf_wave = RECI_LEAF / 'seq-any-fp.vcd.reci_leaf_diag.reci_leaf_any_counterexample.vcd'
        assert sha256(leaf_wave.read_bytes()).hexdigest() == 'b093d137790e521fefe780953eec8faa2e2120f34a404e0eda91452c4f01263b'
        assert int((RECI_LEAF / 'seq-any-fp.memory.peak').read_text()) == 44961792
        assert re.search(r'(?m)^\[reci_leaf_diag\.reci_leaf_any\].*: REFUTED$', (RECI_LEAF / 'seq-any-fp.log').read_text())
        row['detail'] += ('。Neon 叶级单 lane 有限前缀 d6 在第 6 拍产生 0x63f，原参照 0x7df，'
                          '0.30 秒、42.9 MiB。此次见证仅覆盖叶级')
        row.update(alternative_evidence=rel(RECI_LEAF_AUDIT), other_evidence=rel(leaf_wave),
                   work_status='叶级有限反例已核，整片待结算', eta_utc='待原片证据',
                   next_action='保留叶级有限反例，整片证据待取')
    if row['verdict'] == 'pending' and RECI_CHIP_D72_AUDIT.is_file():
        assert int((RECI_CHIP_D72 / 'd72.memory.peak').read_text()) == 24 * 1024 ** 3
        events = dict(line.split() for line in (RECI_CHIP_D72 / 'd72.memory.events').read_text().splitlines())
        assert events['oom_kill'] == '1', events
        transcript = (RECI_CHIP_D72 / 'd72.log').read_text()
        assert transcript.rstrip().endswith('Generating Decision Problem\nProperties')
        assert 'Solving with SMT2' not in transcript and not list(RECI_CHIP_D72.glob('*.vcd'))
        assert '179.76 s' in RECI_CHIP_D72_AUDIT.read_text()
        row['detail'] += '。原片 d72 在 Properties 展开阶段 179.76 秒触及 24 GiB 硬限并被 OOM kill，未入 Yices、无 VCD 或性质判定'
        row.update(other_evidence=rel(RECI_CHIP_D72_AUDIT),
                   work_status='叶级有限反例已核，原片 d72 OOM，整片待结算',
                   next_action='保留叶级见证与原片 d72 资源证据，整片认证待取')
    fixed_report = RECI_FIXED_D72 / 'RUN.md'
    if row['verdict'] == 'pending' and fixed_report.is_file():
        fixed = RECI_FIXED_D72 / 'out'
        assert int((fixed / 'd72.memory.peak').read_text()) == 24 * 1024 ** 3
        assert 'oom_kill 1' in (fixed / 'd72.memory.events').read_text().splitlines()
        systemd_log = (fixed / 'systemd.log').read_text()
        assert 'Finished with result: oom-kill' in systemd_log and 'Service runtime: 4min 45.184s' in systemd_log
        phases = {r['phase']: int(r['monotonic_ns']) for r in rows(fixed / 'phase-boundaries.tsv')}
        assert f"{(phases['property_return'] - phases['property_enter']) / 1e6:.3f}" == '54.663'
        transcript = (fixed / 'd72.log').read_text()
        assert 'Solving with SMT2 QF_AUFBV using Yices' in transcript
        assert not re.search(r'(?im)^\s*(?:SAT|UNSAT|satisfiable|unsatisfiable)\s*$', transcript)
        assert not list(fixed.glob('*.vcd'))
        row['detail'] += ('。固定 #72 监视器唯一实际 d72 的性质展开为 54.663 毫秒，已进入 Yices。'
                          '整个服务 285.184 秒达到 24 GiB 硬限并 OOM，无 SAT/UNSAT 或 VCD')
        row.update(other_evidence=rel(fixed_report),
                   work_status='原式与固定 #72 的 d72 均 OOM，整片待结算',
                   next_action='保留原式 Properties 与固定 #72 的 Yices 两阶段 OOM，以及叶级有限见证')

    if choices.get('6') == 'a' and RECI_UNDRIVEN_AUDIT.is_file():
        record_reci_undriven_refutation(row)


def record_reci_undriven_refutation(row):
    run = RECI_UNDRIVEN_RUN / 'runs/case31-lz2'
    zero_control = run.with_name('case31-lz0')
    local = RECI_UNDRIVEN_RUN.parent / 'vec-split5/out/split_v_reci_v_r5-lane0-bmc'
    paths = tuple(path / name for path in (run, zero_control)
                  for name in ('command.txt', 'program.mem', 'samples.tsv', 'replay.vcd')) + (local / 'stdout',)
    manifest = dict(reversed(line.split(maxsplit=1))
                    for line in (RECI_UNDRIVEN_RUN / 'evidence.sha256').read_text().splitlines())
    for path in paths:
        name = ('../' + path.relative_to(RECI_UNDRIVEN_RUN.parent).as_posix()
                if path == local / 'stdout' else path.relative_to(RECI_UNDRIVEN_RUN).as_posix())
        assert sha256(path.read_bytes()).hexdigest() == manifest[name], name
    assert '+lz_hi=2' in (run / 'command.txt').read_text() and '+lz_hi=0' in (zero_control / 'command.txt').read_text()
    assert (run / 'program.mem').read_bytes() == (zero_control / 'program.mem').read_bytes()
    samples = {int(item['sample']): item for item in rows(run / 'samples.tsv')}
    quiet = ('ele', 'red', 'a_valid', 'b_valid', 's_valid')
    assert all(samples[cycle]['rst'] == '0' for cycle in range(152, 167))
    assert (samples[152]['ele'], samples[152]['broadcast']) == ('5', '1')
    assert tuple(samples[154][signal] for signal in ('a_valid', 'b_valid', 's_valid')) == ('1', '0', '1')
    assert all(samples[cycle][signal] == '0' for cycle in range(155, 166) for signal in quiet)
    assert samples[164]['write_req'] == '1'
    assert (samples[166]['actual'][-3:], samples[166]['reference'][-3:]) == ('01f', '7df')
    zero, = (item for item in rows(zero_control / 'samples.tsv') if item['sample'] == '166')
    assert zero['actual'] == zero['reference']
    assert '--bound 24 --finite-prefix' in (local / 'command.txt').read_text()
    assert re.search(r'(?m)^\[split_v_reci_v_r5\.data_lane\[0\]\.lane\].*: REFUTED$', (local / 'stdout').read_text())

    wall = re.search(r'walltime ([\d.]+) s', (run / 'sim.log').read_text())[1]
    detail = (
        '合法程序 #152–#166 完整命中原窗口，#166 lane0 实际 0x01F、原参照 0x7DF。'
        '原 SALT 沿用默认 --undriven seq，fp_reciprocal.leading_zeros[5:4] 无驱动且每拍自由，'
        '整片复放为这两位选取原模型允许的 2\'b10。局部 lane0 有限 BMC d24 亦 REFUTED。'
        'Verilator 默认两态将这两位置 0，同程序对照通过，因此此前仿真未见失配。'
        '按第 6 条 a 收为有限反例，严格无限延拓证书未取得。历史整片结果。' + row['detail']
    )
    row.update(measurement('refuted', 'Verilator 5.037 · 原片复放并选定未驱动值', '', wall, '',
                           'Verilator 仿真墙钟', '', '', detail, run / 'samples.tsv', local / 'stdout'))
    row.update(waveform=rel(run / 'replay.vcd'), alternative_evidence=rel(RECI_UNDRIVEN_AUDIT),
               work_status='按第 6 条 a 收账', next_action='保留原模型未驱动语义、有限反例及严格认证状态',
               eta_utc='已结算')


def record_fp_preserve_prefix(by_property):
    old = FP_PRESERVE_PREFIX / 'd24/outcome.tsv'
    new = FP_PRESERVE_RECEIVED / 'd24/outcome.tsv'
    if not all(path.is_file() for path in (old, new, FP_PRESERVE_PREFIX_AUDIT)):
        return
    third = FP_PRESERVE_FP_STALL / 'd24/outcome.tsv'
    fourth = FP_PRESERVE_MATRIX_CW / 'd24/outcome.tsv'
    fourth_reviewed = fourth.is_file() and FP_PRESERVE_MATRIX_CW_AUDIT.is_file()
    fourth_excluded = fourth_reviewed and FP_PRESERVE_WRITE_REDUCTION_AUDIT.is_file()
    outcomes = [
        (old, ('8', '11', 'SUB_FP', '3.03', '590270464')),
        (new, ('6', '9', 'MUL_FP', '4.67', '588910592')),
    ]
    if third.is_file():
        outcomes.append((third, ('14', '17', 'MUL_FP', '2.85', '594030592')))
    if fourth_reviewed:
        outcomes.append((fourth, ('10', '13', 'ADD_FP', '3.38', '589328384')))
    for path, expected in outcomes:
        result, = rows(path)
        assert tuple(result[key] for key in ('depth', 'raw_final', 'finite_witness', 'oom_kill')) == (
            '24', 'PROVED', '1', '0'
        ), result
        assert tuple(result[key] for key in ('c0', 'c3', 'operation', 'wall_s', 'cgroup_peak_bytes')) == expected, result
        assert (path.parent / result['vcd']).is_file()
        assert (path.parent / 'atomic-samples.tsv').is_file()
    assert 'received_v_reduct_result=1' in FP_PRESERVE_PREFIX_AUDIT.read_text()
    detail = ('。公共前缀第一次 d24 原 cover PROVED，局部 SUB 命中 #8–11。'
              'Opus 以 #6 received=1 与 v_s_out_valid=0 的原片反馈矛盾排除旧轨迹。'
              '仅将 received 接为 v_s_out_valid 与自由位相与，再做一次 d24，原 cover PROVED，'
              '局部 MUL 命中 #6–9，4.67 秒、峰值 588,910,592 B、无 OOM')
    if FP_PRESERVE_RECEIVED_AUDIT.is_file():
        audit = FP_PRESERVE_RECEIVED_AUDIT.read_text()
        assert '#2 拍的自由反馈' in audit and 'stall_req_from_fp=1' in audit
        detail += '。Opus 以 #2 stall_req_from_fp=1 与原复位时序冲突排除第二条轨迹'
    if third.is_file():
        detail += ('。保留 received 并接回原 FP stall 寄存时序后的第三次 d24 原 cover PROVED，'
                   '局部 MUL 命中 #14–17，2.85 秒、峰值 594,030,592 B、无 OOM')
    latest = third if third.is_file() else new
    latest_audit = FP_PRESERVE_PREFIX_AUDIT
    next_action = '保留旧局部轨迹排除证据与最新 VCD，等待 Opus 核原片约束'
    work_status = '局部前缀有新候选，整片待结算'
    eta = '待新候选原片约束独立复核'
    if fourth_reviewed:
        audit = FP_PRESERVE_MATRIX_CW_AUDIT.read_text()
        assert '局部见证 #10–13' in audit and 'port_a=1' in audit and '反推出来的' in audit
        assert '#6 的' in audit and 'v_s_out_valid=1' in audit
        detail += ('。第四次 d24 原 cover PROVED，局部 ADD 命中 #10–13，3.38 秒、峰值 589,328,384 B、无 OOM。'
                   'VCD 结构体高位为 x，#7、#8、#10 的 port_a=1 由唯一停顿项 PC:169 反推')
        latest = fourth
        latest_audit = FP_PRESERVE_MATRIX_CW_AUDIT
        next_action = '核对 #6 received/valid 和 port_a 的原片约束，第五次暂缓'
        if not fourth_excluded:
            detail += '。#6 的 received/valid 和 port_a 的原片可实现性均待查'
    if fourth_excluded:
        audit = FP_PRESERVE_WRITE_REDUCTION_AUDIT.read_text()
        assert '最早在 #10' in audit and '在 #7–10 为 0' in audit
        detail += ('。Opus 已排除第四次局部轨迹的原片可实现性。#1 的归约最早可在 #10 产生输出，'
                   '#6 的 v_s_out_valid=1 和 received=1 提前。执行级缺少 MM_WO 与向量元素写来源，'
                   '#7、#8、#10 的 port_a 在原片均为 0。完整原前件及后件待证')
        latest_audit = FP_PRESERVE_WRITE_REDUCTION_AUDIT
        next_action = '保留两条排除证据及静态边界映射，第五次暂缓'
        work_status = '第四次局部候选已排除，整片待结算'
        eta = '待静态边界独立复核及原片证据'
    else:
        detail += '。最新局部候选的原片可实现性、完整原前件及后件均待证'
    for op in ('add', 'sub', 'max', 'mul'):
        row = by_property[f'plena_isa_chip_fp_preserve.s_{op}_fp__chip_other_register']
        if row['verdict'] == 'pending':
            row['detail'] += detail
            row.update(work_status=work_status,
                       next_action=next_action,
                       eta_utc=eta, alternative_evidence=rel(latest_audit),
                       other_evidence=rel(latest))


def fp_prefix_proof_evidence():
    for label, pdr_run, properties in FP_LONG_PDR_CASES:
        for engine, run in (('长 PDR', pdr_run), ('k-induction（k≤24）', pdr_run.with_name('kind-k24'))):
            outcome = run / 'outcome.tsv'
            if not outcome.is_file():
                continue
            result, = rows(outcome)
            report = run / 'README.md'
            detail = f"{label}的投影 {engine} 终态为 {result['final']}，{result['elapsed_s']} 秒"
            if result['detail'] and result['vcd'] == 'none':
                detail += f"（{result['detail']}）"
            detail += f"，cgroup 峰值 {int(result['peak_bytes']):,} B，oom_kill={result['oom_kill']}"
            if result['vcd'] == 'none':
                detail += '，无 VCD'
            if result['service_result'] == 'timeout' or 'timeout' in result['detail'].lower():
                detail += '。本轮到限收口，整片待结算'
            elif result['final'] == 'proved':
                detail += '。投影不可达已证，提升待独立复核，整片待结算'
            elif result['final'] == 'refuted':
                detail += '。投影反例的原片可实现性待核，整片待结算'
            yield properties, report, detail


def record_fp_prefix_proofs(by_property):
    for properties, report, detail in fp_prefix_proof_evidence():
        for name in properties:
            row = by_property[name]
            if row['verdict'] == 'pending':
                row['detail'] += '。' + detail
                row['other_evidence'] = rel(report)

    results = rows(FP_GATED_PREFIX / 'results.tsv')
    finite = [result for result in results if result['engine'] == 'finite']
    assert len(finite) == 2 and all(result['final'] == 'REFUTED' and result['base'] == 'SAT' for result in finite)
    numerical = [name for name in by_property if name.startswith(FLOAT_BATCH + '.') and name != FLOAT_ZERO]
    preserve = [name for name in by_property if name.startswith('plena_isa_chip_fp_preserve.')]
    assert len(numerical) == len(preserve) == 9
    for name in numerical + preserve:
        row = by_property[name]
        if row['verdict'] == 'pending':
            row['detail'] += '。门控投影 d24 仍可达；旧包排空引理不成立；待 decisions 第 8 条'
            row.update(work_status='原模型旧包路径未排除', next_action='等 decisions 第 8 条',
                       eta_utc='待第 8 条决定', other_evidence=rel(FP_GATED_PREFIX / 'README.md'))


def record_g1_programs(by_property):
    for op in ('add', 'lui'):
        run = G1_PROGRAMS / op / 'max-fp'
        result = run / 'result.txt'
        if not result.is_file():
            continue
        row = by_property[f'plena_isa_chip_int_preserve_g1.s_{op}_int__chip_other_register']
        if row['verdict'] != 'pending':
            continue
        assert 'c0 #144, c10 #154, c11 #155; GP reads gp1 at c3/c9, operation rd=gp3' in result.read_text(), result
        assert 'Verified 240 VCD rising-edge samples' in (run / 'verify-wave.py.log').read_text(), run
        row['detail'] += f'。{op.upper()} 定向程序按原取指接口在 #144–154 命中整片前件，#151 写 gp3，两次读 gp1；逐拍 VCD 核对通过'
        row['next_action'] = '拆分写回仲裁、前递和同址读回安全义务，另证原片触发可无限延拓'
        row['eta_utc'] = '待首轮安全义务和闭环复核'
        row['work_status'] = '整片有限前件已证，安全性与无限延拓待证'
        row['other_evidence'] = rel(result)
        row['waveform'] = rel(run / 'chosen.vcd')
        if G1_OBLIGATIONS_REPORT.is_file():
            control, = (G1_OBLIGATIONS / f'g1_{op}_control').glob('crisp-out/*/progress-*.tsv')
            read, = (G1_OBLIGATIONS / f'g1_{op}_read').glob('crisp-out/*/progress-*.tsv')
            control_event, = final_rows(control)
            read_event, = final_rows(read)
            assert (control_event['property'], control_event['status']) == (f'g1_{op}_control.writeback_isolation', 'proved'), control
            assert (read_event['property'], read_event['status'], read_event['detail']) == (
                f'g1_{op}_read.readback_isolation', 'unknown', 'proved; vacuity check timed out'
            ), read
            row['detail'] += '。写回隔离与回读隔离均已由局部字级归纳证得安全性。回读的 Crisp 前件认证在 44.001 秒到限，CLI 与 final 为 unknown'
            row.update(
                next_action='独立复核两项义务对原事件的逐拍提升，再认证定向轨迹的原片无限延拓',
                eta_utc='待独立提升审阅及整片闭环', work_status='局部安全义务已证，提升及原片闭环待证',
                alternative_evidence=rel(G1_OBLIGATIONS_REPORT),
            )
            if G1_LIFT_AUDIT.is_file():
                assert '两项无界安全性组合提升均成立（SOUND）' in G1_LIFT_AUDIT.read_text()
                row['detail'] += '。独立复核确认两义务逐拍使用相同的 24 项卫条件，零新增假设，安全性已提升至原片'
                row.update(
                    next_action='复用 STORE 的缩短取指程序布局，在原片保留冷复位并认证本条前件的完整状态回边',
                    work_status='整片无界安全性已证，前件无限延拓待证',
                    alternative_evidence=rel(G1_LIFT_AUDIT),
                )
        if (FIRST_FILL_G1 / 'results.tsv').is_file():
            record_single_fill(row, FIRST_FILL_G1, op)


def record_single_fill(row, run, target):
    result, = (item for item in rows(run / 'results.tsv') if item['target'] == target)
    assert [result[key] for key in ('c0', 'c7_GP_write', 'c10', 'c11', 'stable_from', 'next_stable_sample')] == [
        '37', '44', '47', '48', '68', '69',
    ], result
    assert result['GP_write_rd'] == '3' and result['c5_gp_out_1_hex'] == result['c11_gp_out_1_hex'], result
    row['detail'] += (f"。单事件首块 #37–47 命中，#44 实写 gp3=0x{result['GP_write_data_hex']}，"
                      '#48 后件相等。#68→#69 为仿真回边候选')
    row.update(next_action='用首块程序准备原模型 #68→#69 全状态闭环认证',
               other_evidence=rel(run / 'README.md'), waveform=rel(run / target / 'trace.vcd'))


def check_first_fill(target):
    event, = (record for record in rows(FIRST_FILL / 'prediction-vs-observation.tsv') if record['target'] == target)
    assert (event['matched_c0'], event['matched_c11'], event['result']) == ('37', '48', 'consequent_equal'), event


def record_sub_obligations(by_property):
    report = SUB_OBLIGATIONS / 'README.md'
    row = by_property['plena_isa_chip_int_preserve_g2.s_sub_int__chip_other_register']
    if row['verdict'] != 'pending' or not report.is_file():
        return
    control, = (SUB_OBLIGATIONS / 'control').glob('crisp-out/*/progress-*.tsv')
    read, = (SUB_OBLIGATIONS / 'read').glob('crisp-out/*/progress-*.tsv')
    control_event, = final_rows(control)
    read_event, = final_rows(read)
    assert (control_event['property'], control_event['status']) == ('g2_sub_control.writeback_isolation', 'proved')
    assert (read_event['property'], read_event['status'], read_event['detail']) == (
        'g2_sub_read.readback_isolation', 'unknown', 'proved; vacuity check timed out',
    )
    row['detail'] += '。局部写回隔离在 0.172 秒证毕，读回安全性也已证，局部前件认证 54.001 秒到限，读回 final 为 unknown'
    row.update(work_status='局部安全义务已证，提升及原片闭环待证', eta_utc='待独立提升审阅、原片定向触发及延拓',
               next_action='独立比对同一原事件的 24 项卫条件，复用 G1 程序夹具取得整片完整 SUB 前件',
               alternative_evidence=rel(report), other_evidence=rel(read))
    if (SUB_PROGRAM / 'window.tsv').is_file():
        window = {sample['relative_sample']: sample for sample in rows(SUB_PROGRAM / 'window.tsv')}
        assert [sample['sample'] for sample in window.values()] == [str(i) for i in range(144, 156)]
        assert (window['c7']['gp_reg_wen'], window['c7']['gp_reg_waddr'], window['c7']['gp_reg_wdata']) == ('1', '3', '0')
        assert window['c5']['gp_out_1'] == window['c11']['gp_out_1'] == '00000091'
        row['detail'] += '。原片仿真 #144–154 命中完整前件，#151 写 gp3=0，#149/#155 均读到 0x91。#175→#176 是仿真回边候选'
        row.update(eta_utc='待独立提升审阅及原模型延拓',
                   next_action='独立复核局部安全性提升；核对能否用短前缀布局做原模型闭环认证',
                   other_evidence=rel(SUB_PROGRAM / 'README.md'), waveform=rel(SUB_PROGRAM / 'sub.vcd'))
    if SUB_LIFT_AUDIT.is_file():
        assert '无界安全性组合提升成立（SOUND，0 缺口）' in SUB_LIFT_AUDIT.read_text()
        row['detail'] += '。两项局部安全性经独立复核可提升至原片，24 项卫条件逐拍一致'
        row.update(work_status='整片无界安全性已证，前件无限延拓待证', alternative_evidence=rel(SUB_LIFT_AUDIT),
                   next_action='核对原片触发及原模型全状态闭合')
    if (FIRST_FILL / 'prediction-vs-observation.tsv').is_file():
        check_first_fill('sub')
        row['detail'] += '。旧首块三指令串接 #37–47 也命中，#48 读回相等，#100→#101 为该旧布局的仿真回边候选'
        row.update(next_action='验证短程序尾段，准备原模型全状态闭合', other_evidence=rel(FIRST_FILL / 'README.md'))
    if (FIRST_FILL_G2 / 'results.tsv').is_file():
        record_single_fill(row, FIRST_FILL_G2, 'sub')
    if FIRST_FILL_STORE_SUB_AUDIT.is_file():
        row['detail'] += '。Neon 独立复核旧串接 SUB 的有限前件，单事件布局的仿真回边候选为 #68→#69'
        row['other_evidence'] = rel(FIRST_FILL_STORE_SUB_AUDIT)


def record_addi_first_fill(by_property):
    row = by_property['plena_isa_chip_int_preserve_g2.s_addi_int__chip_other_register']
    if row['verdict'] != 'pending' or not (FIRST_FILL_G2 / 'results.tsv').is_file():
        return
    record_single_fill(row, FIRST_FILL_G2, 'addi')
    row.update(work_status='原片有限前件已证，安全性和无限延拓待证',
               next_action='验证 ADDI 原写回与读回安全义务；另认证首块原模型严格延拓')
    report = ADDI_OBLIGATIONS / 'README.md'
    if report.is_file():
        control, = (ADDI_OBLIGATIONS / 'control').glob('crisp-out/*/progress-*.tsv')
        read, = (ADDI_OBLIGATIONS / 'read').glob('crisp-out/*/progress-*.tsv')
        control_event, = final_rows(control)
        read_event, = final_rows(read)
        assert (control_event['property'], control_event['status']) == ('g2_addi_control.writeback_isolation', 'proved')
        assert (read_event['property'], read_event['status'], read_event['detail']) == (
            'g2_addi_read.readback_isolation', 'unknown', 'proved; vacuity check timed out',
        )
        row['detail'] += ('。两项局部主安全性均已证，写回 final proved，读回的局部前件认证 114.001 秒到限，final unknown；'
                          '串行 114.198 秒、峰值 189.03 MiB')
        row.update(work_status='局部安全义务已证，提升及原片闭环待证', eta_utc='待独立提升审阅和首块严格延拓',
                   next_action='独立复核局部义务逐拍提升；认证首块输入的原模型回边',
                   alternative_evidence=rel(report))
        if ADDI_LIFT_AUDIT.is_file():
            audit = ADDI_LIFT_AUDIT.read_text()
            assert '无界安全性组合提升成立（SOUND，0 缺口）' in audit and '24 项' in audit
            row['detail'] += '。Neon 独立复核确认 24 项逐拍交接可提升为整片无界安全性，原模型严格无限延拓待证'
            row.update(work_status='整片无界安全性已证，前件无限延拓待证', eta_utc='待首块严格延拓',
                       next_action='认证已命中首块前件的原模型严格延拓', alternative_evidence=rel(ADDI_LIFT_AUDIT))
        if ADDI_OPUS_AUDIT.is_file():
            audit = ADDI_OPUS_AUDIT.read_text()
            assert '在 VCD 上独立重采样' in audit and '#37–#47 命中前件' in audit
            row['detail'] += '。Opus 独立重采样命中原片 #37–47 完整前件，#48 后件成立'
            row['other_evidence'] = rel(ADDI_OPUS_AUDIT)


def record_loop_obligations(by_property, target, run):
    row = by_property[f'plena_isa_chip_int_preserve_g2.c_{target}__chip_other_register']
    report = run / 'README.md'
    if row['verdict'] != 'pending' or not report.is_file():
        return
    control, = (run / 'control').glob('crisp-out/*/progress-*.tsv')
    read, = (run / 'read').glob('crisp-out/*/progress-*.tsv')
    control_event, = final_rows(control)
    read_event, = final_rows(read)
    assert (control_event['property'], control_event['status']) == (f'g2_{target}_control.writeback_isolation', 'proved')
    assert (read_event['property'], read_event['status'], read_event['detail']) == (
        f'g2_{target}_read.readback_isolation', 'unknown', 'proved; vacuity check timed out',
    )
    row['detail'] += '。两项局部主安全性均已证，写回 final 为 proved，读回的局部前件认证在 119 秒到限，final 为 unknown'
    row.update(work_status='局部安全义务已证，提升及原片闭环待证', alternative_evidence=rel(report))
    if target == 'loop_start':
        record_single_fill(row, FIRST_FILL_LOOP_START, 'loop-start')
        row.update(eta_utc='待独立提升审阅和首块严格延拓',
                   next_action='独立复核写回和同址回读义务的逐拍提升，认证原模型回边')
        if LOOP_START_LIFT_AUDIT.is_file():
            audit = LOOP_START_LIFT_AUDIT.read_text()
            assert '两项局部义务可以组合提升为整片无界安全结论' in audit and '24 项交接事实' in audit
            assert '119.001 s' in audit
            row['detail'] = row['detail'].replace('局部前件认证在 119 秒到限', '局部前件认证在 119.001 秒到限')
            row['detail'] += '。Opus 独立复核确认 24 项逐拍交接可提升为整片无界安全性，原模型严格无限延拓待证'
            row.update(work_status='整片无界安全性已证，前件无限延拓待证', eta_utc='待首块严格延拓',
                       next_action='认证已命中首块前件的原模型严格延拓',
                       alternative_evidence=rel(LOOP_START_LIFT_AUDIT))
    else:
        row.update(eta_utc='待独立提升审阅、整片定向前件与严格延拓',
                   next_action='独立复核局部提升，并定向重放原循环退出及两次 PASS 的窗口')
        results = FIRST_FILL_LOOP_END / 'results.tsv'
        if results.is_file():
            sample, = rows(results)
            assert tuple(sample[key] for key in ('target', 'c3_PASS1', 'c5_LOOP_DEC', 'c7_GP_write',
                                                'required_c9_PASS2', 'observed_PASS2', 'complete_original_hits')) == (
                'loop-end', '57', '59', '61', '63', '67', '0',
            )
            row['detail'] += ('。原片单次定向复放完成循环退出，#57 首次 PASS、#59 LOOP_DEC、#61 写 gp3。'
                              'FP 停顿使第二次 PASS 从要求的 #63 延至 #67，完整前件命中 0 次，无原后件判定')
            row.update(other_evidence=rel(FIRST_FILL_LOOP_END / 'README.md'),
                       next_action='保留已测布局，先静态核对能否把第二次 PASS 对齐原 c9，再安排复放')
        if LOOP_END_LIFT_AUDIT.is_file():
            audit = LOOP_END_LIFT_AUDIT.read_text()
            assert '两项局部义务可以独立提升到整片无界安全' in audit
            assert '119.001 s' in audit and 'complete_original_hits=0' in audit
            row['detail'] = row['detail'].replace('局部前件认证在 119 秒到限', '局部前件认证在 119.001 秒到限')
            row['detail'] += '。Opus 独立复核确认 24 项逐拍交接可提升为整片无界安全性，原片触发及严格延拓待证'
            row.update(work_status='整片无界安全性已证，原片前件和无限延拓待证',
                       eta_utc='待复核新布局逐拍推导、原片触发及严格延拓', alternative_evidence=rel(LOOP_END_LIFT_AUDIT),
                       next_action='核对新布局的逐拍推导后安排一次短仿真')
        if (FIRST_FILL_LOOP_END_A / 'results.tsv').is_file():
            observed, = rows(FIRST_FILL_LOOP_END_A / 'results.tsv')
            assert tuple(observed[key] for key in ('complete_original_hits', 'stall_59_62', 'op_at_63',
                                                  'gp_write_addr_at_61')) == ('0', '1,1,1,1', '0', '3'), observed
            row['detail'] += ('。候选 A 仅改 #10 指令 D，#47 变 PASS。#59–62 停顿仍在，#63 仍 idle，'
                              '完整前件 0 命中。Opus 核对 word[7] 在 #48/#55 两次派发，#59 FP exe tag=9、determine tag=8')
            row.update(next_action='保留两次短复放和重复派发证据，后续布局复跑暂停',
                       eta_utc='待原片前件可达证据及严格延拓',
                       other_evidence=rel(FIRST_FILL_LOOP_END_A / 'README.md'),
                       waveform=rel(FIRST_FILL_LOOP_END_A / 'loop-end/trace.vcd'))


def record_mul_obligations(by_property):
    row = by_property['plena_isa_chip_int_preserve_g2.s_mul_int__chip_other_register']
    report = MUL_OBLIGATIONS / 'README.md'
    if row['verdict'] != 'pending' or not report.is_file():
        return
    control, = (MUL_OBLIGATIONS / 'control').glob('crisp-out/*/progress-*.tsv')
    read, = (MUL_OBLIGATIONS / 'read').glob('crisp-out/*/progress-*.tsv')
    control_event, = final_rows(control)
    read_event, = final_rows(read)
    assert (control_event['property'], control_event['status']) == ('g2_mul_control.writeback_isolation', 'proved')
    assert (read_event['property'], read_event['status'], read_event['detail']) == (
        'g2_mul_read.readback_isolation', 'unknown', 'proved; vacuity check timed out',
    )
    assert '119.001 s  proved; vacuity check timed out' in (MUL_OBLIGATIONS / 'read/stdout').read_text()
    row['detail'] += ('。局部写回隔离 final proved，读回主安全性 proved，局部前件认证在 119.001 秒到限，'
                      '读回 final unknown。25 项逐拍交接已交独立审阅')
    row.update(work_status='局部安全义务已证，提升及原片闭环待证', alternative_evidence=rel(report),
               other_evidence=rel(read), eta_utc='待独立提升审阅、原片前件及严格延拓',
               next_action='独立复核 25 项逐拍交接，在原片首块取得完整 MUL 前件并另行认证回边')
    if (FIRST_FILL_MUL / 'results.tsv').is_file():
        record_single_fill(row, FIRST_FILL_MUL, 'mul')
    if MUL_LIFT_AUDIT.is_file():
        row['detail'] = row['detail'].replace('25 项逐拍交接已交独立审阅', '25 项逐拍交接经独立审阅可提升')
        row['detail'] += '。整片无界安全性已证，原模型严格无限延拓待证'
        row.update(work_status='整片无界安全性已证，前件无限延拓待证',
                   alternative_evidence=rel(MUL_LIFT_AUDIT), other_evidence=rel(read),
                   next_action='保留原片有限触发及两项安全义务，原模型严格无限延拓待证')


def record_shared_rst_lasso(by_property):
    summary = RST_LASSO / 'SUMMARY.txt'
    if not summary.is_file():
        return
    log = (RST_LASSO / 'stdout.log').read_text()
    assert 'pm_rst_never: witness loop 0 -> 1 (equal endpoints), bad frame 0' in log
    assert 'u_ant_probe.rst: REFUTED' in log
    waveform, = RST_LASSO_RUN.glob('*counterexample.vcd')
    prefix = 'plena_kinds_chip_l1_spec.'
    pm = by_property[prefix + 'pm_prefetch_valid_reset']
    proof = PM_CONE_ARCHIVE / 'progress-yhFYsE.tsv'
    result, = final_rows(proof)
    assert (result['property'], result['status']) == ('pm_reset_cone.pm_prefetch_valid_reset', 'proved'), result
    pm.update(
        engine='Crisp / EBMC new IC3/PDR · 手工锥投影与独立接线复核',
        wall_s='0.03', peak_kib='17608', resource_scope='手工锥投影，GNU time 最大 RSS',
        crisp_sha='094d7e4d49b16ba86e5576db41f7bc560b1942aa',
        ebmc_sha='83e914af6d26da76b84559d1665cede1efc44e99', evidence=rel(proof),
        alternative_evidence=rel(PM_CONE_REPORT),
        detail='手工锥投影 PDR 已证安全性，独立接线复核确认可提升到原片。#55 原 u_dut 短试仍为 8 GiB 限额下的 unknown',
    )
    for name in ('pm_prefetch_valid_reset', 'pv_ele_a_reset', 'pm_scale_d_ready_reset'):
        row = by_property[prefix + name]
        if row is not pm:
            assert row['crisp_sha'] == CRISP_40 and any(
                event['property'] == row['property'] and event['status'] == 'proved'
                for event in final_rows(ROOT / row['evidence'])
            ), row
            row['detail'] = row['detail'].removesuffix('。原整片前件 rst 的无限延拓待证')
        row['detail'] += '。同一无显式复位、零假设的原片 rst 在 #0 触发，原生全状态严格闭环 #0→#1（25.052 秒，cgroup 峰值 1154.57 MiB）'
        row.update(verdict='proved', depth='', other_evidence=rel(summary), waveform=rel(waveform),
                   next_action='保留安全证明、接线复核与原片严格闭环证据', eta_utc='已结算', work_status='整片无界已证')
    pv = by_property[prefix + 'pv_prefetch_valid_reset']
    if pv['verdict'] == 'pending':
        pv['detail'] += '。已获同包装同前件的原片严格闭环 #0→#1，整片无界安全性待证'
        pv.update(alternative_evidence=rel(summary), waveform=rel(waveform),
                  next_action='核对向量预取复位锥投影能否提升，补无界安全证明',
                  eta_utc='待复位锥接线与证明', work_status='原片前件闭环已证，无界安全性待证')
        if PV_CONE_REPORT.is_file():
            journal, = PV_CONE_RUN.glob('crisp-out/*/progress-*.tsv')
            event, = final_rows(journal)
            assert (event['property'], event['status']) == ('pv_reset_cone.pv_prefetch_valid_reset', 'proved'), event
            pv['detail'] = pv['detail'].removesuffix('，整片无界安全性待证')
            pv['detail'] += '。三片原 register_slice、原精度选择与 join2 的 PDR 已证切点安全性，0.03 秒、17,640 KiB'
            pv.update(alternative_evidence=rel(PV_CONE_REPORT), other_evidence=rel(summary),
                      next_action='复核实际切点与已审原接口映射，核对可提升后收账',
                      eta_utc='待实际切点核对', work_status='原片闭环和切点 PDR 已证，待提升复核')
            if PV_CONE_AUDIT.is_file() and '可无条件提升（SOUND，无缺项）' in PV_CONE_AUDIT.read_text():
                resource = dict(line.split('=', 1) for line in (PV_CONE_RUN / 'cone.time').read_text().splitlines())
                versions = dict(line.split('=', 1) for line in (PV_CONE_RUN / 'VERSIONS.txt').read_text().splitlines()
                                if line.startswith(('crisp_head=', 'hw_cbmc=')))
                assert resource['exit'] == '0' and not (PV_CONE_RUN / 'cone.stderr').stat().st_size
                pv['detail'] = ('原三片缓存、精度选择及 join2 的切点 PDR 已证。独立逐字核对确认可无条件提升至原整片。'
                                '同一无显式复位、零假设模型在 #0 触发 rst，严格全状态回边 #0→#1')
                pv.update(
                    verdict='proved', depth='', engine='Crisp / EBMC new IC3/PDR · 手工锥投影与独立接线复核',
                    wall_s=resource['wall_s'], peak_kib=resource['maxrss_kib'],
                    resource_scope='手工锥投影，GNU time 最大 RSS',
                    crisp_sha=versions['crisp_head'], ebmc_sha=versions['hw_cbmc'], evidence=rel(journal),
                    next_action='保留切点证明、接线复核与原片严格闭环证据', eta_utc='已结算', work_status='整片无界已证',
                )


def record_exp_repeat_release(by_property):
    if not EXP_REPEAT_REPORT.is_file():
        return
    report = EXP_REPEAT_REPORT.read_text()
    if '01f535662700fa1351b5ba0155b1280aaed805d7' not in report:
        return
    for name in ('plena_isa_chip_fp.s_exp_fp__chip_result', 'plena_isa_chip_fp_preserve.s_exp_fp__chip_other_register'):
        row = by_property[name]
        if row['verdict'] != 'pending':
            continue
        row['detail'] += '。EBMC Spec 12 发布版 01f53566 的验收构建通过原语法拒绝点，原样局部转而在 Spot 119.01 秒到限，未进入 IC3'
        row.update(work_status='已通过 EBMC 转换，Spot 限时', alternative_evidence=rel(EXP_REPEAT_REPORT),
                   next_action='分别核对寄存器写回控制与 SFU 数值切点，EBMC 定位 Spot 转换耗时', eta_utc='待局部义务校验')
        case = 'preserve' if name.endswith('other_register') else 'result'
        stage = EXP_SPOT_WINDOW / case / 'stage.txt'
        if stage.is_file() and 'property_check_started=1 result_line=0' in stage.read_text():
            assert stage.with_name('rc').read_text().strip() == '124'
            row['detail'] += '。Spec 14 原型已进入 new IC3，原 119 秒限时内无性质判定'
            row.update(work_status='Spec 14 原型已进 IC3，局部判定待证', other_evidence=rel(stage))
        if EXP_SPOT_RELEASE.is_file() and '2d274b5a8e73f36e9ea99b533ff9fd7f840c2fc9' in EXP_SPOT_RELEASE.read_text():
            row['detail'] += '。Spec 14 现已发布 2d274b5a，源树与验收 head 一致，排 Crisp gitlink'
            row['other_evidence'] = rel(EXP_SPOT_RELEASE)
    row = by_property['plena_isa_chip_fp_preserve.s_exp_fp__chip_other_register']
    if row['verdict'] == 'pending' and FP_EXP_ISOLATION_REPORT.is_file():
        control, = (FP_EXP_ISOLATION_RUN / 'fp_exp_control').glob('crisp-out/*/progress-*.tsv')
        read, = (FP_EXP_ISOLATION_RUN / 'fp_exp_read').glob('crisp-out/*/progress-*.tsv')
        c, = final_rows(control)
        r, = final_rows(read)
        assert (c['property'], c['status'], c['detail']) == (
            'fp_exp_control.check_control', 'unknown', 'k-induction inconclusive at depth 32'
        ), c
        assert (r['property'], r['status']) == ('fp_exp_read.check_read', 'proved'), r
        row['detail'] += '。原匹配监视器的写回隔离 kind d32 不收敛，0.335 秒。相同隔离守卫下原 RF 回读已证，0.589 秒'
        row.update(next_action='按原复位及 ALU/SFU/LOAD 仲裁分拆控制义务，定位最早归纳缺口',
                   work_status='局部有条件回读已证，写回隔离与原片前件待证', alternative_evidence=rel(FP_EXP_ISOLATION_REPORT))
        if FP_EXP_INDUCTION_REPORT.is_file():
            grouped, = (FP_EXP_INDUCTION_RUN / 'fp_exp_grouped').glob('crisp-out/*/progress-*.tsv')
            results = list(final_rows(grouped))
            assert len(results) == 8 and all(item['status'] == 'passed' for item in results), grouped
            row['detail'] += '。归纳超集允许 c0 旧 SFU 向待读地址写回；保留真实复位的重放仅 c21 写 fpd。控制切点 d64 八组有界通过，1.845 秒、75.3 MiB'
            row.update(next_action='检验 c0 写穿是否仍保值，缩到必要写回区间或补旧计算飞行的可达状态不变量',
                       alternative_evidence=rel(FP_EXP_INDUCTION_REPORT))
        if FP_EXP_MIN_REPORT.is_file():
            journal, = FP_EXP_MIN_RUN.glob('crisp-out/*/progress-*.tsv')
            assert not final_rows(journal) and (FP_EXP_MIN_RUN / 'returncode.txt').read_text().strip() == '3', journal
            assert re.search(r'fp_exp_minimum\.check_control\s+unknown\s+28\.002 s\s+timeout', (FP_EXP_MIN_RUN / 'stdout').read_text())
            row['detail'] += ('。逐拍核对允许 c0 同址写穿，c2 首次样本已读到写后值。c1–c23 同址隔离加原前递与 c21 写址足以推出回读等式；'
                              '该控制投影 PDR 28.002 秒到限，无 OOM，原整片仍待证')
            row.update(next_action='保留原回读等式的条件证明，待更小的控制证据及原片前件，不重跑现有 PDR',
                       alternative_evidence=rel(FP_EXP_MIN_REPORT))
        if FP_EXP_C21.is_file():
            assert 'unknown  55.007 s  timeout after 55 s' in FP_EXP_C21.read_text()
            row['detail'] += '。旧 SFU/ALU 排空局部 PDR 4.225 秒证毕。单独 c21 目标隔离 PDR 55.007 秒到限，峰值 113,405,952 B'
            row.update(next_action='保留旧生产者排空引理和 c21 逐拍推导，后续复核组合提升与原片前件',
                       alternative_evidence=rel(FP_EXP_DECOMPOSITION), other_evidence=rel(FP_EXP_C21))


def record_pwrite_lasso(by_property):
    summary = PWRITE_LASSO / 'SUMMARY.txt'
    if not summary.is_file():
        return
    log = (PWRITE_LASSO / 'stdout.log').read_text()
    assert 'pwrite_ready_never: witness loop 0 -> 1 (equal endpoints), bad frame 0' in log
    assert 'u_ant_probe.hbm_ready_to_write: REFUTED' in log
    waveform, = PWRITE_LASSO_RUN.glob('*counterexample.vcd')
    row = by_property['plena_kinds_chip_l1_spec.pwrite_ready_scale']
    assert row['crisp_sha'] == CRISP_40 and any(
        event['property'] == row['property'] and event['status'] == 'proved'
        for event in final_rows(ROOT / row['evidence'])
    ), row
    row['detail'] = row['detail'].removesuffix('。原整片前件 u_dut.hbm_ready_to_write 的无限延拓待证')
    row['detail'] += ('。原无显式复位、零假设包装在 #0 的 hbm_ready_to_write 为 1，'
                      '严格全状态回边 #0→#1（68.851 秒，cgroup 峰值 1382.04 MiB）')
    row.update(verdict='proved', depth='', other_evidence=rel(summary), waveform=rel(waveform),
               next_action='保留局部安全证明、独立接线复核和原片严格闭环证据',
               eta_utc='已结算', work_status='整片无界已证')


def record_ext_r8_evidence(by_property):
    summary = EXT_R8_LASSO / 'SUMMARY.txt'
    if summary.is_file():
        log = (EXT_R8_LASSO / 'stdout.log').read_text()
        assert 'ext_r8_avalid_never: witness loop 0 -> 1 (equal endpoints), bad frame 0' in log
        assert any('ext_r8_no_unsolicited_first' in line and line.endswith(': ASSUMED') for line in log.splitlines())
        assert any('ext_r8_avalid_never]' in line and line.endswith(': REFUTED') for line in log.splitlines())
        waveform, = EXT_R8_LASSO_RUN.glob('*counterexample.vcd')
        row = by_property['ext_20260801_spec.ext_r8_ready_when_requesting']
        assert row['crisp_sha'] == CRISP_40 and any(
            event['property'] == row['property'] and event['status'] == 'proved'
            for event in final_rows(ROOT / row['evidence'])
        ), row
        row['detail'] = row['detail'].removesuffix('。原整片前件 !rst && instr_mem_tl_a_valid_o 的无限延拓待证')
        row['detail'] += ('。原片保留唯一假设 ext_r8_no_unsolicited_first，#0 非复位且 A valid 为 1，'
                          '严格全状态回边 #0→#1（46.143 秒，cgroup 峰值 1357.41 MiB）')
        row.update(verdict='proved', depth='', other_evidence=rel(summary), waveform=rel(waveform),
                   next_action='保留局部安全证明、独立接线复核和原片严格闭环证据',
                   eta_utc='已结算', work_status='整片无界已证')

    if EXT_ATOMIC_REPORT.is_file():
        row = by_property['ext_20260801_spec.ext_r8_accept_atomic']
        if row['verdict'] != 'pending':
            return
        proof, = EXT_ATOMIC_PROOF.glob('crisp-out/*/progress-*.tsv')
        event, = final_rows(proof)
        assert (event['property'], event['status']) == ('ext_r8_atomic_cut.ext_r8_accept_atomic', 'proved'), event
        row['detail'] += ('。原同拍 SVA 以自由端口 PDR 证明无界安全性。Ext 请求闭环的 A ready 为 0，'
                          '握手前件需另证，先试一次握手后进入静止回边')
        row.update(next_action='保留原唯一假设，认证一次接受握手及其后全状态回边',
                   work_status='整片无界安全性已证，前件无限延拓待证', alternative_evidence=rel(EXT_ATOMIC_REPORT))
        if EXT_ATOMIC_TIMEOUT.is_file():
            assert 'FINITE `#0` TRIGGER `SAT`, LASSO CERTIFICATION `TIMEOUT` AT `90 s`' in EXT_ATOMIC_TIMEOUT.read_text()
            row['detail'] += '。保留唯一假设时 BMC #0 的完整握手前件 SAT；3 帧严格回边在 Yices 到限，90.265 秒、1830.95 MiB，无 VCD'
            row.update(next_action='保留原模型有限触发，优先评估在前缀后固定单条候选回边的严格认证',
                       other_evidence=rel(EXT_ATOMIC_TIMEOUT))
        if EXT_ATOMIC_LOOP1_TIMEOUT.is_file():
            text = EXT_ATOMIC_LOOP1_TIMEOUT.read_text()
            assert 'NOT CERTIFIED' in text and '90.244 s' in text
            row['detail'] += '。只固定 #1→#2 的逐字节单行特化在 Yices 90.244 秒到限，657.93 MiB，无求解输出'
            row.update(next_action='用原 Ext 模型和唯一原假设单选做正式 BMC d2，核对同界握手前件与结论',
                       other_evidence=rel(EXT_ATOMIC_LOOP1_TIMEOUT))
        record_pr57_bounded(
            row, EXT_ATOMIC_BMC, 2,
            '原完整 RTL、无显式复位、唯一原 assume 下的正式同界非 vacuous 通过。独立原断言 d20 无反例，d20 的前件查询超时。'
            '同拍无界安全性已证，原片无限延拓两次认证超时。',
        )


def spec13_timeout(run):
    exit_code = run / 'solve.rc'
    if not exit_code.is_file() or exit_code.read_text().strip() != '124':
        return ''
    wall, _, status = (run / 'solve.time').read_text().splitlines()[-1].split()
    assert status == '124'
    peak = int((run / 'solve.memory.peak').read_text()) / (1 << 30)
    return f'正式求解 {wall} 秒到限（退出 124），cgroup 峰值 {peak:.2f} GiB，无性质判定'


def record_spec13_vector_exp(by_property):
    run = VECTOR_SPEC13_EXP
    if not (run / 'parse.rc').is_file():
        return
    assert (run / 'export.rc').read_text().strip() == (run / 'parse.rc').read_text().strip() == '0'
    assert (run / 'export.audit').read_text().split() == ['quantifier_lines', '0', 'check_sat', '1']
    assert 'spec13-parse-complete' in (run / 'parse.log').read_text()
    solve = run / 'solve.log'
    if not solve.is_file() or 'Solving with SMT2 QF_AUFBV using Yices' not in solve.read_text():
        return
    export_wall, export_peak, _ = (run / 'export.time').read_text().split()
    parse_wall, parse_peak, _ = (run / 'parse.time').read_text().split()
    row = by_property[VECTOR_EXP]
    if row['verdict'] == 'pending':
        timeout = spec13_timeout(run)
        row['detail'] += (f'。锁定 f3b352c9 的原样 d27 导出零量词，{export_wall} 秒/{export_peak} KiB；'
                          f'Yices 独立解析通过，{parse_wall} 秒/{parse_peak} KiB。'
                          + (timeout or '正式查询已进入 Yices 求解，性质判定待核'))
        row.update(next_action=('保留 d27 到限证据，优先完成 SUM/MAX，再评估局部安全义务' if timeout
                                else '保留正式 d27 求解到时限，分开核对原断言与原模型前件证据'),
                   work_status=('Yices 到限，原性质待结算' if timeout else '已解除量词解析阻塞，正式求解判定待验收'),
                   other_evidence=rel(run / 'solve.time' if timeout else solve),
                   alternative_evidence=rel(run / 'export.audit'))
        if VECTOR_SPEC13_GATE.is_file():
            gate = VECTOR_SPEC13_GATE.read_text()
            if 'All tests passed (4760 assertions in 118 test cases)' in gate:
                row['detail'] += '。候选门禁 118 项、4,760 断言通过；两项原有 CVC5 超时与父版去版本行后一致'


def record_vector_exp_g5(by_property):
    row = by_property[VECTOR_EXP]
    replay = VECTOR_EXP_G5 / 'replay.tsv'
    if row['verdict'] != 'pending' or not replay.is_file():
        return
    journal, = VECTOR_EXP_G5.glob('crisp-out/*/progress-*.tsv')
    event, = final_rows(journal)
    assert (event['property'], event['status']) == ('vector_exp_leaf_check.compare_g5', 'refuted')
    assert (VECTOR_EXP_G5 / event['detail']).is_file()
    samples = {sample['tick']: sample for sample in rows(replay)}
    assert [samples[tick]['a_hex'] for tick in ('1', '2', '3')] == ['590', '432', '498']
    assert [samples['17'][key] for key in ('result_valid', 'result_hex', 'reference_hex')] == ['1', '5fe', '3df']
    row['detail'] += '。G5 单 lane 字级 BMC 深度 17 在 9.683 秒报告局部反例，第 17 拍 valid=1，实际 0x5fe、参照 0x3df'
    row.update(work_status='局部反例已复现，原片见证待证',
               next_action='核对原整片合法输入下的三拍 lane 数据和原性质采样窗口，准备定向复放',
               eta_utc='待原片接线映射和定向复放', alternative_evidence=rel(VECTOR_EXP_CUT_REPORT))


def vector_exp_top_replays():
    if not VECTOR_EXP_TOP_RESULTS.is_file():
        return []
    attempts = list(rows(VECTOR_EXP_TOP_RESULTS))
    assert [r['run'] for r in attempts] == ['first', 'exact_g5']
    assert [r['lane0_a'] for r in attempts] == ['590/590/432', '590/432/498']
    for attempt in attempts:
        assert [attempt[k] for k in ('command', 'packet', 'write_sample', 'write_req', 'compare')] == [
            '69', '71', '92', '1', '94',
        ]
        assert all(re.fullmatch('[0-9a-f]{24}', attempt[k]) for k in ('actual', 'reference'))
        expected = 'finite_mismatch' if attempt['actual'] != attempt['reference'] else 'finite_match'
        assert attempt['status'] == expected
    assert attempts[0]['status'] == 'finite_mismatch'
    return attempts


def vector_exp_top_detail(attempts):
    results = []

    def vector_value(word):
        return '均为 0x' + word[:3] if word == word[:3] * 8 else '为 0x' + word

    for label, attempt in zip(('首轮', '精确 G5 复验'), attempts):
        actual, reference = (vector_value(attempt[k]) for k in ('actual', 'reference'))
        results.append(f"{label} A={attempt['lane0_a']}，实际{actual}、原参照{reference}")
    return ('冻结原片两次定向仿真均命中原前件，#69 命令、#71 操作数、#72–#93 静默，#92 写请求，#94 比较。'
            + '。'.join(results) + '。')


def record_vector_exp_top(by_property, choices):
    row = by_property[VECTOR_EXP]
    attempts = vector_exp_top_replays()
    if row['verdict'] != 'pending' or not attempts:
        return
    finite = vector_exp_top_detail(attempts)
    waveform = VECTOR_EXP_TOP_EXACT / 'replay.vcd'
    if choices.get('6') == 'a':
        assert all(attempt['status'] == 'finite_mismatch' for attempt in attempts)
        assert '原性质有限失配确认' in VECTOR_EXP_TOP_AUDIT.read_text()
        sim_wall = re.search(r'walltime ([\d.]+) s', (VECTOR_EXP_TOP_EXACT / 'sim.log').read_text())[1]
        peak = int((VECTOR_EXP_TOP_EXACT / 'memory-peak.txt').read_text()) // 1024
        detail = (finite + 'Neon 独立核对通过，按 Leiqi 第 6 条 a 收为整片反例。原生 SVA 严格认证及无限延拓未取得。'
                  + '历史整片求解和局部结果。' + row['detail'])
        row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', sim_wall, peak,
                               '精确复验，仿真墙钟及构建和仿真 cgroup 峰值', '', '', detail,
                               VECTOR_EXP_TOP_RESULTS, VECTOR_EXP_TOP_REPORT))
        row.update(work_status='按第 6 条 a 收账', next_action='保留两次有限失配及严格认证状态',
                   eta_utc='已结算', alternative_evidence=rel(VECTOR_EXP_TOP_AUDIT), waveform=rel(waveform))
        return
    row['detail'] += '。' + finite
    row.update(work_status='原片有限失配已取证，整片待结算',
               next_action='复核有限失配证据及整片验收口径', eta_utc='待整片证据复核',
               alternative_evidence=rel(VECTOR_EXP_TOP_REPORT), waveform=rel(waveform))


def masked_reduction_replays():
    for mode in ('sum', 'max'):
        case = VECTOR_REDUCTION_MASKED / mode
        result = case / 'result.tsv'
        if not result.is_file():
            continue
        attempt, = rows(result)
        assert attempt['property'] == f'plena_isa_chip_vector_fp12.v_red_{mode}__chip_reduction_result'
        assert int(attempt['tree_input']) == int(attempt['packet']) + 2
        assert int(attempt['compare']) == int(attempt['packet']) + 31
        assert (attempt['rmask'], attempt['recorded_mask'], attempt['packed_tree_input']) == ('1', 'fe', '0' * 27)
        assert all(re.fullmatch('[0-9a-f]{3}', attempt[k]) for k in ('actual', 'reference'))
        matched = attempt['actual_valid'] == '1' and attempt['actual'] == attempt['reference']
        assert attempt['status'] == ('finite_match' if matched else 'finite_mismatch')
        yield mode, case, attempt


def masked_reduction_detail(attempt):
    return (f"冻结原片 #{attempt['anchor']} 复位锚，#{attempt['mask_write']} 在 Q 内写 mask 0x{attempt['recorded_mask']}，"
            f"#{attempt['command']} 命令 rmask={attempt['rmask']}，#{attempt['packet']} 仅 A/S valid。"
            f"原 A0=0x{attempt['raw_a'][-3:]}、S=0x{attempt['raw_s']}，#{attempt['tree_input']} 入树九槽全零。"
            f"原 p+31 在 #{attempt['compare']} 采到 valid={attempt['actual_valid']}，"
            f"实际 0x{attempt['actual']}、原参照 0x{attempt['reference']}。")


def record_masked_reduction(by_property, choices):
    review = ({row['property']: row for row in rows(VECTOR_REDUCTION_MASKED_REVIEW)}
              if VECTOR_REDUCTION_MASKED_REVIEW.is_file() else {})
    for mode, case, attempt in masked_reduction_replays():
        row = by_property[attempt['property']]
        if row['verdict'] != 'pending' or attempt['status'] != 'finite_mismatch':
            continue
        finite = masked_reduction_detail(attempt)
        review_row = review.get(attempt['property'])
        waveform = case / 'replay.vcd'
        if choices.get('6') == 'a' and review_row and review_row['status'] == 'confirmed':
            independent = ROOT / review_row['evidence']
            assert independent.is_file()
            sim_wall = re.search(r'walltime ([\d.]+) s', (case / 'sim.log').read_text())[1]
            peak = int((case / 'memory-peak.txt').read_text()) // 1024
            prior = row['other_evidence']
            detail = (finite + '独立复核通过，按 Leiqi 第 6 条 a 收为整片反例。原生 SVA 严格认证及无限延拓未取得。'
                      + '历史整片求解记录。' + row['detail'])
            row.update(measurement('refuted', 'Verilator 5.037 · 原片定向仿真', '', sim_wall, peak,
                                   '仿真墙钟及 cgroup 峰值' + ('（含构建）' if mode == 'sum' else ''), '', '',
                                   detail, case / 'result.tsv', prior))
            row.update(work_status='按第 6 条 a 收账', next_action='保留有限失配及严格认证状态',
                       eta_utc='已结算', alternative_evidence=rel(independent), waveform=rel(waveform))
        else:
            row['detail'] += '。' + finite
            row.update(work_status='原片有限失配已取证，独立复核待回', next_action='复核原前件、掩码及波形',
                       eta_utc='待独立复核', alternative_evidence=rel(VECTOR_REDUCTION_MASKED_REPORT),
                       waveform=rel(waveform))


def record_pm_witness_start(by_property, now):
    timing = PM_LASSO_TRIAL / 'timing.tsv'
    row = by_property['plena_kinds_chip_l1_spec.pm_prefetch_valid_reset']
    if row['verdict'] == 'pending' and PM_LASSO_ARCHIVE.is_file():
        assert '**Result**: `TIMEOUT`' in PM_LASSO_ARCHIVE.read_text()
        row['detail'] += '。原整片 9 帧闭环在 480.492 秒超时，峰值 4,564.39 MiB，无 VCD 或证书'
        row['next_action'] = '保留原模型完整状态，已派一转移闭环短试'
        return
    if row['verdict'] != 'pending' or not timing.is_file():
        return
    events = {item['event']: item for item in rows(timing)}
    began = dt.datetime.fromisoformat(events['start']['utc'].replace('Z', '+00:00'))
    deadline = began + dt.timedelta(minutes=8)
    if 'end' in events or now >= deadline:
        return
    row['detail'] += '。原整片短试已进入严格闭环认证，BMC 界 1，闭环最多 8 个转移'
    row['next_action'] = '核对原生证书的触发拍、闭合帧和原片约束'
    row['eta_utc'] = f'{deadline:%m-%d %H:%M:%S} UTC 任务上限'


def link(label, path, line=''):
    target = str(ROOT / path)
    where = f':{line}' if line else ''
    return f'[{label}]({target}{where})'


def table_line(row, replay_sections):
    prop = row['property'].split('.', 1)[-1]
    top = row['property'].split('.')[0]
    status = LABELS[row['verdict']]
    if row['depth']:
        status += f" · d{row['depth']}"
    if row['verdict'] != 'proved':
        status += ' · ' + row['detail']
    if row['verdict'] == 'proved':
        mib = int(row['peak_kib']) / 1024
        status += f" · {row['engine']} · {row['wall_s']} 秒 · {mib:.1f} MiB"
    action = row['next_action']
    evidence = link('证据', row['evidence'])
    if row['verdict'] == 'proved' and row['property'].startswith((CSR_RESET_NO, CSR_SET)):
        status += ' · ' + row['detail']
        evidence += ' · ' + link('原 d10 前件证据', row['other_evidence'])
    elif row['verdict'] == 'proved' and row['property'] in RESET:
        status += ' · ' + row['detail']
        evidence += ' · ' + link('早期 d10 证据', row['other_evidence'])
    elif row['verdict'] == 'proved' and row['property'] in (
            FLOAT_READBACK, FLOAT_MAX_PRESERVE, FLOAT_MAX_RESULT, FLOAT_SQRT_RESULT,
            *FLOAT_CASE_B_PRESERVE, *FLOAT_FWD_LOGS):
        status += ' · ' + row['detail']
        evidence += ' · ' + link('原跑日志', row['other_evidence'])
        fwd_log = FLOAT_FWD_LOGS.get(row['property'])
        if fwd_log:
            evidence += ' · ' + link('fwd 原跑日志', fwd_log)
    elif row['work_status'] == DECISION7_NOTE:
        status += ' · ' + row['detail']
        evidence += ' · ' + link('原片核对', row['other_evidence'])
    if row['alternative_evidence']:
        alt = row['alternative_evidence']
        label, section = replay_sections.get(alt, ('补测', ''))
        evidence += ' · ' + link(label, alt, section)
    if row['waveform']:
        evidence += ' · ' + link('波形', row['waveform'])
    return (f"| {top} | {link(prop, row['source'], row['source_line'])} | {status.replace('|', '&#124;')}"
            f" | {row['vacuous']} | {evidence} | {action} | {row['eta_utc']} |")


def chip_counts_line(report_rows):
    counts = Counter(row['verdict'] for row in report_rows)
    return f"| 整芯片 | {len(report_rows)} | " + ' | '.join(str(counts[verdict]) for verdict in LABELS) + ' |'


def vacuous_counts(report_rows):
    counts = Counter(row['vacuous'] for row in report_rows)
    bounded_misses = 0
    for state, count in counts.items():
        validate_vacuity(state, 'ledger')
        if state not in (TRIGGERED, NEVER_TRIGGERED, UNCHECKED):
            bounded_misses += count
    return {TRIGGERED: counts[TRIGGERED], 'N 拍内未触发': bounded_misses,
            NEVER_TRIGGERED: counts[NEVER_TRIGGERED], UNCHECKED: counts[UNCHECKED]}


def markdown(report_rows, stamp, observations, memory_pdr, common_cover, cover_timeouts, checkpoint, choices):
    replay_sections = {
        rel(path): (label, next(i for i, text in enumerate(path.read_text().splitlines(), 1)
                                if text.startswith(f'## {section}. ')))
        for path, section, label in (
            (INT_MEMORY_LIFT_AUDIT, 5, 'Opus 第 5 节独立重采样'),
            (ADD_FORWARD_AUDIT, 5, 'Opus 第 5 节独立重采样'),
            (PASS_ADDR2_AUDIT, 4, 'Opus 第 4 节独立重采样'),
            (COMP_ADDR2_AUDIT, 4, 'Opus 第 4 节独立重采样'),
            (RECI_LEAF_AUDIT, 6, 'Neon 第 6 节叶级证据'),
        ) if path.is_file()
    }

    lines = [
        f'# PLENA 原 spec · 整芯片 {len(report_rows)} 条逐条验证',
        '',
        f'更新于 {stamp}。目标是 87 条最终均有无界证明或反例，有界通过是中间状态。'
        + ('原片定向仿真的反例按第 6 条 a 收账，逐条证据见下表。' if choices.get('6') == 'a'
           else 'GP0 按 Leiqi 确定的口径以原片波形收为反例。')
        + '历史深度、配置及完整证据见长期 TSV。',
        '',
        '| 范围 | 总数 | 无界证明 | 有界通过 | 反例 | 待结算 |',
        '| --- | ---: | ---: | ---: | ---: | ---: |',
    ]
    lines.append(chip_counts_line(report_rows))
    lines.extend(('', 'vacuous 分布为 ' + '、'.join(
        f'{state} {count} 条' for state, count in vacuous_counts(report_rows).items()
    ) + '。'))
    approved = choices.get('7') == 'approved' and choices.get('8') == 'approved'
    if approved:
        lines[-1] += '未查的一条为 pinstr_d_ready_tie，原断言无显式前件。'
    legacy = sum('Crisp c82f61c depth 20 passed' in row['historical'] for row in report_rows)
    vector_plan = ''
    if (VECTOR_BITWUZLA / 'assertion/start.tsv').is_file():
        if (VECTOR_BITWUZLA / 'outcomes.tsv').is_file():
            vector_plan = '。原样模型的 Bitwuzla 补测见逐阶段结果。' + link('Bitwuzla 结果', rel(VECTOR_BITWUZLA / 'outcomes.tsv'))
        else:
            vector_plan = '。原样模型已启动 fd5ef342/Bitwuzla 深度 27。' + link('Bitwuzla 复验命令', rel(VECTOR_BITWUZLA / 'assertion/command.sh'))
    vector_fix = REF.parent / 'ebmc/13-yices-packed-past.md'
    if vector_fix.is_file():
        vector_plan += ('。EBMC Spec 13 已修复同源量词，保留原样 d27/d35/d35 的历史复验记录。'
                        if choices.get('6') == 'a' else '。EBMC 先修同源量词，原样 d27/d35/d35 复验统一排资源。')
        vector_plan += link('EBMC Spec 13', rel(vector_fix))
    pdr_queue = PRIORITY_PDR / 'wrapper-queue.tsv'
    pdr_queue_note = (
        '旧版首诊断为 IC3 转移约束的 34 条已按原属性分四个包装排队，ext 3、kinds 6、scalar 1、CSR 24。'
        + link('整设计 PDR 队列', rel(pdr_queue))
        if pdr_queue.is_file() else ''
    )
    ext_pause = PRIORITY_PDR / 'runs/001-ext_20260801_spec/attempt-status.tsv'
    if ext_pause.is_file():
        pdr_queue_note += (
            ' Ext 首轮实耗 312.26 秒，cgroup 峰值 18,649,710,592 字节，为 Vector EXP 等任务让出内存。'
            ' 已保留增量 TSV，停止时无 OOM、无最终结论。'
            + link('Ext 首轮停止记录', rel(ext_pause))
        )
    spec43_trial_note = (
        'Spec 43 同源本地候选已让原样 int_memory 进入 new IC3 求解。600 秒限时实耗 601.10 秒，峰值 RSS 18,230,724 KiB，'
        '退出 124，无 OOM，无性质判定。相同源码树的发布版 83e914af 已经 Crisp #55 合入 main 094d7e4d。'
        + link('原样 PDR 超时证据', rel(SPEC43_TRIAL))
        + ' · ' + link('Spec 43 发布门禁', 'docs/ref/ebmc-spec43-disabled-sequence.md')
        if SPEC43_TRIAL.is_file() else ''
    )
    bmc_updates = []
    r4 = next(row for row in report_rows if row['property'] == R4_PROP)
    if r4['verdict'] == 'refuted' and r4['alternative_evidence'] == rel(R4_AUDIT):
        bmc_updates.append(
            'ext_r4_mask_op_not_issued 按原无显式复位模型收为有限反例。Crisp c82f61c 的任意初态轨迹满足唯一假设，'
            'step1 issued_c_valid=1、issued_c_op=0x2e。严格重放 OOM，未得 final。'
            + link('Opus 波形核对', r4['alternative_evidence']) + ' · ' + link('历史整片 VCD', r4['waveform'])
            + ' · ' + link('严格重放进度', r4['other_evidence']) + ' · ' + link('Spec 44 结算依据', rel(SPEC44))
        )
    sub_vv = next(row for row in report_rows if row['property'] == VECTOR_SUB_VV)
    if sub_vv['verdict'] == 'refuted' and sub_vv['alternative_evidence'] == rel(VECTOR_SUB_AUDIT):
        bmc_updates.append(
            'Vector SUB_VV 在 #84 实际 0x3e0、原参照 0xbe0，比较前两拍 #82 wreq=1。完整前件及失配经 Neon 核对，'
            '按第 6 条 a 收为原片有限反例，原生严格认证及无限延拓未取得。'
            + link('独立核对', rel(VECTOR_SUB_AUDIT)) + ' · ' + link('原片 VCD', sub_vv['waveform'])
        )
    add_int = next(row for row in report_rows if row['property'] == ADD_INT_CHIP)
    if add_int['verdict'] == 'refuted' and add_int['alternative_evidence'] == rel(ADD_INT_CHIP_AUDIT):
        bmc_updates.append(
            '标量 ADD_INT 原片 #52–#55 命中完整前件，#57 实际 0x00000007、原参照 0x0000012a。'
            '按第 6 条 a 收为有限反例，原生严格认证未取得。'
            + link('原式、逐拍核对及配置', rel(ADD_INT_CHIP_AUDIT)) + ' · ' + link('原片 VCD', add_int['waveform'])
        )
    if choices.get('6') == 'a' and SCALAR_GP0_SIX_AUDIT.is_file():
        bmc_updates.append(
            'ADDI_INT、SUB_INT、MUL_INT 的原片复放完整命中原前件，分别在 #57、#57、#58 因 gp0 前递失配，'
            '按第 6 条 a 收为有限反例，原生严格认证未取得。'
            'LUI_INT、LOOP_START 的 PASS 读非零 rd，LOOP_END 的首次 PASS 因原式绑定与 decoder 的 rs1=rd 也读非零地址。'
            + ('三条按第 7 条记无界证明。' if approved else '三条保持待结算。')
            + link('六条逐式核对、原片输入与 VCD', rel(SCALAR_GP0_SIX_AUDIT))
        )
    bmc_updates.extend((
        'LUI_INT、LOOP_START 的局部控制与数据经 k-induction 证成，提升终审及独立复核通过，原片完整前件有限见证的后件成立。'
        'LOOP_END 数值式局部安全已证并独立复核，配对 LOOP_START 的 V2 经 skid 路径在 #65–#68 命中，#70 后件成立。'
        + ('三条按第 7 条记无界证明。' if approved else '三条仍待 decisions 第 7 条。')
        + link('提升终审', rel(SCALAR_LIFT_FINAL)) + ' · '
        + link('独立复核', rel(SCALAR_LIFT_INDEPENDENT)) + ' · '
        + link('LUI、LOOP_START 原片见证', rel(SCALAR_CHIP_WITNESS)) + ' · '
        + link('LOOP_END V2 原片见证', rel(SCALAR_LOOP_END_WITNESS)),
        'COMP_ADDR 的局部控制与数据 k=16 证成，独立复核通过。原片 #37–#43 完整命中，#44 后件成立。'
        'G2 LOOP_END 保值安全已提升并独立复核，原片 #37–#47 完整命中，#48 后件成立（gp1=0，未配对 LOOP_START）。'
        + ('两条按第 7 条记无界证明。' if approved else '两条仍待 decisions 第 7 条。')
        + link('COMP_ADDR 复核及原片见证', rel(COMP_ADDR_WITNESS)) + ' · '
        + link('G2 LOOP_END 原片见证', rel(G2_LOOP_END_WITNESS)),
        ('FP 18 条按第 8 条初次入账时记原模型有界通过。' if approved else 'FP 18 条仍待 decisions 第 8 条。')
        + '门控投影 d24 仍可达，旧包排空引理不成立。'
        + link('门控运行记录', rel(FP_GATED_PREFIX / 'README.md')) + ' · '
        + link('旧包排空引理审阅', rel(FP_STALE_DRAIN_AUDIT)),
    ))
    if VECTOR_SUB_VF_AUDIT.is_file():
        bmc_updates.append(
            'Vector SUB_VF _2、_9 各有一次原片完整有限触发。#69 指令发射，#71 取样，#82 写请求为 1。'
            '#84 对原 VF 参考重算均相等，lane0 为 0x000，其余 lane 分别为 0xbe0 和 0x3e0。'
            + ('两条按第 7 条记无界证明。' if approved else '两条保持待结算。')
            + link('原前件与 VF 参考核对', rel(VECTOR_SUB_VF_AUDIT)) + ' · '
            + link('_2 原片 VCD', rel(VECTOR_SUB_VF_RUNS / 'vsubvf-sub2/replay.vcd')) + ' · '
            + link('_9 原片 VCD', rel(VECTOR_SUB_VF_RUNS / 'vsubvf-rsub9/replay.vcd'))
        )
    add_mul_reports = [(directory.upper().replace('-', '_') + '_' + opcode, VECTOR_ADD_MUL_RUNS / directory / 'REPORT.md')
                       for directory, opcode, _, _ in VECTOR_ADD_MUL_CASES]
    if all(report.is_file() for _, report in add_mul_reports):
        bmc_updates.append(
            'Vector ADD_VV_1、ADD_VF_1、MUL_VV_3、MUL_VF_3 各有一次完整原片有限正例。'
            '两条 ADD 在 #84、两条 MUL 在 #82 与原参照相等。'
            + ('四条按第 7 条记无界证明。' if approved else '四条保持待结算。')
            + link('四条原片复放汇总', rel(VECTOR_ADD_MUL_REPORT)) + ' · '
            + ' · '.join(link(name + ' 逐拍证据与 VCD', rel(report)) for name, report in add_mul_reports)
        )
    if VECTOR_SWEEP_AUDIT.is_file():
        bmc_updates.append(
            'Opus 对 ADD_VV_1、ADD_VF_1、MUL_VV_3、MUL_VF_3、SUB_VF_2、SUB_VF_9 逐条扫查 32 组操作数。'
            '每条 32/32 完整命中、32/32 后件成立、0 失配。'
            + ('六条按第 7 条记无界证明。' if approved else '六条保持待结算。')
            + link('扫查审阅及六条原始 TSV', rel(VECTOR_SWEEP_AUDIT))
        )
    for _, report, detail in fp_prefix_proof_evidence():
        if approved:
            detail = detail.removesuffix('，整片待结算')
        bmc_updates.append(detail + '。' + link('原终态、命令与资源', rel(report)))
    reci = next(row for row in report_rows if row['property'] == VECTOR_RECI)
    if reci['verdict'] == 'refuted' and reci['alternative_evidence'] == rel(RECI_UNDRIVEN_AUDIT):
        bmc_updates.append(
            'Vector RECI 合法程序 #152–#166 完整命中原窗口，#166 lane0 实际 0x01F、原参照 0x7DF。'
            '原 SALT 默认 --undriven seq 允许无驱动的 fp_reciprocal.leading_zeros[5:4] 每拍自由，'
            '局部 d24 BMC 亦 REFUTED。Verilator 两态默认将两位置 0，同程序对照通过。'
            '按第 6 条 a 收为有限反例，严格无限延拓证书未取得。'
            + link('Opus 原片及局部证据', rel(RECI_UNDRIVEN_AUDIT)) + ' · ' + link('原片 VCD', reci['waveform'])
        )
    if RECI_LEAF_AUDIT.is_file():
        bmc_updates.append(
            'RECI 此前的叶级有限前缀 d6 在第 6 拍得到 0x63f，原参照 0x7df，0.30 秒、42.9 MiB。'
            + link('独立复核与叶级 VCD', rel(RECI_LEAF_AUDIT), replay_sections[rel(RECI_LEAF_AUDIT)][1])
        )
    if RECI_CHIP_D72_AUDIT.is_file():
        bmc_updates.append(
            'Vector RECI 原片 d72 在 Properties 展开阶段运行 179.76 秒，触及 24 GiB 硬限并被 OOM kill。'
            '未入 Yices，无 VCD 或性质判定。'
            + link('Neon 命令、原日志及资源', rel(RECI_CHIP_D72_AUDIT))
        )
    if (RECI_FIXED_D72 / 'RUN.md').is_file():
        bmc_updates.append(
            'Vector RECI 固定 #72 监视器只实际运行一次 d72。性质展开 54.663 毫秒，已进入 Yices，'
            '整个服务 285.184 秒达到 24 GiB 硬限并 OOM。无 SAT/UNSAT 或 VCD。'
            + link('固定 #72 命令和分阶段证据', rel(RECI_FIXED_D72 / 'RUN.md'))
        )
    setup_vacuity = {
        prop for prop, _, update in observations
        if prop.split('.', 1)[0] in INT_STARTUP_GROUPS and update['depth'] == '20' and update['verdict'] == 'vacuous'
    }
    if setup_vacuity:
        cover_note = (
            '六条的第一项 PASS_ADDR 是短界的共同必要条件。'
            + link('原前件查询队列', rel(INT_COVER_QUEUE))
            if INT_COVER_QUEUE.is_file() else
            '正在核对首个实际出指与完整前件的最短窗口。'
        )
        bmc_updates.append(
            f"int_memory、int_preserve_g1 与 int_effects 已测的 {len(setup_vacuity)} 条都在冷启动后深度 20 为 vacuous。"
            + cover_note
        )
        if INT_WINDOWS_REPORT.is_file():
            bmc_updates.append(
                '逐源式核对显示，首个 PASS_ADDR 须在 #4–13，loop 最晚 #14。'
                ' 32 字取指、Top 直接接线及解码复位合起来给出 PASS_ADDR 不早于 #33 的保守下界。'
                + (' 六条原完整前件按 d48 单选查询，从 loop 开始。' if common_cover else
                   ' 公共 d44 与六条完整 d48 cover 已排，等待 CSR 和正式整设计 PDR 释放资源。')
                + link('窗口推导与逐源式证据', rel(INT_WINDOWS_REPORT))
            )
        if common_cover:
            bmc_updates.append(
                f"原整芯片公共前缀 `idle×3 → PASS_ADDR` 在深度 {common_cover['depth']} "
                f"使用 EBMC {common_cover['ebmc'][:8]}/Yices 查询，{common_cover['wall_seconds']} 秒超时，"
                f"进程树峰值 {common_cover['peak_tree_MiB']} MiB，未得到 SAT 或 UNSAT。"
                + link('公共 d44 实测 TSV', rel(INT_COMMON_COVER))
            )
        for result, evidence in cover_timeouts.values():
            bmc_updates.append(
                f"完整前件 `{result['cover']}` 在深度 {result['depth']} 使用 EBMC {result['ebmc'][:8]}/Yices 查询，"
                f"{result['wall_seconds']} 秒超时，进程树峰值 {result['peak_tree_MiB']} MiB，未得到 SAT 或 UNSAT。"
                + link('完整前件实测 TSV', rel(evidence))
            )
        if INT_COVER_PAUSE.is_file():
            bmc_updates.append(
                '公共 d44 和四个完整 d48 查询均超时后，深界队列已暂停。STORE 刚启动即停，未出判定，store/load 未启动。'
                + link('队列暂停记录', rel(INT_COVER_PAUSE))
            )
        if INT_G2_REPORT.is_file():
            bmc_updates.append(
                'G2 与接口的另十条也需要同一个首轮 PASS_ADDR。G2 最晚第 13 拍，四条短接口最晚第 17 拍，'
                '依赖 ADD 最晚第 14 拍，均早于第 33 拍下界。'
                + link('十条前件与正式 TSV', rel(INT_G2_REPORT))
            )
    pdr_command = INT_MEMORY_OFFICIAL / 'command.txt'
    official_note = (
        'Crisp 正式原样 int_memory 整设计 PDR 已单独启动，原 Spec 27 SALT 保持相同 SHA-256，20 GiB cgroup，原任务墙钟 1,800 秒。'
        + link('正式重放命令', rel(pdr_command))
        if pdr_command.is_file() else ''
    )
    if memory_pdr:
        official_note = (
            f"与原 Spec 27 SALT 字节相同的 Crisp 正式 PDR 已结束。{memory_pdr['wall']} 秒，"
            f"峰值 {int(memory_pdr['peak']):,} KiB，退出 {memory_pdr['exit']}，"
            f"状态 {memory_pdr['verdict']}。{memory_pdr['detail']}。"
            + link('正式 PDR CLI', rel(memory_pdr['cli']))
            + ' · ' + link('增量 TSV', rel(memory_pdr['journal']))
        )
    if KINDS_INTERRUPTED.is_file():
        bmc_updates.append(
            'kinds 两条首轮在整片前件查询中人工停止，未得 final。'
            + link('中断、归档与队尾状态', rel(KINDS_INTERRUPTED))
        )
        kinds_audit = KINDS_TAIL / 'SUMMARY.txt'
        if not TAIL_CONSOLIDATION.is_file() and kinds_audit.is_file() and any(
            by_row['verdict'] == 'pending' for by_row in report_rows if by_row['property'] in KINDS_RESET_CASES
        ):
            same_depth = KINDS_TAIL / 'cov_d20.time'
            bmc_updates.append(
                'kinds 两条原断言在深度 20 均无反例，共用 rst 在深度 1 有可重放见证。'
                + ('定点 d20 Yices 查询 300.45 秒超时，峰值 4,128,200 KiB。已排同模型字级 Z3。'
                   if same_depth.is_file() else '同界可达性待一条延伸到第 20 拍的原模型见证。')
                + link('两份独立证据和命令', rel(kinds_audit))
            )
        if any(row['wrapper'] == SCALAR_STARTUP for row in rows(KINDS_INTERRUPTED)):
            scalar_split = KINDS_INTERRUPTED.parent / 'split_manifest.tsv'
            split_link = ' · ' + link('分槽逐条清单', rel(scalar_split)) if scalar_split.is_file() else ''
            bmc_updates.append(
                'Scalar 首轮卡在单独的 gp0 前件查询，已中断且无 final，按同一原包装拆成七条正式 d20 和单条 gp0。'
                + link('Scalar 中断证据', rel(KINDS_INTERRUPTED))
                + split_link
            )
        if any(row['wrapper'] == FLOAT_BATCH for row in rows(KINDS_INTERRUPTED)):
            bmc_updates.append(
                'FP 十条首轮在主断言 B221 停止，未进入前件检查，增量 TSV 无 final。'
                ' 先跑 scalar 七条，再做 FP 八条长前件，fp0 独立，s_map_v_fp 单条后移。'
                + link('FP 中断证据', rel(KINDS_INTERRUPTED))
            )
    refresh_table = KINDS_INTERRUPTED.parent / 'per_property_results.tsv'
    refresh_rows = list(rows(refresh_table))
    if refresh_rows:
        detail_by_wrapper = Counter(row['wrapper'] for row in refresh_rows)
        packages = sum(detail_by_wrapper[name] == size for name, size in NEON_WRAPPER_TOTALS.items())
        vacuities = sum(row['status'] == 'vacuous' for row in refresh_rows)
        split_manifest = refresh_table.parent / 'split_manifest.tsv'
        slot_note = ''
        if split_manifest.is_file():
            slots = len({row['task_key'] for row in refresh_rows})
            slot_note = f"已完成 {slots}/{sum(1 for _ in rows(split_manifest))} 槽。"
        bmc_updates.append(
            f'非 CSR 正式刷新已完成 {packages}/10 个原包装，落盘 {len(refresh_rows)}/46 条，其中 {vacuities} 条为深度 20 vacuous。'
            + slot_note
            + link('非 CSR 逐条结果', rel(refresh_table))
        )
    if SCALAR_WINDOWS.is_file():
        bmc_updates.append(
            'Scalar 拆分出的七条已有正式 d20 vacuous。它们都要求第 2 拍或更晚用 PASS_ADDR 回读，而原接线在第 2–32 拍排除该操作。'
            + link('七条源式计数和 final', rel(SCALAR_WINDOWS))
        )
    if FP_WINDOWS_REPORT.is_file():
        bmc_updates.append(
            'FP 与 FP preserve 共 17 条。两条 EXP 的最短前件/后件分别为第 30/31 拍和第 28/29 拍，序列长度已解释 d20 vacuous。'
            ' 其余 15 条在语法上可落入 d20。第 37 拍下界仅适用于新取指导出的 LD_OUT，执行级旧值和停顿恢复另需取证。'
            + link('FP 原式窗口与执行级路径', rel(FP_WINDOWS_REPORT))
        )
    gp0_audit = TAIL_EVIDENCE / 'scalar_gp0/SUMMARY.txt'
    fp0_audit = TAIL_EVIDENCE / 'fp_fp0/SUMMARY.txt'
    if TAIL_CONSOLIDATION.is_file():
        pm_note = '优先对一个未结算的 kinds 原落点做新版局部 PDR 短试。'
        if PM_CONE_REPORT.is_file():
            pm_note = '新版 pm 的局部 PDR 实际落在整个 u_dut，触及 8 GiB。'
            if (PM_CONE_ARCHIVE / 'VERSIONS.txt').is_file():
                pm_note += '手工锥投影 PDR 经独立接线核对可提升整片无界安全性。'
                if (RST_LASSO / 'SUMMARY.txt').is_file():
                    pm_note += ('原模型在 #0 触发 rst，全状态回边 #0→#1 已获 EBMC 原生严格认证，25.052 秒，峰值 1,154.57 MiB。'
                                'pm 矩阵预取、向量 element valid 与矩阵 scale ready 三条收为整片无界。'
                                + link('原片闭环和 VCD 核验', rel(RST_LASSO / 'SUMMARY.txt')))
                else:
                    pm_note += '触发的无限延拓待证。'
            else:
                pm_note += '已验证更小的复位路径，整片结论待独立复核。'
            pm_note += link('pm 短试与复位路径', rel(PM_CONE_REPORT))
        bmc_updates.append(
            '队尾四组共五条原断言 d20 独立求解均无反例。已取得 kinds、gp0、fp0 的原模型短界见证，'
            '同界取证因超时或内存护栏暂停。'
            + link('原断言、波形与同界查询账', rel(TAIL_CONSOLIDATION))
            + ' ' + pm_note
        )
    elif gp0_audit.is_file() and fp0_audit.is_file():
        bmc_updates.append(
            'gp0、fp0 的独立原断言在 d20 均无反例。d5 原模型波形确认 gp0 前件在第 1 拍、fp0 前件在第 3–4 拍匹配。'
            '已排同原包装和冷启动复位的 d20 见证。'
            + link('gp0 独立证据', rel(gp0_audit)) + ' · ' + link('fp0 独立证据', rel(fp0_audit))
        )
    for prop, _, update in observations:
        evidence = link('正式 BMC 证据', update['evidence'])
        if prop == INT_MEMORY and update['depth'] == '20' and update['verdict'] == 'vacuous':
            bmc_updates.append(f"int_memory 的正式 BMC 深度 20 为有界 vacuous，{update['wall_s']} 秒、{int(update['peak_kib']):,} KiB。" + evidence)
        elif prop == 'ext_20260801_spec.ext_r8_accept_atomic' and 'vacuity check timed out' in update['detail']:
            bmc_updates.append('Ext accept atomic 的深度 20 原断言通过，前件查询在 1,800 秒任务总限内未完成，最终 unknown。' + evidence)
        elif prop == FLOAT_ZERO and 'vacuity check timed out' in update['detail']:
            bmc_updates.append(
                f"fp0 正式 d20 单选为 unknown，原断言无反例，前件查询超时。用时 {update['wall_s']} 秒，"
                f"峰值 {int(update['peak_kib']) / (1024 ** 2):.2f} GiB。"
                + evidence
            )
    use_local = choices.get('5') == 'b'
    decision_note = (
        '11 月 23 日批准第 7、8 条。第 7 条的 26 条记无界证明，含 fp0 与 EXT R8。'
        '第 8 条批准时，18 条 FP 按当时实测深度记有界通过，d40 十六条、MAX 保值 d52、MAP_V 首元素 d16。当前深度见逐条表。'
        '三条两拍复位已由整设计字级 k-induction k=4 提升为无界证明。'
        + link('第 7 条输入', rel(DECISION7_BATCH)) + ' · ' + link('第 8 条输入', rel(DECISION8_BATCH))
        + ' · ' + link('决定记录', rel(PROJECT_DECISIONS))
        if approved else
        '第 5 条选 b，整片主线转为可提升的 veval 局部无界证明。'
        + link('决定记录', rel(PROJECT_DECISIONS)) + ' · ' + link('首批队列', rel(LOCAL_PDR_QUEUE))
    )
    main_path = ('整片先验 Spec 27 局部落点、时钟复位、接口约束及传输前提，逐条核对能否提升回原断言。'
                 if use_local else '整芯片先收正式有界缺项和五条晚触发，继续 Spec 27 的子模块无界对照。')
    if LOCAL_PDR_ATTEMPTS.is_file():
        bmc_updates.append(
            '两条 EXP 均完整下放至 scalar_machine_init，保留原复位和接口绑定，零新增假设。'
            '保值 15.631 秒、335.72 MiB，数值 15.849 秒、370.42 MiB。两条均在 [*0:$] 的 Büchi 转换阶段报 unknown。'
            '独立接线和参考函数复核已通过，安全性提升待局部证明。'
            + link('Spec 27 局部试跑台账', rel(LOCAL_PDR_ATTEMPTS))
            + ' · ' + link('实际绑定与秒级复现', 'docs/ref/spec27-fp-late-pdr.md')
        )
        if EXP_REPEAT_REPORT.is_file():
            bmc_updates.append(
                'EBMC Spec 12 发布版 01f53566 的验收构建已为两条冻结 EXP 生成 Spot 输入。两次均于 119.01 秒到限，未进入 IC3。'
                'Crisp #57 已合入新 gitlink。EBMC 定位 Spot 转换，Crisp 继续拆安全义务。'
                + link('发布版本、门禁与两次 Spot 证据', rel(EXP_REPEAT_REPORT))
                + ' · ' + link('EBMC Spec 14', 'docs/ebmc/14-fp-exp-spot.md')
            )
        if all((EXP_SPOT_WINDOW / case / 'stage.txt').is_file() for case in ('preserve', 'result')):
            bmc_updates.append(
                'Spec 14 原型在两条冻结局部都绕过原 Spot 瓶颈并进入 new IC3，均到原 119 秒时限且无性质判定。'
                '串行窗口于 16:06:24.766 UTC 关闭，已交 Spec 13 的 Vector EXP。'
                + link('窗口逐项时刻', rel(EXP_SPOT_WINDOW / 'times.tsv'))
                + ' · ' + link('保值阶段', rel(EXP_SPOT_WINDOW / 'preserve/stage.txt'))
                + ' · ' + link('数值阶段', rel(EXP_SPOT_WINDOW / 'result/stage.txt'))
            )
        if EXP_SPOT_RELEASE.is_file() and '2d274b5a8e73f36e9ea99b533ff9fd7f840c2fc9' in EXP_SPOT_RELEASE.read_text():
            bmc_updates.append(
                'EBMC Spec 14 已发布 2d274b5a。两条冻结 EXP 均进入 IC3，但原 119 秒时限内无性质判定。Crisp 独立 gitlink 更新已排。'
                + link('发布与独立试验', rel(EXP_SPOT_RELEASE))
            )
        fp0_report = REF / 'spec27-fp0-local-pdr.md'
        if fp0_report.is_file():
            fp0_note = '下一轮缩到原寄存器组读口 2 和零地址旁路。'
            if FP0_CUT_PROGRESS.is_file() and FP0_CUT_REPORT.is_file():
                fp0_note = ('原读口与零地址旁路的小投影已在 0.064 秒证得安全性，峰值 21.69 MiB。'
                            '独立接线复核确认安全性可提升到原片，前件无限延拓待证。'
                            + link('小投影及复验材料', rel(FP0_CUT_REPORT)))
            bmc_updates.append(
                'fp0 的完整局部 PDR 在 scalar_machine_init 超时，用时 119.094 秒，cgroup 峰值 946.79 MiB。'
                'CLI 为 unknown，进度 TSV 只有 local。'
                + link('原试跑、静态切点及逐拍数据', rel(fp0_report))
                + ' ' + fp0_note
            )
    integer_cut = REF / 'spec27-int-second-local-pdr.md'
    if integer_cut.is_file():
        bmc_updates.append(
            'LOOP_END 与 LOAD 的局部检查均进入 new IC3 初态 SAT 求解，1199 秒限时后均为 CLI unknown。'
            'ADDI、SUB 和两条 LOOP 保值共用普通写回隔离，MUL 保留独立有效位和优先级。'
            + link('整数试跑阶段、原片映射和六条候选', rel(integer_cut))
        )
    loop_zero = next(row for row in report_rows if row['property'] == LOOP_ZERO_REFUTED)
    if loop_zero['verdict'] == 'refuted':
        bmc_updates.append(
            'LOOP_END 零值原片在 #49–#58 完整命中前件，#59 的 gp_out_1 为 0xffffffff，原后件要求 0。'
            '按第 6 条 a 收为有限反例，严格无限延拓证书未取得。'
            + link('原片逐拍核对及冻结配置', rel(LOOP_ZERO_REFUTED_AUDIT)) + ' · '
            + link('原片 VCD', loop_zero['waveform'])
        )
    elif LOOP_ZERO_REPORT.is_file():
        loop_step = '下一步只调整原取指对齐。'
        if LOOP_ZERO_ALIGNED.is_file():
            loop_step = '两个回跳对照已对齐 c8，FP 停顿使 c3 仍未发出首次 PASS，完整前件仍无命中。'
        bmc_updates.append(
            'LOOP_ZERO 原片候选确认零值减一后的实际回读为全 1。第二次 PASS 比原窗晚一拍，240 拍无完整触发。'
            + loop_step
            + link('原片程序、逐拍缺口和输入', rel(LOOP_ZERO_REPORT))
        )
    if STORE_CUT_REPORT.is_file():
        note = '下一步拆成写入控制和读回一致两条义务。'
        if STORE_OBLIGATIONS_REPORT.is_file():
            note = ('排空与前递的 PDR 以 9.730 秒通过。回读隔离的字级归纳已证安全性，前件认证在 44.011 秒超时。'
                    '两义务绑定同一原事件，待独立复核与原片触发。'
                    + link('拆分义务与结果', rel(STORE_OBLIGATIONS_REPORT)))
        if (STORE_PROGRAM / 'match.tsv').is_file():
            note = ('两安全义务经独立复核可提升。原整片定向运行在 #144–154 命中 STORE 完整前件，#155 后件成立，'
                    '已交付全部顶层输入和有限尾部，完整原模型回边待证。'
                    + link('义务复核与原片波形', rel(STORE_OBLIGATIONS_REPORT)))
        if STORE_CLOSURE.is_file():
            note += ('全层次仿真定位到 #175→#176 候选自环。' + link('尾段回边候选', rel(STORE_CLOSURE)))
        if STORE_SCALE.is_file():
            note += ('6/21 帧 Yices 实测峰值 1.81/4.99 GiB，原长界按斜率估计超过 32 GiB。先缩短程序。'
                     + link('实测资源与外推', rel(STORE_SCALE)))
        bmc_updates.append(
            'STORE 保值切点的逐拍映射已核，PDR 在 29.004 秒限时，采样峰值 89.38 MiB。'
            'CLI 为 unknown，TSV 只有表头，归档缺 Crisp 退出码。'
            + link('原切点、日志和采集说明', rel(STORE_CUT_REPORT))
            + ' ' + note
        )
    if SPEC27_LIFT_REPORT.is_file():
        lift_note = ('已有五条局部 PDR 的安全性均可提升。pinstr 无前件，向量 element valid 与矩阵 scale ready 共用原片 rst 闭环。'
                     'pwrite 和 Ext 请求仍待各自原模型的无限前件证据。') if (RST_LASSO / 'SUMMARY.txt').is_file() else (
            '已有五条局部 PDR 的安全性均可提升。pinstr 无前件，保留整片无界，其余四条待原片无限触发。')
        if (PWRITE_LASSO / 'SUMMARY.txt').is_file():
            lift_note = ('已有五条局部 PDR 的安全性均可提升。pinstr 无前件，两条 rst 性质共用原片闭环。'
                         'pwrite 原片的 hbm_ready_to_write 在 #0 命中且全状态 #0→#1 闭合，68.851 秒、1,382.04 MiB，整片收为无界。'
                         + link('pwrite 原片严格证书', rel(PWRITE_LASSO / 'SUMMARY.txt')))
            if (EXT_R8_LASSO / 'SUMMARY.txt').is_file():
                lift_note += (' Ext 请求保留唯一原假设，非复位 A valid 在 #0 命中并全状态 #0→#1 闭合，46.143 秒、1,357.41 MiB，整片收为无界。'
                              + link('Ext 请求原片严格证书', rel(EXT_R8_LASSO / 'SUMMARY.txt')))
            else:
                lift_note += ' Ext 请求仍待其原模型的无限前件证据。'
        bmc_updates.append(
            lift_note
            + link('五条接线与见证复核', rel(SPEC27_LIFT_REPORT))
        )
    if EXT_ATOMIC_REPORT.is_file():
        bmc_updates.append(
            'Ext 接受原子性同拍检查已在自由端口上获无界 PDR。与 Ext 请求不同，该原性质前件需要 A ready，原片无限延拓另行认证。'
            + link('无界安全证明与前件范围', rel(EXT_ATOMIC_REPORT))
        )
        if EXT_ATOMIC_TIMEOUT.is_file():
            bmc_updates.append(
                'Ext 接受原子性在唯一原假设下于 #0 取得有限 SAT。两转移三帧认证的两个候选回边在 Yices 到限，90.265 秒、1,830.95 MiB，无 VCD。'
                + link('原片分阶段计时和超时记录', rel(EXT_ATOMIC_TIMEOUT))
            )
        if EXT_ATOMIC_LOOP1_TIMEOUT.is_file():
            bmc_updates.append(
                '只固定 #1→#2 回边的原 SMT2 副本在 90.244 秒到限，657.93 MiB。改做一次原包装同界 BMC d2 的有界退路。'
                + link('单行差异、哈希与超时', rel(EXT_ATOMIC_LOOP1_TIMEOUT))
            )
        if (EXT_ATOMIC_BMC / 'SUMMARY.txt').is_file():
            bmc_updates.append(
                'Ext 接受原子性在 #57 原包装字级 BMC 深度 2 正式 passed，81.325 秒，cgroup 峰值 1,421.08 MiB。'
                '保留 d20 无反例和无界延拓的超时记录。'
                + link('正式 d2 CLI 与进度', rel(EXT_ATOMIC_BMC / 'SUMMARY.txt'))
            )
    if FP_EXP_ISOLATION_REPORT.is_file():
        bmc_updates.append(
            'FP EXP 保值的最小控制切点完成原事件监视器映射。写回隔离字级归纳 d32 不收敛，0.335 秒；同一隔离守卫下原 RF 回读已证，0.589 秒。'
            '写回控制和原片前件继续取证。' + link('两义务与逐拍映射', rel(FP_EXP_ISOLATION_REPORT))
        )
    if FP_EXP_INDUCTION_REPORT.is_file():
        bmc_updates.append(
            '保值控制的归纳超集在 c0 出现旧 SFU 写回。真实复位重放只在 c21 写 fpd，控制切点 d64 八组有界通过，1.845 秒、75.3 MiB。'
            + link('归纳失配与八组有界证据', rel(FP_EXP_INDUCTION_REPORT))
        )
    if FP_EXP_MIN_REPORT.is_file():
        bmc_updates.append(
            'FP EXP 保值可允许 c0 同址写。原 RF 写穿让 c2 看到写后值，保护区从 c1 开始。'
            '缩小后的控制义务唯一一次 PDR 在 28.002 秒到限，无 OOM，控制与整片结论待证。'
            + link('最小义务与原回读时序', rel(FP_EXP_MIN_REPORT))
        )
    if (FP_PRESERVE_RECEIVED / 'd24/outcome.tsv').is_file() and FP_PRESERVE_PREFIX_AUDIT.is_file():
        bmc_updates.append(
            'FP ADD/SUB/MAX/MUL 保值公共前缀的第一次 d24 命中局部 SUB，原 cover PROVED。'
            'Opus 依据 #6 received=1 且 v_s_out_valid=0 排除其原片可实现性。'
            '只修正这条反馈后，第二次 d24 原 cover PROVED，得到 #6–9 的局部 MUL 候选，'
            '4.67 秒、峰值 588,910,592 B、无 OOM。'
            + link('旧 SUB 原 CLI 与 VCD', rel(FP_PRESERVE_PREFIX / 'README.md')) + ' · '
            + link('独立排除证据', rel(FP_PRESERVE_PREFIX_AUDIT)) + ' · '
            + link('第二次 MUL 原 CLI、终态及 VCD', rel(FP_PRESERVE_RECEIVED / 'README.md'))
        )
    if (FP_PRESERVE_FP_STALL / 'd24/outcome.tsv').is_file() and FP_PRESERVE_RECEIVED_AUDIT.is_file():
        bmc_updates.append(
            'Opus 依据 #2 stall_req_from_fp=1 与原复位时序冲突排除第二条局部轨迹。'
            '保留 received 收紧并接回原 FP stall 寄存时序后，第三次 d24 原 cover PROVED，'
            '得到 #14–17 的局部 MUL，2.85 秒、峰值 594,030,592 B、无 OOM。'
            + link('第二条轨迹的独立排除', rel(FP_PRESERVE_RECEIVED_AUDIT)) + ' · '
            + link('第三次原 CLI、逐拍时序、终态及 VCD', rel(FP_PRESERVE_FP_STALL / 'README.md'))
        )
    if (FP_PRESERVE_MATRIX_CW / 'd24/outcome.tsv').is_file() and FP_PRESERVE_MATRIX_CW_AUDIT.is_file():
        limits = '#6 的 received 与 valid，以及 port_a 的原片可实现性仍待查。'
        if FP_PRESERVE_WRITE_REDUCTION_AUDIT.is_file():
            limits = (
                'Opus 已排除这条轨迹的原片可实现性。#6 归约 valid 早于最早 #10，'
                '#7、#8、#10 的 port_a 缺少原片写入来源。'
                + link('两条排除依据', rel(FP_PRESERVE_WRITE_REDUCTION_AUDIT))
            )
            if (FP_PRESERVE_WRITE_REDUCTION / 'README.md').is_file():
                limits += ' · ' + link('写请求和九拍归约静态映射', rel(FP_PRESERVE_WRITE_REDUCTION / 'README.md')) + '。'
        bmc_updates.append(
            '第四次保值公共前缀 d24 原 cover PROVED，返回局部 ADD #10–13。'
            'VCD 结构体高位为 x，#7、#8、#10 的 port_a=1 由唯一停顿项 PC:169 反推。'
            + limits
            + link('Opus 原片约束审阅', rel(FP_PRESERVE_MATRIX_CW_AUDIT)) + ' · '
            + link('第四次原 CLI、终态及 VCD', rel(FP_PRESERVE_MATRIX_CW / 'README.md'))
        )
    if FP_EXP_DECOMPOSITION.is_file():
        bmc_updates.append(
            'FP EXP 保值的旧 SFU/ALU 排空局部 PDR 于 4.225 秒证毕，cgroup 峰值 22,339,584 B。'
            'c21 目标隔离和后窗回读已按原 RTL 逐拍推导，组合提升与整片前件继续核对。'
            + link('三项义务和 PDR 证据', rel(FP_EXP_DECOMPOSITION))
        )
        if FP_EXP_C21.is_file():
            bmc_updates.append(
                'FP EXP 保值的独立 c21 写目标 PDR 在 55.007 秒到限，无性质判定，峰值 113,405,952 B。'
                + link('局部 CLI 结果', rel(FP_EXP_C21))
            )
    if (FP0_BMC / 'SUMMARY.txt').is_file():
        bmc_updates.append(
            'fp0 原片正式字级 BMC d5 非 vacuous 通过，100.991 秒，峰值 2,067.24 MiB。'
            '前件位于 #3/#4，后件在 #5；原片无限延拓待证。' + link('原配置 CLI、终态及资源', rel(FP0_BMC / 'SUMMARY.txt'))
        )
    if any(row['property'] == SCALAR_STARTUP + '.gp0__chip_zero' and row['verdict'] == 'refuted' for row in report_rows):
        bmc_updates.append(
            'GP0 按 Leiqi 确定的口径收为反例。原片 #43 命中前件，#45 采到 0x1000；此前 d5 有界通过保留。'
            + link('原片反例波形', rel(GP0_PROGRAM / 'gp0.vcd')) + ' · '
            + link('逐拍核对', rel(GP0_PROGRAM_AUDIT)) + ' · ' + link('历史 d5 证据', rel(GP0_BMC / 'SUMMARY.txt'))
        )
    if (SUB_OBLIGATIONS / 'README.md').is_file():
        sub_note = '原片完整前件及延拓继续取证。'
        if (SUB_PROGRAM / 'window.tsv').is_file():
            sub_note = ('原片定向仿真 #144–154 命中完整前件，#151 写 gp3=0，#149/#155 均读到 0x91。'
                        '#175→#176 是仿真回边候选。'
                        + link('原片逐拍输入、性质采样与波形', rel(SUB_PROGRAM / 'README.md')) + ' · ')
        if SUB_LIFT_AUDIT.is_file():
            sub_note += ('独立复核确认局部安全性可提升至整片。'
                         + link('24 项卫条件逐拍复核', rel(SUB_LIFT_AUDIT)) + ' · ')
        bmc_updates.append(
            'SUB 保值的写回与回读局部安全性均已证。回读的局部前件认证在 54.001 秒到限，final 为 unknown。'
            + sub_note + link('原事件映射及两份 final', rel(SUB_OBLIGATIONS / 'README.md'))
        )
    if (FIRST_FILL / 'prediction-vs-observation.tsv').is_file():
        bmc_updates.append(
            'STORE 与 SUB 在首块取指的原片仿真均于 #37–47 命中，#48 后件相等。STORE #68→#69 是更短的仿真回边候选。'
            + link('首块程序、逐拍采样及波形', rel(FIRST_FILL / 'README.md'))
        )
        if FIRST_FILL_STORE_SUB_AUDIT.is_file():
            bmc_updates.append(
                'Neon 独立核对 STORE 及旧串接 SUB 的首块有限触发。#100→#101 来自旧 ALU 三指令串接。'
                'STORE 与独立单事件 SUB 的仿真候选均为 #68→#69。'
                + link('首块独立复核', rel(FIRST_FILL_STORE_SUB_AUDIT)) + ' · '
                + link('单事件 SUB 证据', rel(FIRST_FILL_G2 / 'README.md'))
            )
    if (STORE_FIXED / 'result.tsv').is_file():
        result, = rows(STORE_FIXED / 'result.tsv')
        bmc_updates.append(result['detail'] + '。' + link('原生 SMT2、阶段日志及配额', rel(STORE_FIXED / 'README.md')))
    if STORE_TOTALITY_TRIAGE.is_file():
        bmc_updates.append(
            'EBMC 独立复现 STORE 总性判据的 packed struct/bv、member、index/with 与组合环边界。整片总性仍待证。'
            + link('后端独立分诊', rel(STORE_TOTALITY_TRIAGE))
        )
    if STORE_SPEC16_REPORT.is_file():
        bmc_updates.append(
            'EBMC Spec16 对 STORE 固定 #68→#69 的查询做静态化简，check-sat 前缀减少 0.52780%，'
            '21,624 条状态等式逐字保留。原生 footer 漏读 33,966 个自由符号，已保存补读表。'
            '该轮未做整片求解。'
            + link('Spec16 缩减及模型补读证据', rel(STORE_SPEC16_REPORT))
        )
    if (INT_MEMORY_FIRST_FILL / 'results.tsv').is_file():
        mapping = ('A/B/C 经 Opus 独立映射核对。' + link('独立映射', rel(INT_MEMORY_LIFT_AUDIT)) + ' · '
                   if INT_MEMORY_LIFT_AUDIT.is_file() else 'A/B/C 等待独立映射审阅。')
        if INT_MEMORY_LOCAL_RESULTS.is_file():
            mapping += ('三项局部主安全性已证，可组合为整片无界主安全性。A 119.002 秒、C 119.001 秒的前件认证到限，'
                        'final unknown。B 2.402 秒 certified，final proved。'
                        + link('单次证明及原 final', rel(INT_MEMORY_LOCAL_INPUTS / 'README.md')) + ' · ')
        elif (INT_MEMORY_LOCAL_INPUTS / 'README.md').is_file():
            mapping += '三个局部监视器已静态展开，等待求解排程。' + link('局部输入', rel(INT_MEMORY_LOCAL_INPUTS / 'README.md')) + ' · '
        bmc_updates.append(
            'STORE/LOAD 同址回读在 #37–47 命中，#43 写与 #45 读均为 SRAM 物理字 5。'
            '#46 两级 LOAD 地址对齐并发生 RF 口 1 同拍写穿，#48 后件相等。全零只计有限轨迹，#64→#65 为仿真候选。'
            + mapping
            + link('完整输入、采样与 VCD', rel(INT_MEMORY_FIRST_FILL / 'README.md')) + ' · '
            + link('静态拆分', rel(INT_MEMORY_LOCAL_REPORT))
        )
    if (FIRST_FILL_ADD_FORWARD / 'results.tsv').is_file():
        bmc_updates.append(
            'ADD 相依前递候选 F 在 #37–46 命中，#44 第一条 ADD 向口 1 前递，口 2 保持原值。'
            '#45 第二条 ADD 写 gp4 并同拍 RF 写穿，#47 后件相等。全零只计有限轨迹，#64→#65 为仿真候选。'
            + link('逐拍证据及后续数据投影边界', rel(FIRST_FILL_ADD_FORWARD / 'README.md'))
        )
    if (ADD_FORWARD_RESULTS / 'proof-results.tsv').is_file():
        control, data = rows(ADD_FORWARD_RESULTS / 'proof-results.tsv')
        bmc_updates.append(
            f"ADD 相依前递控制、数据主安全分别为 {control['main_safety']}、{data['main_safety']}，"
            f"原 final 分别为 {control['final']}、{data['final']}。"
            + link('两项单次证明及前件阶段', rel(ADD_FORWARD_RESULTS / 'README.md')) + ' · '
            + link('38 项交接独立核对', rel(ADD_FORWARD_LIFT_AUDIT))
        )
    if (ADDR2_FIRST_FILL / 'M/results.tsv').is_file():
        bmc_updates.append(
            'Q 的 PASS_ADDR_2 和 M 的两条 COMP_ADDR_2 前件各在 #41–47 完整命中一次，#48 分别输出 0/7 和 5/7，'
            '仿真回边候选分别为 #66→#67 和 #68→#69。'
            + link('原片逐拍输入及 VCD', rel(ADDR2_FIRST_FILL / 'README.md')) + ' · '
            + link('Q 独立重采样', rel(PASS_ADDR2_AUDIT), replay_sections[rel(PASS_ADDR2_AUDIT)][1]) + ' · '
            + link('M 独立重采样', rel(COMP_ADDR2_AUDIT), replay_sections[rel(COMP_ADDR2_AUDIT)][1])
        )
    if (ADDR2_OBLIGATIONS / 'proof-results.tsv').is_file() and ADDR2_LIFT_AUDIT.is_file():
        bmc_updates.append(
            'PASS_ADDR_2 与 COMP_ADDR_2 地址、数据三项局部主安全均 proved，经独立核对可提升为原式无界主安全。'
            '前件认证分别在 119.002、119.001、119.001 秒到限，三个 final 均 unknown，无 OOM。'
            + link('三份原 final 与资源', rel(ADDR2_OBLIGATIONS / 'README.md')) + ' · '
            + link('Opus 投影提升核对', rel(ADDR2_LIFT_AUDIT))
        )
    if (LOAD_OBLIGATIONS / 'README.md').is_file():
        bmc_updates.append(
            'LOAD 保值写回 final proved，读回主安全 proved，前件认证 119.001 秒到限，final unknown。'
            'Opus 独立核对 23/23 交接及 #37–47 原片有限 VCD。未初始化 SRAM 的零值只属于本轨迹。'
            + link('两份 final 与局部义务', rel(LOAD_OBLIGATIONS / 'README.md')) + ' · '
            + link('独立核对', rel(LOAD_LIFT_AUDIT))
        )
    for names, run in (('G1 ADD/LUI', FIRST_FILL_G1), ('G2 SUB/ADDI', FIRST_FILL_G2)):
        if (run / 'results.tsv').is_file():
            bmc_updates.append(
                f'{names} 的单事件首块仿真分别在 #37–47 命中，#48 后件相等，#68→#69 为仿真回边候选。'
                + link('实际输入、采样及波形', rel(run / 'README.md'))
            )
    if (ADDI_OBLIGATIONS / 'README.md').is_file():
        lift = ''
        if ADDI_LIFT_AUDIT.is_file():
            lift += ('Neon 独立核对 24 项逐拍交接，整片无界安全性成立。'
                     + link('独立提升复核', rel(ADDI_LIFT_AUDIT)) + ' · ')
        if ADDI_OPUS_AUDIT.is_file():
            lift += ('Opus 独立重采样确认原片 #37–47 命中、#48 后件成立。'
                     + link('独立有限触发复核', rel(ADDI_OPUS_AUDIT)) + ' · ')
        bmc_updates.append(
            'ADDI 保值的两项局部主安全性均已证。写回 final 为 proved，读回的局部前件认证在 114.001 秒到限，final 为 unknown。'
            '串行 114.198 秒，峰值 189.03 MiB。' + lift
            + link('两项义务、原 SVA 映射及 final', rel(ADDI_OBLIGATIONS / 'README.md'))
        )
    if (LOOP_START_OBLIGATIONS / 'README.md').is_file():
        lift = ('Opus 独立核对 24 项逐拍交接，整片无界安全性成立。'
                + link('独立提升复核', rel(LOOP_START_LIFT_AUDIT)) + ' · ' if LOOP_START_LIFT_AUDIT.is_file()
                else '逐拍提升和原模型严格延拓待核。')
        timeout = '119.001' if LOOP_START_LIFT_AUDIT.is_file() else '119'
        bmc_updates.append(
            f'LOOP_START 保值的两项局部主安全性均已证。写回 final 为 proved，读回的局部前件认证在 {timeout} 秒到限，final 为 unknown。'
            '原片首块 #37–47 命中完整前件，#48 后件相等。#68→#69 是仿真回边候选。' + lift
            + link('两项义务、整片波形和回边候选', rel(LOOP_START_OBLIGATIONS / 'README.md'))
        )
    if (LOOP_END_OBLIGATIONS / 'README.md').is_file():
        lift = ('Opus 独立复核确认 24 项交接可提升为整片无界安全性。该次布局的原片完整前件 0 命中。'
                + link('独立提升及原片采样复核', rel(LOOP_END_LIFT_AUDIT)) + ' · ' if LOOP_END_LIFT_AUDIT.is_file()
                else '原片完整前件及逐拍提升待核。')
        timeout = '119.001' if LOOP_END_LIFT_AUDIT.is_file() else '119'
        bmc_updates.append(
            f'LOOP_END 保值的两项局部主安全性均已证。写回 final 为 proved，读回前件认证在 {timeout} 秒到限，final 为 unknown。' + lift
            + link('两项义务及原 SVA 映射', rel(LOOP_END_OBLIGATIONS / 'README.md'))
        )
    if (MUL_OBLIGATIONS / 'README.md').is_file():
        lift = ('25 项逐拍交接经独立复核，整片无界安全性已证。'
                + link('独立提升复核', rel(MUL_LIFT_AUDIT)) + ' · ' if MUL_LIFT_AUDIT.is_file()
                else '25 项逐拍交接待独立复核。')
        bmc_updates.append(
            'MUL 保值的局部写回隔离 final 为 proved，读回主安全性已证。读回局部前件认证在 119.001 秒到限，'
            'final 为 unknown。' + lift
            + link('原命令、逐拍交接及两份 final', rel(MUL_OBLIGATIONS / 'README.md'))
        )
    if (FIRST_FILL_MUL / 'results.tsv').is_file():
        bmc_updates.append(
            'MUL 原片单事件首块在 #37–47 命中完整前件，#44 由 MUL 支路写 gp3，#48 后件相等。'
            '#68→#69 是仿真回边候选。'
            + link('完整输入、逐拍采样及波形', rel(FIRST_FILL_MUL / 'README.md'))
        )
    if (FIRST_FILL_LOOP_END / 'results.tsv').is_file():
        bmc_updates.append(
            'LOOP_END 保值的定向复放完成真实循环退出。#57 首次 PASS，#59 LOOP_DEC。FP 停顿使第二次 PASS 从要求的 #63 延至 #67，完整前件未命中。'
            + link('原片逐拍输入、窗口和波形', rel(FIRST_FILL_LOOP_END / 'README.md'))
        )
    if (FIRST_FILL_LOOP_END_A / 'results.tsv').is_file():
        bmc_updates.append(
            'LOOP_END 候选 A 仅改 #10 的 D 载荷，#47 改发 PASS。#59–62 仍停顿，#63 无 PASS，原完整前件 0 命中。'
            '两条在途 MAX 来自 word[7] 在 #48/#55 的重复派发。'
            + link('候选 A 唯一复放', rel(FIRST_FILL_LOOP_END_A / 'README.md')) + ' · '
            + link('派发来源勘误', rel(LOOP_END_LAYOUT_AUDIT))
        )
    if (VECTOR_EXP_G5 / 'replay.tsv').is_file():
        bmc_updates.append(
            'Vector EXP 单 lane 字级 BMC 深度 17 在 9.683 秒复现局部反例。第 17 拍 valid=1，实际 0x5fe，参照 0x3df。'
            + link('原性质、局部波形及逐拍 TSV', rel(VECTOR_EXP_CUT_REPORT))
        )
    vector_replays = vector_exp_top_replays()
    if vector_replays:
        exp = next(row for row in report_rows if row['property'] == VECTOR_EXP)
        if exp['verdict'] == 'refuted' and choices.get('6') == 'a':
            closeout = ('Neon 独立核对通过，按第 6 条 a 收为整片反例。原生 SVA 严格认证和无限延拓未取得。'
                        + link('独立接线复核', rel(VECTOR_EXP_TOP_AUDIT)) + ' · '
                        + link('精确复验波形', exp['waveform']) + ' · ')
        else:
            closeout = '有限仿真已取证，整片待结算。'
        bmc_updates.append('Vector EXP ' + vector_exp_top_detail(vector_replays) + closeout
                           + link('两次逐拍采样、顶层输入及 VCD', rel(VECTOR_EXP_TOP_REPORT)))
    for mode, case, attempt in masked_reduction_replays():
        reduction = next(row for row in report_rows if row['property'] == attempt['property'])
        if reduction['verdict'] == 'refuted' and reduction['work_status'] == '按第 6 条 a 收账':
            closeout = ('独立复核通过，按第 6 条 a 收为整片反例，严格认证及无限延拓未取得。'
                        + link('独立复核', reduction['alternative_evidence']) + ' · ')
        else:
            closeout = '保留有限仿真及独立复核状态。'
        bmc_updates.append(f'Vector {mode.upper()} ' + masked_reduction_detail(attempt) + closeout
                           + link('原窗、九槽及完整顶层输入', rel(VECTOR_REDUCTION_MASKED_REPORT)) + ' · '
                           + link('波形', rel(case / 'replay.vcd')))
    if any(r['work_status'] in ('已解除量词解析阻塞，正式求解判定待验收', 'Yices 到限，原性质待结算') for r in report_rows):
        bmc_updates.append(
            'EBMC Spec 13 锁定 f3b352c9 的原样 Vector EXP d27 已成功导出零量词公式，68.74 秒、1,840,960 KiB。'
            'Yices 独立解析通过，27.46 秒、1,316,524 KiB。'
            + (spec13_timeout(VECTOR_SPEC13_EXP) or '正式查询已进入求解，性质判定待验收') + '。'
            + link('导出审计', rel(VECTOR_SPEC13_EXP / 'export.audit'))
            + ' · ' + link('独立解析', rel(VECTOR_SPEC13_EXP / 'parse.log'))
            + ' · ' + link('正式查询日志', rel(VECTOR_SPEC13_EXP / 'solve.log'))
        )
        if VECTOR_SPEC13_REBASED_GATE.is_file():
            gate = VECTOR_SPEC13_REBASED_GATE.read_text()
            assert 'All tests passed (4760 assertions in 118 test cases)' in gate
            assert gate.count('same timeout output after removing the version line') == 3
            bmc_updates.append(
                '交付候选 37e36f7e 通过 118 项单测、4,760 项断言及 12 组回归。三个 CVC5 超时与最新父版一致。'
                + link('交付门禁审计', rel(VECTOR_SPEC13_REBASED_GATE))
            )
        elif VECTOR_SPEC13_GATE.is_file() and 'All tests passed (4760 assertions in 118 test cases)' in VECTOR_SPEC13_GATE.read_text():
            bmc_updates.append(
                'Spec 13 候选通过 118 项单测、4,760 项断言及 12 组回归。CVC5 的两项已知 120 秒超时与父版一致。'
                + link('门禁审计', rel(VECTOR_SPEC13_GATE))
            )
        if VECTOR_SPEC13_REBASE.is_file():
            bmc_updates.append(
                'Spec 13 补丁已重放到 Spec 14 发布基线。EXP d27 保持试跑版 f3b352c9。'
                'SUM/MAX d35 依次用交付候选 37e36f7e，小例一致。'
                + link('两版的二进制与对照', rel(VECTOR_SPEC13_RELEASE))
            )
        for name, run in (('SUM', VECTOR_SPEC13_SUM), ('MAX', VECTOR_SPEC13_MAX)):
            timeout = spec13_timeout(run)
            if timeout:
                bmc_updates.append(
                    f'Vector {name} d35 使用 37e36f7e 导出零量词并通过 Yices 解析。{timeout}。'
                    + link('求解计时', rel(run / 'solve.time')) + ' · ' + link('资源峰值', rel(run / 'solve.memory.peak'))
                )
    if all((G1_PROGRAMS / op / 'max-fp/result.txt').is_file() for op in ('add', 'lui')):
        g1_note = '局部安全性与原片无限延拓继续取证。'
        if G1_OBLIGATIONS_REPORT.is_file():
            g1_note = ('写回隔离和回读隔离的字级归纳安全证明均已出。回读任务的 Crisp 前件认证于 44.001 秒到限。'
                       '独立提升与原片无限延拓继续取证。' + link('四项实测与逐拍义务', rel(G1_OBLIGATIONS_REPORT)))
        if G1_LIFT_AUDIT.is_file():
            g1_note = ('独立复核确认写回与回读义务的 24 项卫条件逐拍同式，整片无界安全性均成立。'
                       '原片无限延拓待认证，优先复用 STORE 的程序缩短方案。'
                       + link('提升审核', rel(G1_LIFT_AUDIT)))
        bmc_updates.append(
            'G1 ADD/LUI 在原取指与 TileLink 下，两条均于 #144–154 命中有限前件。#151 写 gp3，两次读 gp1。'
            + g1_note
            + link('ADD 逐拍核验', rel(G1_PROGRAMS / 'add/max-fp/result.txt'))
            + ' · ' + link('LUI 逐拍核验', rel(G1_PROGRAMS / 'lui/max-fp/result.txt'))
        )
    if PV_CONE_REPORT.is_file():
        pv_proved = any(row['property'] == 'plena_kinds_chip_l1_spec.pv_prefetch_valid_reset' and row['verdict'] == 'proved'
                        for row in report_rows)
        pv_note = ('独立复核确认原接线可提升，复用同包装的严格前件闭环，收为整片无界。' if pv_proved
                   else '原片前件由同包装的严格闭环认证，已派实际切点提升复核。')
        bmc_updates.append(
            '向量预取 valid 的三片原 register_slice、精度选择和 join2 小锥已获 PDR，0.03 秒、17,640 KiB。'
            + pv_note + link('切点结果与原片接线', rel(PV_CONE_REPORT))
        )
    lines.extend([
        '',
        decision_note,
        *(('待结算复核安排 · ' + checkpoint,) if any(row['verdict'] == 'pending' for row in report_rows) else ()),
        '',
        f'旧版 Crisp c82f61c 的整芯片深度 20 有 {legacy} 条通过，记录在逐条 TSV。',
        '',
        '## 历史运行与证据',
        '',
        f'- {main_path}旧版的整设计证据中，53 条首诊断是 SVA→Büchi 不支持，34 条是 IC3 转移约束不支持。{link("排程与资源", "docs/spec/44-plena-original-spec-completion.md")} · {link("长期逐条 TSV", "docs/ref/plena-original-spec.tsv")} · {link("Spec 27 审计", rel(NEON / "spec44_chip87_audit.tsv"))}',
        f'- EBMC Spec 39 发布版 fd5ef342 将原样 int_memory 的非恒真约束从 172 降至 0。Crisp #54 已合入 main ffe177fe。独立重放原配置确认 IC3 的约束拒绝消失，原性质仍报 SVA 转换失败。Spec 43 秒级复现已交 EBMC。{link("EBMC 门禁与约束结果", "docs/ref/ebmc-spec39-disjoint-drivers.md")} · {link("SVA 小例", "experiment/crisp-engineer-sol1-high/spec43/variants/min-sequence-disable.sv")}',
        f'- 正式整芯片 BMC 已完成首轮。Neon 用 8c9c1135 刷新非 CSR 包装。sol2 用 #48 补 CSR 与晚触发，preserve 深度 30 仍为 vacuous，历史见证来自自由输入的局部模型。原样 Vector EXP d27、SUM/MAX d35 导出的唯一 forall 逐字相同，来自两维 packed 向量的 $past 默认全零数组，Yices 拒绝它在 QF_AUFBV 下生成的 forall{vector_plan} {link("EBMC 三目标量词复现", "docs/ref/ebmc-spec44-yices-quantifier.md")} · {link("非 CSR 实时台账", "docs/ref/evidence/spec44-chip/8c9c1135-fe6f8ac1-63d083f9f46e/per_property_results.tsv")} · {link("CSR 与晚触发证据", "docs/ref/spec44-plena-csr-late-trigger.md")}',
        *(('- ' + pdr_queue_note,) if pdr_queue_note else ()),
        *(('- ' + spec43_trial_note,) if spec43_trial_note else ()),
        *(('- ' + official_note,) if official_note else ()),
        *('- ' + update for update in bmc_updates),
        '',
        '## 三条连续两拍复位性质',
        '',
        ('原单拍冷启动配置的三条性质由字级 k-induction k=4 证出无界安全。kind 辅助查询证出前件不可达。'
         if approved else '原单拍冷启动配置按 BMC 结果记有界通过到 d10。')
        + '该配置在 #0 拉高 rst，从 #1 起一直为低。' + link('复位语义', 'docs/spec/18-salt-reset-input.md', 17),
        '',
    ])
    lookup = {r['property']: r for r in report_rows}
    for prop in sorted(RESET):
        row = lookup[prop]
        lines.append(f"- {prop.rsplit('.', 1)[1]}。{link('Crisp 正式证据', row['evidence'])}")
    if (CSR_RESET_CONTRAST / 'VERSIONS_AND_RESULT.txt').is_file():
        lines.extend([
            '',
            '独立两拍复位对照沿用三条原断言，额外约束 rst 在 #0、#1 为高，#2 起为低。'
            'Crisp #55 字级 d4 三条通过，53.47 秒，cgroup 峰值 1,718.01 MiB。'
            '独立波形核对两拍前件在 #1 完成，三组寄存器的 #2 值与 #1 相同。'
            + link('两拍对照配置、版本和结果', rel(CSR_RESET_CONTRAST / 'VERSIONS_AND_RESULT.txt'))
            + ' · ' + link('逐拍波形核验', rel(CSR_RESET_CONTRAST / 'vcd_cycle_check.txt')),
        ])
    lines.extend([
        '',
        '## 五条晚触发',
        '',
        '早期 d20 结果为 vacuous。历史 cover 用原 |=> 将后件换成 rst，记录的拍已包含下一拍的后件采样。',
        '',
        'Preserve depth 30 的原样检查仍为 vacuous。历史局部见证在第 1 拍令归约输出有效，整芯片冷启动同拍取 1 为 UNSAT，翻成 0 为 SAT。原见证第 1–7 拍全部逐值重放也是 UNSAT。完整前件的可达界仍待核。' + link('Preserve 对照与逐拍证据', 'docs/ref/spec44-preserve-witness-context.md'),
        '',
        '| 原目标 | 历史 cover 拍 | 计划深度 | 当前整芯片结果 | 证据 |',
        '| --- | ---: | ---: | --- | --- |',
    ])
    for prop, (hit, bound) in LATE.items():
        row = lookup[prop]
        status = LABELS[row['verdict']]
        if row['verdict'] == 'passed':
            status += f"到 d{row['depth']}"
        elif row['verdict'] == 'pending' and 'Yices 拒绝' in row['detail']:
            status += f" · d{row['depth']} 查询受 Yices 量词限制"
        lines.append(f"| {prop.split('.', 1)[1]} | {hit} | {bound} | {status} | {link('正式 Crisp', row['evidence'])} |")
    lines.extend(['', f'[历史 cover 与正式 vacuous 对照]({ROOT}/docs/ref/spec21-buechi-integration.md)', ''])
    lines.extend([
        '## 整芯片逐条',
        '',
        '| 包装 | 原断言 | 当前状态 | vacuous | 证据 | 下一步 | 预计 UTC |',
        '| --- | --- | --- | --- | --- | --- | --- |',
    ])
    lines.extend(table_line(row, replay_sections) for row in report_rows)
    lines.append('')
    return '\n'.join(lines)


def read_approved_ledger():
    with LEDGER.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader(source, delimiter='\t', strict=True)
        fields = reader.fieldnames or []
        if (len(fields) != 25 or len(set(fields)) != 25 or fields[6:9] != ['verdict', 'depth', 'vacuous']
                or not {'scope', 'property'} <= set(fields)):
            raise ValueError(f'{LEDGER}: expected the 25-column PLENA ledger')
        data = list(reader)
    if any(None in row or None in row.values() or not row['property'] for row in data):
        raise ValueError(f'{LEDGER}: malformed PLENA ledger row')
    by_property = {row['property']: row for row in data}
    if len(data) != 87 or len(by_property) != 87 or any(row['scope'] != 'chip' for row in data):
        raise ValueError(f'{LEDGER}: expected 87 distinct PLENA chip properties')
    for row in data:
        context = f"{LEDGER}: {row['property']}"
        if row['verdict'] not in LABELS:
            raise ValueError(f"{context}: invalid ledger verdict {row['verdict']!r}")
        validate_vacuity(row['vacuous'], context)
    return data, by_property


def refresh_approved_ledger(choices):
    # The published ledger retains verified rows after short-lived run artifacts are removed.
    data, by_property = read_approved_ledger()
    record_retained_triggers(by_property)
    record_approved_bookings(by_property)
    other_results(by_property)
    record_retained_triggers(by_property)
    ext_ready = by_property['ext_20260801_spec.ext_r8_ready_when_requesting']
    if ext_ready['alternative_evidence'] and not (ROOT / ext_ready['alternative_evidence']).is_file():
        ext_ready['alternative_evidence'] = rel(SPEC27_LIFT_REPORT)
    write_approved_ledger(data, choices)


def write_approved_ledger(data, choices):
    for row in data:
        assert row['verdict'] in LABELS, row
        for key in ('source', 'salt', 'evidence', 'other_evidence', 'waveform', 'alternative_evidence'):
            if row[key]:
                assert (ROOT / row[key]).is_file(), (key, row['property'], row[key])
        for key, value in row.items():
            row[key] = value.replace('空' '洞', 'vacuous')
    stamp = report_stamp()
    observations = sorted(chip_observations(), key=lambda item: item[1])
    common_cover = next(rows(INT_COMMON_COVER), None) if INT_COMMON_COVER.is_file() else None
    if common_cover and common_cover['stopped'] != 'cover timeout':
        common_cover = None
    doc = markdown(data, stamp, observations, official_int_memory(), common_cover, int_cover_timeouts(), '', choices)
    write_outputs(data, doc, stamp)


def main():
    choices = {row['item']: row['choice'] for row in rows(PROJECT_DECISIONS)} if PROJECT_DECISIONS.is_file() else {}
    approved = choices.get('7') == 'approved' and choices.get('8') == 'approved'
    if ARGS and (ARGS.evidence_dir or ARGS.manual_updates):
        if not approved:
            raise ValueError('import requires the current approved PLENA ledger and decisions')
        data, by_property = read_approved_ledger()
        if ARGS.verified_outcomes:
            other_results(by_property)
        for directory in ARGS.evidence_dir:
            import_directory(by_property, directory, ROOT)
        if ARGS.manual_updates:
            apply_annotations(by_property, ARGS.manual_updates, ROOT)
        write_approved_ledger(data, choices)
        return
    if approved:
        refresh_approved_ledger(choices)
        return
    data = list(chip_rows())
    assert len(data) == 87 and all(row['scope'] == 'chip' for row in data)
    assert len({row['property'] for row in data}) == len(data)
    by_property = {r['property']: r for r in data}
    record_spec27_lift_review(by_property)
    observations = sorted(chip_observations(), key=lambda item: item[1])
    apply_chip_observations(by_property, observations)
    vector_crosscheck(by_property)
    priority_pdr_queue(by_property)
    prepared_vector_queue(by_property)
    other_results(by_property)
    record_r4_interrupt(by_property)
    record_spec43_trial(by_property)
    memory_pdr = official_int_memory()
    record_official_int_memory(by_property, memory_pdr)
    common_cover = next(rows(INT_COMMON_COVER), None) if INT_COMMON_COVER.is_file() else None
    if common_cover and common_cover['stopped'] != 'cover timeout':
        common_cover = None
    cover_timeouts = int_cover_timeouts()
    record_int_windows(by_property, common_cover, cover_timeouts)
    record_fp_windows(by_property)
    record_kinds_handoff(by_property)
    record_split_handoffs(by_property, observations)
    record_short_tail_witnesses(by_property)
    record_tail_closeout(by_property)
    record_pm_pdr_trial(by_property)
    choices = apply_project_decisions(by_property)
    record_r4_vcd(by_property, choices)
    record_reset_contrast(by_property)
    record_csr_kind(by_property)
    record_local_pdr_attempts(by_property)
    record_fp0_cut(by_property)
    record_gp0_evidence(by_property)
    record_store_cut(by_property)
    record_scalar_memory_and_forwarding(by_property)
    record_fp_preserve_prefix(by_property)
    record_addr2_first_fill(by_property)
    record_load_preserve(by_property)
    record_g1_programs(by_property)
    record_sub_obligations(by_property)
    record_addi_first_fill(by_property)
    record_loop_obligations(by_property, 'loop_start', LOOP_START_OBLIGATIONS)
    record_loop_obligations(by_property, 'loop_end', LOOP_END_OBLIGATIONS)
    record_mul_obligations(by_property)
    record_shared_rst_lasso(by_property)
    record_exp_repeat_release(by_property)
    record_pwrite_lasso(by_property)
    record_ext_r8_evidence(by_property)
    if (REF.parent / 'ebmc/13-yices-packed-past.md').is_file():
        for prop in VECTOR_LATE_QUANTIFIER:
            row = by_property[prop]
            if row['verdict'] == 'pending':
                row['next_action'] = f"等 EBMC Spec 13 的 Yices 编码通过小例门禁，再按原模型深度 {row['depth']} 排正式复验"
    record_spec13_vector_exp(by_property)
    record_vector_exp_g5(by_property)
    record_vector_exp_top(by_property, choices)
    record_vector_sub_and_reci(by_property, choices)
    record_vector_sub_vf(by_property)
    record_vector_add_mul(by_property)
    record_vector_operand_sweep(by_property)
    record_add_int_chip_refutation(by_property, choices)
    record_scalar_gp0_forwarding(by_property, choices)
    record_loop_zero_refutation(by_property, choices)
    record_decision_seven_witnesses(by_property)
    if VECTOR_SPEC13_REBASE.is_file():
        assert 'f3b352c9 = 1:  37e36f7e' in VECTOR_SPEC13_REBASE.read_text()
        for prop in VECTOR_LATE_QUANTIFIER - {VECTOR_EXP}:
            row = by_property[prop]
            if row['verdict'] == 'pending':
                row['next_action'] = '等 EXP 原样求解结束，用锁定交付候选 37e36f7e 串行复验原样 d35'
                row['alternative_evidence'] = rel(VECTOR_SPEC13_RELEASE)
                is_sum = prop.endswith('.v_red_sum__chip_reduction_result')
                run = VECTOR_SPEC13_SUM if is_sum else VECTOR_SPEC13_MAX
                if (run / 'parse.rc').is_file() and (run / 'parse.rc').read_text().strip() == '0':
                    assert (run / 'export.audit').read_text().split() == ['quantifier_lines', '0', 'check_sat', '1']
                    timeout = spec13_timeout(run)
                    row['detail'] += '。37e36f7e 原样 d35 已导出零量词并通过 Yices 解析'
                    row.update(next_action='核对原样 d35 求解结果', other_evidence=rel(run / 'export.audit'))
                    if timeout:
                        row['detail'] += '。' + timeout
                        row.update(work_status='Yices 到限，原性质待结算', other_evidence=rel(run / 'solve.time'),
                                   next_action='保留到限与去量词证据，转核原局部安全义务')
                elif (run / 'ebmc.version').is_file():
                    row.update(work_status='交付候选原样 d35 已启动', next_action='核对原样 d35 导出、解析与求解结果',
                               other_evidence=rel(run / 'ebmc.version'))
    record_masked_reduction(by_property, choices)
    now = dt.datetime.now(dt.timezone.utc)
    checkpoint = update_review_schedule(data, now, choices)
    record_fp_prefix_proofs(by_property)
    record_local_pdr_starts(by_property, now)
    record_pm_witness_start(by_property, now)
    fp0 = by_property[FLOAT_ZERO]
    fp0_veval_audit = REF / 'evidence/spec27/fp0-veval-basecase-opus.md'
    assert fp0_veval_audit.is_file(), fp0_veval_audit
    fp0['detail'] = fp0['detail'].rstrip('。') + (
        '。veval 11-15 整片 KI 的 refuted 是基例反例，但查的是旧版 PLENA'
        '（regfile_2p1w 读前递缺 waddr!=0 守卫，f0 读到前递的 SRAM 自由值）；'
        f'原片 bab29dba 逐拍复放不复现，与本表 d5 通过一致。证据 {rel(fp0_veval_audit)}'
    )
    record_retained_triggers(by_property)
    for row in data:
        for key in ('source', 'salt', 'evidence', 'other_evidence', 'waveform', 'alternative_evidence'):
            if row[key]:
                assert (ROOT / row[key]).is_file(), (key, row['property'], row[key])
        if row['verdict'] == 'proved':
            assert all(row[key] for key in ('engine', 'wall_s', 'peak_kib', 'crisp_sha', 'ebmc_sha')), row
        assert row['next_action'] and row['eta_utc'], row
    stamp = report_stamp(now)
    doc = markdown(data, stamp, observations, memory_pdr, common_cover, cover_timeouts, checkpoint, choices)
    write_outputs(data, doc, stamp)


def report_stamp(now=None):
    if ARGS and ARGS.stamp:
        return ARGS.stamp
    return (now or dt.datetime.now(dt.timezone.utc)).strftime('%Y-%m-%d %H:%M UTC')


def write_outputs(data, doc, stamp):
    distribution = vacuous_counts(data)
    assert all(row['verdict'] in LABELS for row in data)
    fields = [key for key in data[0] if key != 'vacuous']
    fields.insert(fields.index('depth') + 1, 'vacuous')
    ledger, report = LEDGER, REPORT
    if ARGS and ARGS.output_dir:
        directory = ARGS.output_dir.resolve()
        ledger, report = directory / LEDGER.name, directory / REPORT.name
        if {ledger.resolve(), report.resolve()} & {LEDGER.resolve(), REPORT.resolve()}:
            raise ValueError('--output-dir must leave the input ledger and report intact')
    if ledger.resolve() == report.resolve():
        raise ValueError('ledger and report must use distinct paths')
    for dest in (ledger, report):
        if dest.exists() and not dest.is_file():
            raise ValueError(f'{dest}: expected a file')
    with ExitStack() as cleanup:
        staged = []
        for dest in (ledger, report):
            with NamedTemporaryFile(mode='w', encoding='utf-8', newline='', dir=dest.parent,
                                    prefix=f'.{dest.name}.', delete=False) as target:
                temporary = Path(target.name)
                cleanup.callback(temporary.unlink, missing_ok=True)
                if dest == ledger:
                    output = csv.DictWriter(target, fieldnames=fields, delimiter='\t', lineterminator='\n')
                    output.writeheader()
                    output.writerows(data)
                else:
                    target.write(doc.replace('空' '洞', 'vacuous'))
            if dest.exists():
                temporary.chmod(dest.stat().st_mode & 0o777)
            staged.append((temporary, dest))
        for temporary, dest in staged:
            temporary.replace(dest)
    print(stamp, Counter((r['scope'], r['verdict']) for r in data))
    print('vacuous', distribution)


def refresh_csr_kind():
    data, by_property = read_approved_ledger()
    previous_counts = chip_counts_line(data)
    record_csr_kind(by_property)
    stamp = report_stamp()
    doc = REPORT.read_text()
    assert doc.count(previous_counts) == 1, REPORT
    doc = doc.replace(previous_counts, chip_counts_line(data))
    doc, count = re.subn(r'(?m)^更新于 \S+ \S+ UTC', f'更新于 {stamp}', doc)
    assert count == 1, REPORT
    for prop, row in by_property.items():
        if prop.startswith((CSR_RESET_NO, CSR_SET)):
            label = re.escape(prop.split('.', 1)[1])
            pattern = rf'(?m)^\| {CSR} \| \[{label}\]\([^\n]*$'
            doc, count = re.subn(pattern, lambda _: table_line(row, {}), doc)
            assert count == 1, prop
    write_outputs(data, doc, stamp)


if __name__ == '__main__':
    try:
        if ARGS.csr_kind:
            refresh_csr_kind()
        else:
            main()
    except (OSError, ValueError, csv.Error) as error:
        PARSER.exit(2, f'error: {error}\n')
