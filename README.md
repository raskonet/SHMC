# SHMC — Symbolic Harmony Music Compiler

A procedural music system that evolves DSL programs into structured, harmonically coherent audio. Combines **MCTS tree search**, a **multi-layer fitness function** grounded in music theory, and a local **LLM** that translates natural language prompts into code.

*Note: The current fitness metric heuristics (especially the symbolic harmony evaluations) are highly experimental and actively being tuned to better correlate with human acoustic aesthetics.*

The core idea: instead of generating waveforms directly, SHMC generates and mutates *programs*. The same DSL file always produces the same WAV — fully deterministic, inspectable, and editable.

---

## Quick Start

```bash
# 1. Clone the repo
git clone https://github.com/raskonet/SHMC && cd SHMC

# 2. Build everything
make

# 3. Start your local LLM server (separate terminal, see options below)

# 4. Compose from a text prompt
./shmc.sh compose --prompt "jazz piano at 90 BPM" --play

# 5. Evolve an existing piece with MCTS
./shmc.sh evolve lemonade/examples/blues_a.shmc --play

# 6. Render a DSL file directly to WAV (no evolution)
./shmc.sh render lemonade/examples/complex_song_v2.dsl out.wav

# 7. Run all 182 formal verification tests
./shmc.sh verify
```

---

## LLM Setup

SHMC needs an LLM server to translate English prompts into DSL. There are three ways to run it:

**Option 1 — Lemonade SDK (Recommended)**
Use the official Lemonade server to pull and run the model.
```bash
# Pull the model
lemonade-server pull user.SeedCoder --checkpoint unsloth/Seed-Coder-8B-Instruct-GGUF:Q4_K_M --recipe llamacpp

# Run it (tune --ctx-size and -ngl to fit your hardware)
lemonade-server run user.SeedCoder --port 8002 --ctx-size 4096 --llamacpp-args "-ngl 4"
```

**Option 2 — Custom llama-server script (Good for low-RAM iGPUs)**
If Lemonade's auto-management uses too much RAM, use the fallback script:
```bash
./start_llama.sh
# You can optionally specify a custom GGUF path:
# ./start_llama.sh --path_to_gguf /path/to/model.gguf
```

**Option 3 — Groq API (fast, free tier)**
If local inference is too slow, use the included proxy that forwards to Groq:
```bash
export GROQ_API_KEY=gsk_...
python3 groq_proxy.py   # listens on port 8002, same interface as llama-server
```

---

## Commands

### `compose` — text prompt to WAV

```bash
./shmc.sh compose --prompt "upbeat jazz piano at 120 BPM"
./shmc.sh compose --prompt "dark ambient synth" --evolve --iters 80
./shmc.sh compose --prompt "slow blues at 75 BPM" --out blues.wav --evolve --iters 60 --play
```

| Option | Default | Description |
|--------|---------|-------------|
| `--prompt TEXT` | required | Musical description |
| `--out FILE` | `composed.wav` | Output WAV path |
| `--evolve` | off | Run MCTS after rendering |
| `--iters N` | `60` | MCTS iterations |
| `--play` | off | Play result when done |
| `--dry-run` | off | Generate DSL only, skip render |
| `--model NAME` | `Seed-Coder-8B-Instruct-Q4_K_M` | LLM model name |

### `evolve` — evolve an existing DSL file

```bash
./shmc.sh evolve lemonade/examples/blues_a.shmc --iters 80 --play
```

### `render` — compile DSL to WAV instantly

```bash
./shmc.sh render lemonade/examples/blues_a.shmc
./shmc.sh render lemonade/examples/jazz_loop.shmc my_output.wav
```

---

## DSL Format

SHMC programs describe music as a four-level hierarchy: patches define synth sounds, motifs define note sequences, sections schedule motifs on a beat grid, songs sequence sections.

```
PATCH fm_bass {
    osc sin ONE
    osc sin ONE
    fm $0 $1 5        # FM synthesis: carrier, modulator, depth
    adsr 1 6 20 8     # attack decay sustain release (indices 0-31)
    mul $2 $3
    lpf $4 20         # low-pass cutoff (0-63)
    out $5
}

MOTIF bass_line {
    note 48 4 10      # MIDI pitch, duration index (3=8th 4=4th 5=half), velocity (0-15)
    note 52 4 9
    rest 4
}

SECTION verse 16.0 {
    use bass_line @ 0 x4 patch fm_bass
    use bass_line @ 8 x4 patch fm_bass t=5 v=0.9   # t=transpose semitones, v=vel scale
}

SONG my_song 120.0 {
    play verse x4
}
```

`$N` refers to the Nth operation's output register. `ONE` is a constant 1.0 used as a frequency multiplier for oscillators. See `lemonade/examples/` for complete working examples.

