
# ══════════════════════════════════════════════════════════════════════
# SHMC Makefile  —  build everything with: make
# ══════════════════════════════════════════════════════════════════════
CC      := gcc
CFLAGS  := -O2 -Wall -Wno-unused-parameter
LFLAGS  := -lm
BINDIR  := bin
VDIR    := bin/verify

INCS := \
  -Ilemonade/include -Ilayer0/include -Ilayer0b/include \
  -Ilayer1/include   -Ilayer2/include -Ilayer3/include  \
  -Ilayer4/include

CORE := \
  lemonade/src/shmc_dsl.c lemonade/src/shmc_mutate.c \
  lemonade/src/shmc_search.c lemonade/src/shmc_dsl_emit.c \
  lemonade/src/shmc_canon.c lemonade/src/shmc_patch_mutate.c \
  lemonade/src/shmc_mut_algebra.c \
  layer0/src/patch_interp.c layer0/src/tables.c layer0/src/patch_ir.c \
  layer0b/src/shmc_hash.c layer0b/src/patch_meta.c layer0b/src/tempo_map.c \
  layer1/src/voice.c layer2/src/motif.c layer2/src/motif_mutate.c \
  layer3/src/section.c layer4/src/song.c

FITNESS := \
  lemonade/src/shmc_evo_fitness.c \
  lemonade/src/shmc_harmony.c \
  lemonade/src/shmc_map_elites.c \
  lemonade/src/shmc_mcts.c

ALL := $(CORE) $(FITNESS)

.PHONY: all verifiers verify verify_fast verify_py clean help

all: dirs $(BINDIR)/shmc_render $(BINDIR)/shmc_evolve verifiers
	@echo ""
	@echo "  All built. Run:  ./shmc.sh help"

dirs:
	@mkdir -p $(BINDIR) $(VDIR)

$(BINDIR)/shmc_render: lemonade/tools/shmc_render.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
	@echo "  built: $@"

$(BINDIR)/shmc_evolve: lemonade/tools/shmc_mcts_tool.c $(ALL)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
	@cp $@ $(BINDIR)/shmc_mcts_tool
	@echo "  built: $@"

# ── verifier binaries ─────────────────────────────────────────────────
verifiers: dirs \
  $(VDIR)/verify_search_v2 $(VDIR)/verify_roundtrip \
  $(VDIR)/verify_mutate $(VDIR)/verify_mut_algebra \
  $(VDIR)/verify_structural_mutations $(VDIR)/verify_canon_struct \
  $(VDIR)/verify_novelty $(VDIR)/verify_evo_fitness \
  $(VDIR)/verify_mcts $(VDIR)/verify_harmony
	@echo "  verifiers ready"

$(VDIR)/verify_search_v2: verify/verify_search_v2.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_roundtrip: verify/verify_roundtrip.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_mutate: verify/verify_mutate.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_mut_algebra: verify/verify_mut_algebra.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_structural_mutations: verify/verify_structural_mutations.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_canon_struct: verify/verify_canon_struct.c $(CORE)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_novelty: verify/verify_novelty.c
	$(CC) $(CFLAGS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_evo_fitness: verify/verify_evo_fitness.c lemonade/src/shmc_evo_fitness.c
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_mcts: verify/verify_mcts.c $(ALL)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@
$(VDIR)/verify_harmony: verify/verify_harmony.c $(ALL)
	$(CC) $(CFLAGS) $(INCS) $^ $(LFLAGS) -o $@

verify verify_fast: verifiers
	@./scripts/verify.sh --fast

verify_py:
	@./scripts/verify.sh --python

clean:
	rm -rf $(BINDIR)
	@echo "  cleaned."

help:
	@echo ""
	@echo "  make              Build all"
	@echo "  make verify       Run all verification suites"
	@echo "  make clean        Remove build artefacts"
	@echo ""
	@echo "  ./shmc.sh compose  --prompt 'upbeat jazz at 120 BPM'"
	@echo "  ./shmc.sh evolve   lemonade/examples/blues_a.shmc"
	@echo "  ./shmc.sh render   myfile.shmc  out.wav"
	@echo "  ./shmc.sh verify"


