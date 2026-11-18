#!/usr/bin/env bash
set -eu
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

usage() {
    cat <<'EOF'
Usage: crisp COMMAND [arguments]

  verify RTL... --top TOP --spec FILE  Run the experimental module verification workflow
  mine  DESIGN.sv [options]       Mine assertions with SMART / MAPminer
  lower TASK.sby --out DIRECTORY  Lower properties with veval
  check TASK.sby [options]        Check module-level properties with veval and SBY

Run crisp COMMAND --help for component options.
EOF
}

case "${1:-}" in
    -h|--help|'') usage; exit 0 ;;
    verify) program="$root/src/verify.py"; build_target=veval; shift ;;
    mine)  program="$root/components/smartVerilog/build/smart"; build_target=smart; shift ;;
    lower|check) program="$root/components/veval/build/veval"; build_target=veval ;;
    *) usage >&2; exit 2 ;;
esac

if [[ ! -x "$program" ]]; then
    echo "Missing $program. Run make $build_target in $root." >&2
    exit 2
fi

bundled_suite="$root/components/smartVerilog/.deps/oss-cad-suite/bin"
if [[ -z ${OSS_CAD_BIN:-} && -x "$bundled_suite/yosys-config" ]]; then
    export OSS_CAD_BIN="$bundled_suite"
fi
if [[ -n ${OSS_CAD_BIN:-} ]]; then PATH="$OSS_CAD_BIN:$PATH"; fi
for dir in "$HOME/.local/oss-cad-suite/bin" "$HOME/.local/spot/bin"; do
    if [[ -d "$dir" ]]; then PATH="$PATH:$dir"; fi
done
export PATH
exec "$program" "$@"
