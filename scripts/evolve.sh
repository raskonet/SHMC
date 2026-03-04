#!/usr/bin/env bash
# scripts/evolve.sh  --  Evolve a seed DSL into better music via MCTS
# Usage: ./scripts/evolve.sh <seed.shmc> [output.wav] [--iters N] [--play]
set -euo pipefail
cd "$(dirname "$0")/.."
EVOLVE=bin/shmc_evolve
SEED=""; OUTPUT=""; ITERS=60; BEAM=8; PLAY=false
usage(){ echo "Usage: $0 <seed.shmc> [out.wav] [--iters N] [--play]"; exit 1; }
[ $# -eq 0 ] && usage
while [[ $# -gt 0 ]]; do
  case "$1" in
    --iters)  ITERS="$2"; shift 2 ;;
    --play)   PLAY=true; shift ;;
    --help)   usage ;;
    *.shmc|*.dsl) [ -z "$SEED" ] && SEED="$1" || OUTPUT="$1"; shift ;;
    *.wav)    OUTPUT="$1"; shift ;;
    *) echo "Unknown: $1"; usage ;;
  esac
done
[ -z "$SEED" ] && { echo "Error: no seed file"; usage; }
[ -f "$SEED" ] || { echo "Error: not found: $SEED"; exit 1; }
[ -x "$EVOLVE" ] || { echo "Error: run make first"; exit 1; }
BASE=$(basename "$SEED" .shmc); BASE=$(basename "$BASE" .dsl)
[ -z "$OUTPUT" ] && OUTPUT="evolved_${BASE}.wav"
echo ""
echo "  Seed:    $SEED"
echo "  Output:  $OUTPUT  (MCTS iters=$ITERS)"
echo "  Fitness: 60% harmony + 35% audio diversity + 5% novelty"
echo ""
"$EVOLVE" "$SEED" "$ITERS" "$BEAM"
[ -f mcts_evolved.wav ] && mv mcts_evolved.wav "$OUTPUT"
[ -f beam_evolved.wav ] && mv beam_evolved.wav "${OUTPUT%.wav}_beam.wav"
echo ""
echo "  Done!  MCTS -> $OUTPUT"
if $PLAY; then
  command -v aplay  &>/dev/null && aplay  "$OUTPUT" && exit
  command -v afplay &>/dev/null && afplay "$OUTPUT" && exit
  echo "  (no player -- open $OUTPUT manually)"
fi
