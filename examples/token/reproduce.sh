#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
case "$#" in
    0) directory=log ;;
    1) if [[ $1 == --help ]]; then echo 'Usage: reproduce.sh [--out LOG_DIRECTORY]'; exit 0; fi; exit 2 ;;
    2) [[ $1 == --out ]] || exit 2; directory=$2 ;;
    *) echo 'Usage: reproduce.sh [--out LOG_DIRECTORY]' >&2; exit 2 ;;
esac
mkdir -p -- "$directory"
run="$(date -u +%Y%m%dT%H%M%SZ)-$$"
for mode in whole split split-lemmas whole-lemmas; do
    split=off
    lemmas=off
    instance=token_formal.dut
    verdict=unknown
    expected=3
    if [[ $mode == split* ]]; then split=on; instance+=.state; fi
    if [[ $mode == *-lemmas ]]; then lemmas=on; verdict=proved; expected=0; fi
    log="$directory/token-$mode.$run.log"
    code=0
    "$root/src/cli.sh" verify "$root/examples/token/rtl/token.sv" --top token_formal \
        --spec "$root/examples/token/spec.sv" --split "$split" --lemmas "$lemmas" \
        --budget 60 --depth 5 --log "$log" > /dev/null || code=$?
    if [[ $code != "$expected" ]] || ! grep -Fxq "token_formal.p_no_conflict @ $instance: $verdict" "$log"; then
        echo "$mode: expected $verdict at $instance; see $log" >&2
        exit 1
    fi
    if [[ $lemmas == on ]]; then
        grep -Eq '^Retry with [1-9][0-9]* independently proved lemma\(s\)$' "$log"
        grep -Fxq 'Sources with all selected module checks proved: 1/1' "$log"
    else
        grep -Fxq 'Sources with all selected module checks proved: 0/1' "$log"
    fi
    echo "$mode: $verdict"
done