---

## Fitness Function

Three-component fitness, no hardcoded musical targets:

```
total_fitness = 0.60 x harmony_score
              + 0.35 x evo_fitness
              + 0.05 x novelty
```

### Symbolic Harmony (60%) — `shmc_harmony.c`

Computed directly from the DSL world structure, no audio render required:

- **H1 Scale consistency** — fraction of notes fitting the detected key (Lerdahl 2001)
- **H2 Harmonic consonance** — mean consonance of simultaneous note pairs (Sethares 1998)
- **H3 Voice leading** — smoothness of pitch motion between successive notes (Tymoczko 2011)
- **H4 Tension arc** — whether tension rises then resolves (Herremans & Chew 2017)
- **H5 Cadence** — root motion scoring toward tonic resolution (Piston 1978)
- **H6 Lerdahl tonal tension** — tonal distance in pitch-class space across time scales

### Audio Diversity (35%) — `shmc_evo_fitness.c`

Measured from a 3-second preview render. Rewards programs that change over time:

- **Spectral diversity** — mean pairwise L2 distance between bark-band frame embeddings
- **Self-dissimilarity** — 1 minus mean cosine similarity between frame pairs
- **Temporal dynamics** — coefficient of variation of block RMS

### Novelty (5%) — `shmc_mcts.c`

Mean distance of the current audio embedding to its k=5 nearest neighbours in a 512-entry ring-buffer archive. Stagnation detection boosts the novelty weight from 0.05 to 0.40 when fitness stops improving for 5 consecutive generations.

---

## MCTS Search

UCT-MCTS over the space of DSL programs.

**Why MCTS over beam search:** beam selection collapses population variance every generation. MCTS maintains a tree that can revisit earlier branching points and explore multiple lineages from the same ancestor.

**UCT formula:** `Q/N + sqrt(2) * sqrt(ln(N_parent) / N_child)`

**Mutation distribution:**
- 30% note pitch — primary melodic exploration
- 20% transpose — harmonic function shifts
- 15% structural motif ops — invert, retrograde, augment, diminish
- 15% harmonic mutations — circle-of-5ths, chord substitution, secondary dominant
- 15% rhythm — note duration, beat offset
- 5% patch structure — DSP op rewiring (rare, preserves timbre stability)

**Policy guidance:** after each pitch mutation, notes are probabilistically snapped to the nearest in-scale pitch (70% probability), biasing toward diatonic regions without hard constraints.

**MAP-Elites:** a 6^4 = 1296-cell behavior grid (brightness x rhythm density x pitch diversity x tonal tension) tracks the best program found in each behavioral region, ensuring diverse exploration.

---

## Project Structure

```
shmc/
├── shmc.sh                    <- unified CLI entry point
├── Makefile
├── start_llama.sh             <- llama-server launch script
├── groq_proxy.py              <- Groq API proxy (drop-in for slow hardware)
├── scripts/
│   ├── compose.py             <- LLM -> DSL -> render pipeline
│   └── evolve.sh              <- MCTS evolution wrapper
├── lemonade/
│   ├── include/               <- public headers
│   ├── src/                   <- DSL compiler, MCTS, harmony, mutation, search
│   ├── tools/                 <- shmc_render.c, shmc_mcts_tool.c, lemonade_bridge.py
│   └── examples/              <- seed DSL files
├── verify/                    <- 182 formal verification tests (ASAN-clean)
├── layer0/                    <- patch IR + register-based DSP VM
├── layer0b/                   <- structural hashing, patch metadata, tempo map
├── layer1/                    <- voice engine, event scheduling
├── layer2/                    <- motif system (note/rest sequences)
├── layer3/                    <- section scheduler (beat grid)
└── layer4/                    <- song renderer
```

---

## Verification

```bash
./shmc.sh verify         # all 182 tests
./shmc.sh verify --fast  # C tests only (~10 seconds)
```

Covers every component with ASAN-clean C tests and exhaustive Python property checks (18.6M cases for layer0, 553K for layer1).

---

## References

- Lerdahl & Jackendoff — *A Generative Theory of Tonal Music* (1983)
- Tymoczko — *A Geometry of Music* (2011)
- Sethares — *Tuning, Timbre, Spectrum, Scale* (1998)
- Herremans & Chew — *MorpheuS: Automatic Music Generation with Recurrent Pattern Constraints* (2017)
- Piston — *Harmony* (1978)
- Lehman & Stanley — *Abandoning Objectives: Evolution Through the Search for Novelty Alone* (2011)
- Coulom — *Efficient Selectivity and Backup Operators in Monte-Carlo Tree Search* (2006)
- Mouret & Clune — *Illuminating Search Spaces by Mapping Elites* (2015)
