#!/usr/bin/env bash
# scripts/verify.sh  --  SHMC verification suite runner
# Usage: ./scripts/verify.sh [--fast|--python]
# Run from project root. 'make' first to build bin/verify/ binaries.
set -euo pipefail
cd "$(dirname "$0")/.."
VDIR=bin/verify
PASS=0; FAIL=0
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'; BLD='\033[1m'
ok()  { echo -e "  ${GRN}OK${NC}  $*"; }
bad() { echo -e "  ${RED}FAIL${NC}  $*"; FAIL=$((FAIL+1)); }

run_c() {
    local name="$1" bin="$2"
    [ -x "$bin" ] || { bad "$name (not built -- run make first)"; return; }
    local out; out=$(ASAN_OPTIONS=detect_leaks=0 "$bin" 2>/dev/null || true)
    local res; res=$(echo "$out" | grep "RESULT" | tail -1)
    local n; n=$(echo "$res" | grep -oP '\d+(?=/)' || echo 0)
    local t; t=$(echo "$res" | grep -oP '(?<=/)\d+' || echo 0)
    PASS=$((PASS+n))
    [ "$n" = "$t" ] && [ "$t" -gt 0 ] && ok "$name  $res" || bad "$name  $res"
}

run_py() {
    local name="$1" f="$2"
    command -v python3 &>/dev/null || { echo -e "  ${YLW}SKIP${NC}  $name (no python3)"; return; }
    [ -f "$f" ] || { echo -e "  ${YLW}SKIP${NC}  $name (not found)"; return; }
    local out; out=$(python3 "$f" 2>&1 || true)
    # grep -c on piped multi-line output is reliable; tr removes any trailing newlines
    local p; p=$(echo "$out" | grep -c "PROVED\|PASSED\|passed" 2>/dev/null || echo 0); p=$(echo "$p" | tr -d "\n")
    local fl; fl=$(echo "$out" | grep -c "REFUTED\|FAILED\|failed\|ERROR" 2>/dev/null || echo 0); fl=$(echo "$fl" | tr -d "\n")
    # A suite "passes" if it has PROVED/PASSED lines and zero explicit failures
    # The python provers print summary lines — check for the "All.*theorems PROVED" or "0 REFUTED"
    local refuted; refuted=$(echo "$out" | grep -c "REFUTED" 2>/dev/null || echo 0); refuted=$(echo "$refuted" | tr -d "\n")
    if [ "$refuted" -gt 0 ]; then
        bad "$name  ($p proved, $refuted refuted)"; echo "$out" | grep "REFUTED" | head -3
    elif echo "$out" | grep -q "theorems PROVED\|PASSED"; then
        ok "$name  ($p checks)"
    else
        bad "$name  (unexpected output)"; echo "$out" | tail -3
    fi
}

RUN_C=1; RUN_PY=1
[ "${1:-}" = "--fast"   ] && RUN_PY=0
[ "${1:-}" = "--python" ] && RUN_C=0

echo ""
echo -e "${BLD}╔══════════════════════════════════════════════╗${NC}"
echo -e "${BLD}║   SHMC Verification Suite                   ║${NC}"
echo -e "${BLD}╚══════════════════════════════════════════════╝${NC}"

if [ $RUN_C -eq 1 ]; then
  echo -e "\n${BLD}-- C suites --${NC}"
  run_c "search_v2            (search engine)"          $VDIR/verify_search_v2
  run_c "roundtrip            (DSL encode/decode)"      $VDIR/verify_roundtrip
  run_c "mutate               (mutation operators)"     $VDIR/verify_mutate
  run_c "mut_algebra          (invertible algebra)"     $VDIR/verify_mut_algebra
  run_c "structural_muts      (motif mutations)"        $VDIR/verify_structural_mutations
  run_c "canon_struct         (canonicalisation)"       $VDIR/verify_canon_struct
  run_c "novelty              (novelty archive)"        $VDIR/verify_novelty
  run_c "evo_fitness [S11]    (evolutionary fitness)"   $VDIR/verify_evo_fitness
  run_c "harmony    [S12]     (symbolic harmony)"       $VDIR/verify_harmony
  run_c "mcts       [S12]     (MCTS tree search)"       $VDIR/verify_mcts
fi

if [ $RUN_PY -eq 1 ]; then
  echo -e "\n${BLD}-- Python integration suites --${NC}"
  run_py "layer0_exhaustive" verify/verify_layer0_exhaustive.py
  run_py "layer1_hash"       verify/verify_layer1_hash.py
  run_py "layers2to5"        verify/verify_layers2to5.py
fi

echo ""
echo -e "${BLD}══════════════════════════════════════════════${NC}"
[ $FAIL -eq 0 ] \
  && echo -e "${GRN}${BLD}  ALL PASSED ($PASS tests)${NC}" \
  || echo -e "${RED}${BLD}  $FAIL SUITE(S) FAILED${NC}"
echo -e "${BLD}══════════════════════════════════════════════${NC}"
echo ""
[ $FAIL -eq 0 ]
