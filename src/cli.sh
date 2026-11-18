#!/usr/bin/env bash
set -eu
set -o pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
veval="$root/components/veval/build/veval"
smart="$root/components/smartVerilog/build/smart"
driver="$root/components/veval/build/crisp/crisp_driver"

usage() {
    cat <<'HELP'
Usage: crisp COMMAND [arguments] [--out LOG_DIRECTORY | --log FILE.log]

  verify RTL... --top TOP --spec FILE  Check assertions on the selected modules
         [--split on|off] [--lemmas on|off] [--depth N] [--budget SECONDS]
  mine DESIGN.sv [SMART options]       Print mined assertions
  lower TASK.sby [TASK] [options]      Print lowered properties and their instances
  check TASK.sby [TASK] [options]      Print module verdicts and counterexamples

Each run writes a text log in ./log by default. --out selects the log directory.
--log selects a new .log file. Component work files are temporary.
HELP
}

case "${1:-}" in
    ''|-h|--help) usage; exit 0 ;;
    verify|mine|lower|check) command=$1; shift ;;
    *) usage >&2; exit 2 ;;
esac
logdir= logpath= args=() dump_config=no
while (( $# )); do
    case "$1" in
        --out|--log|-o|--output)
            option=$1
            if (( $# < 2 )) || [[ -z $2 ]]; then echo "$1 needs a path" >&2; exit 2; fi
            if [[ $option == --out ]]; then logdir=$2; else logpath=$2; fi
            shift 2 ;;
        --out=*) logdir=${1#*=}; shift ;;
        --log=*|--output=*) logpath=${1#*=}; shift ;;
        --workdir|--workdir=*|--keep-work|--resume|--lowering|--lowering=*|--block)
            echo "$1 requests a persistent component workspace. Use --out DIR for Crisp logs." >&2; exit 2 ;;
        --dump-config) dump_config=yes; shift ;;
        -h|--help)
            usage
            if [[ $command == mine && -x $smart ]]; then "$smart" --help; fi
            if [[ ( $command == lower || $command == check ) && -x $veval ]]; then "$veval" "$command" --help; fi
            exit 0 ;;
        --*=*)
            if [[ $command == verify ]]; then args+=("${1%%=*}" "${1#*=}"); else args+=("$1"); fi
            shift ;;
        *) args+=("$1"); shift ;;
    esac
done
if [[ -n $logdir && -n $logpath ]]; then echo 'Choose either --out or --log.' >&2; exit 2; fi
if [[ -n $logpath && $logpath != *.log ]]; then echo 'Choose a .log file for output.' >&2; exit 2; fi

program=$driver
if [[ $command == mine ]]; then program=$smart; fi
if [[ ! -x $program ]]; then echo "Missing $program. Run make build in $root." >&2; exit 2; fi
suite="$root/components/smartVerilog/.deps/oss-cad-suite/bin"
if [[ -d $root/runtime/suite ]]; then
    export OSS_CAD_BIN="$root/runtime/suite/bin"
    export CONSFLOW_BUILD_DIR="$root/components/veval/build" CONSFLOW_WORKFLOW_ROOT="$root/components/veval"
    export LD_LIBRARY_PATH="$root/runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    PATH="$root/runtime/bin:$PATH"
fi
if [[ -z ${OSS_CAD_BIN:-} && -x "$suite/yosys-config" ]]; then export OSS_CAD_BIN="$suite"; fi
if [[ -n ${OSS_CAD_BIN:-} ]]; then PATH="$OSS_CAD_BIN:$PATH"; fi
for dir in "$HOME/.local/oss-cad-suite/bin" "$HOME/.local/spot/bin"; do
    if [[ -d $dir ]]; then PATH="$PATH:$dir"; fi
done
export PATH PYTHONDONTWRITEBYTECODE=1
if [[ $dump_config == yes ]]; then
    [[ $command == mine ]] || { echo '--dump-config belongs to SMART.' >&2; exit 2; }
    exec "$smart" "${args[@]}" --dump-config
fi

# Native tools create proof receipts, waveforms and solver models here. Never
# inherit a TMPDIR pointing into a user's source tree.
scratch=$(mktemp -d /tmp/crisp.XXXXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$scratch/tmp"
export TMPDIR="$scratch/tmp"

if [[ -z $logpath ]]; then
    logdir=${logdir:-log}
    mkdir -p -- "$logdir"
    logpath=$(mktemp -- "$logdir/$command.XXXXXXXX.log")
else
    mkdir -p -- "$(dirname -- "$logpath")"
    if ! (set -C; : > "$logpath") 2>/dev/null; then echo "Log already exists: $logpath" >&2; exit 2; fi
fi
echo "Log: $logpath" >&2

run() {
    case "$command" in
        verify) "$driver" verify "$root" "$scratch" "${args[@]}" ;;
        mine)
            if [[ -d $root/runtime/suite ]]; then args=(--simulator iverilog "${args[@]}"); fi
            "$smart" "${args[@]}" --workdir "$scratch/mining" --output "$scratch/assertions.sv" || return $?
            if [[ -f $scratch/assertions.sv ]]; then cat -- "$scratch/assertions.sv"; fi ;;
        lower)
            "$veval" lower "${args[@]}" --out "$scratch/lowering" || return $?
            "$driver" show-lowering "$scratch/lowering" ;;
        check)
            local code=0
            "$veval" check "${args[@]}" --out "$scratch/check" --lowering "$scratch/lowering" --keep-work || code=$?
            if [[ -f $scratch/check/check.json ]]; then "$driver" show-checks "$scratch/check" || return $?; fi
            return "$code" ;;
    esac
}
run 2>&1 | tee -- "$logpath"
