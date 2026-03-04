#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════
# shmc.sh  --  SHMC unified command-line interface
#
# Commands:
#   compose  Generate music from a text prompt (needs Lemonade server)
#   evolve   Evolve an existing .shmc file into something better
#   render   Convert a .shmc DSL file directly to WAV
#   verify   Run all verification suites
#   build    Build / rebuild all binaries
#   help     Show this message
#
# Quick start:
#   make                                # build everything first
#   ./shmc.sh compose --prompt "jazz piano at 90 BPM"
#   ./shmc.sh evolve  lemonade/examples/blues_a.shmc
#   ./shmc.sh render  lemonade/examples/complex_song_v2.dsl out.wav
#   ./shmc.sh verify
# ══════════════════════════════════════════════════════════════════════
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

RED='\033[0;31m'; GRN='\033[0;32m'; CYN='\033[0;36m'; YLW='\033[1;33m'
NC='\033[0m'; BLD='\033[1m'

cmd="${1:-help}"
shift || true

# ── helpers ────────────────────────────────────────────────────────────
need_build() {
    local b="$1"
    if [ ! -x "$b" ]; then
        echo -e "${RED}Error: $b not built.${NC}  Run: ${BLD}make${NC}"
        exit 1
    fi
}

banner() {
    echo ""
    echo -e "${BLD}╔══════════════════════════════════════════════╗${NC}"
    printf "${BLD}║  %-44s║${NC}\n" "$*"
    echo -e "${BLD}╚══════════════════════════════════════════════╝${NC}"
    echo ""
}

# ── commands ───────────────────────────────────────────────────────────

case "$cmd" in

# ── compose: prompt -> LLM -> DSL -> WAV [-> evolve] ─────────────────
compose)
    banner "SHMC Compose"
    echo -e "  Requires: Lemonade server at ${CYN}localhost:8000${NC}"
    echo -e "  Docs:     ${CYN}https://lemonade-server.ai${NC}"
    echo ""
    if [ ! -f scripts/compose.py ]; then
        echo -e "${RED}Error: scripts/compose.py not found${NC}"; exit 1
    fi
    exec python3 scripts/compose.py "$@"
    ;;

# ── evolve: .shmc -> MCTS evolution -> WAV ───────────────────────────
evolve)
    banner "SHMC Evolve"
    need_build bin/shmc_evolve
    exec scripts/evolve.sh "$@"
    ;;

# ── render: .shmc -> WAV (no evolution) ──────────────────────────────
render)
    need_build bin/shmc_render
    if [ $# -lt 1 ]; then
        echo "Usage: $0 render <input.shmc> [output.wav]"
        echo ""
        echo "Examples:"
        echo "  $0 render lemonade/examples/blues_a.shmc"
        echo "  $0 render lemonade/examples/jazz_loop.shmc my_jazz.wav"
        exit 1
    fi
    INPUT="$1"
    OUTPUT="${2:-${INPUT%.shmc}.wav}"
    OUTPUT="${OUTPUT%.dsl}.wav"
    echo ""
    echo -e "  Rendering: ${CYN}$INPUT${NC}"
    echo -e "  Output:    ${CYN}$OUTPUT${NC}"
    echo ""
    bin/shmc_render "$INPUT" "$OUTPUT"
    echo ""
    echo -e "${GRN}  Done! -> $OUTPUT${NC}"
    echo ""
    ;;

# ── verify: run all test suites ──────────────────────────────────────
verify)
    if [ ! -d bin/verify ]; then
        echo -e "${YLW}Verifier binaries not found — building first...${NC}"
        make verifiers
    fi
    exec scripts/verify.sh "$@"
    ;;

# ── build: compile everything ─────────────────────────────────────────
build)
    exec make "$@"
    ;;

# ── help ──────────────────────────────────────────────────────────────
help|--help|-h|"")
    echo ""
    echo -e "${BLD}  SHMC — Symbolic Harmony Music Composer${NC}"
    echo ""
    echo -e "  ${BLD}Usage:${NC}  ./shmc.sh <command> [options]"
    echo ""
    echo -e "  ${BLD}Commands:${NC}"
    echo -e "    ${CYN}compose${NC}   Generate music from a text prompt"
    echo -e "    ${CYN}evolve${NC}    Evolve a .shmc file with MCTS + harmony fitness"
    echo -e "    ${CYN}render${NC}    Render a .shmc DSL file to WAV (no evolution)"
    echo -e "    ${CYN}verify${NC}    Run all verification suites"
    echo -e "    ${CYN}build${NC}     Compile/rebuild all binaries"
    echo -e "    ${CYN}help${NC}      Show this message"
    echo ""
    echo -e "  ${BLD}Quick start:${NC}"
    echo -e "    make"
    echo -e "    ./shmc.sh compose --prompt 'jazz piano at 90 BPM'"
    echo -e "    ./shmc.sh compose --prompt 'dark techno 138 BPM' --evolve --iters 80"
    echo -e "    ./shmc.sh evolve  lemonade/examples/blues_a.shmc"
    echo -e "    ./shmc.sh evolve  lemonade/examples/jazz_loop.shmc out.wav --iters 100 --play"
    echo -e "    ./shmc.sh render  lemonade/examples/complex_song_v2.dsl out.wav"
    echo -e "    ./shmc.sh verify"
    echo -e "    ./shmc.sh verify --fast    # C only, ~10 seconds"
    echo ""
    echo -e "  ${BLD}Compose options:${NC}"
    echo -e "    --prompt TEXT   Musical description (required)"
    echo -e "    --out FILE      Output WAV (default: composed.wav)"
    echo -e "    --evolve        Also run MCTS evolution on the result"
    echo -e "    --iters N       MCTS iterations (default: 60)"
    echo -e "    --play          Play the result when done"
    echo -e "    --dry-run       Generate DSL only, skip rendering"
    echo -e "    --model NAME    Lemonade model (default: Qwen3-4B-GGUF)"
    echo ""
    echo -e "  ${BLD}Evolve options:${NC}"
    echo -e "    <seed.shmc>     Input DSL file (required)"
    echo -e "    [output.wav]    Output WAV (default: evolved_<name>.wav)"
    echo -e "    --iters N       MCTS iterations (default: 60)"
    echo -e "    --play          Play when done"
    echo ""
    echo -e "  ${BLD}Fitness function (Stage 12):${NC}"
    echo -e "    60% symbolic harmony  (Lerdahl tonal space, voice leading, tension arc)"
    echo -e "    35% audio diversity   (spectral + temporal variation)"
    echo -e "    5%  novelty           (archive distance)"
    echo ""
    ;;

*)
    echo -e "${RED}Unknown command: $cmd${NC}"
    echo "  Run: ./shmc.sh help"
    exit 1
    ;;
esac
