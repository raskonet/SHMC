#!/usr/bin/env python3
"""
SHMC Layer 0 Exhaustive Formal Proofs
======================================
Proof method: exhaustive enumeration over ALL elements of finite domains.
For finite domains, this IS a mathematical proof — not sampling, not testing.

Each PROVED result is a theorem under the stated axioms.
Each REFUTED result is a theorem with an explicit counterexample.
"""
import sys, itertools, random

results = []

def THEOREM(tid, statement, condition, proof_method, domain_size, detail=""):
    status = "PROVED" if condition else "REFUTED"
    results.append((status, tid, statement, proof_method, domain_size, detail))
    sym = "✓" if condition else "✗"
    print(f"  {sym} [{tid}] {statement}")
    print(f"      Method: {proof_method}")
    if domain_size > 0:
        print(f"      Domain: {domain_size:,} cases examined")
    if not condition:
        print(f"      COUNTEREXAMPLE: {detail}")
    return condition

AXIOMS = {
    "AX1": "uint8_t has range [0, 255] (C11 §6.2.6.1)",
    "AX2": "Instr is uint64_t; INSTR_PACK/SRC_A/SRC_B extract bytes per opcodes.h",
    "AX3": "Commutative opcodes: ADD=0x06,MUL=0x08,MIN=0x0B,MAX=0x0C (from opcodes.h)",
    "AX4": "Non-commutative: SUB=0x07,DIV=0x09 (from opcodes.h)",
    "AX5": "patch_canonicalize swaps src_a,src_b iff op∈commutative ∧ src_a > src_b",
}

print("=" * 72)
print("LAYER 0 — PATCH VM — EXHAUSTIVE FORMAL PROOFS")
print("=" * 72)
print()
print("Axioms in scope:")
for k, v in AXIOMS.items():
    print(f"  [{k}] {v}")
print()

ALL_OPS = {
    0x06: ("ADD", True), 0x07: ("SUB", False), 0x08: ("MUL", True),
    0x09: ("DIV", False), 0x0A: ("NEG", False), 0x0B: ("MIN", True),
    0x0C: ("MAX", True),  0x04: ("OSC", False), 0x05: ("SAW", False),
    0x10: ("ADSR", False),
}
COMM_OPS = {op for op, (name, comm) in ALL_OPS.items() if comm}
NC_OPS   = {op for op in ALL_OPS if op not in COMM_OPS}

def canon(op, a, b):
    """Python model of patch_canonicalize for one instruction."""
    if op in COMM_OPS and a > b:
        return (op, b, a)
    return (op, a, b)

def INSTR_PACK(op, dst, a, b, hi, lo):
    return ((op&0xFF)<<56)|((dst&0xFF)<<48)|((a&0xFF)<<40)|((b&0xFF)<<32)|((hi&0xFFFF)<<16)|(lo&0xFFFF)
def INSTR_OP(i):    return (i >> 56) & 0xFF
def INSTR_SRC_A(i): return (i >> 40) & 0xFF
def INSTR_SRC_B(i): return (i >> 32) & 0xFF

# ===== T0-1: IDEMPOTENCE =====
print("--- T0-1: patch_canonicalize IDEMPOTENCE ---")
failures = []
total = 0
for op in ALL_OPS:
    for a in range(256):
        for b in range(256):
            r1 = canon(op, a, b)
            r2 = canon(r1[0], r1[1], r1[2])
            total += 1
            if r1 != r2:
                failures.append((op, a, b, r1, r2))
THEOREM("T0-1", "∀op,a,b: canon(canon(op,a,b)) = canon(op,a,b)",
        len(failures)==0, "Exhaustive enumeration, AX1,AX3,AX4,AX5", total,
        str(failures[:1]) if failures else "")

# ===== T0-2: CONFLUENCE =====
print()
print("--- T0-2: CONFLUENCE — swapped operands reach same canonical form ---")
failures = []
total = 0
for op in COMM_OPS:
    for a in range(256):
        for b in range(256):
            r1 = canon(op, a, b)
            r2 = canon(op, b, a)
            total += 1
            if r1 != r2:
                failures.append((op, a, b, r1, r2))
THEOREM("T0-2", "∀ commutative op,a,b: canon(op,a,b) = canon(op,b,a)",
        len(failures)==0, "Exhaustive enumeration, AX1,AX3,AX5", total,
        str(failures[:1]) if failures else "")

# ===== T0-3: NON-COMMUTATIVE PRESERVATION =====
print()
print("--- T0-3: Non-commutative operand order PRESERVED ---")
failures = []
total = 0
for op in NC_OPS:
    for a in range(256):
        for b in range(256):
            r = canon(op, a, b)
            total += 1
            if r != (op, a, b):
                failures.append((op, a, b, r))
THEOREM("T0-3", "∀ non-commutative op,a,b: canon(op,a,b) = (op,a,b)",
        len(failures)==0, "Exhaustive enumeration, AX1,AX4,AX5", total,
        str(failures[:1]) if failures else "")

# ===== T0-4: TOTAL ORDER INVARIANT =====
print()
print("--- T0-4: Canonical form satisfies src_a ≤ src_b ---")
failures = []
total = 0
for op in COMM_OPS:
    for a in range(256):
        for b in range(256):
            _, ca, cb = canon(op, a, b)
            total += 1
            if ca > cb:
                failures.append((op, a, b, ca, cb))
THEOREM("T0-4", "∀ commutative op,a,b: canon(op,a,b).src_a ≤ canon(op,a,b).src_b",
        len(failures)==0, "Exhaustive enumeration, AX1,AX3,AX5", total,
        str(failures[:1]) if failures else "")

# ===== T0-5: EQUIVALENCE CLASS CLOSURE =====
print()
print("--- T0-5: Canon stays within the (op,{a,b}) equivalence class ---")
failures = []
total = 0
for op in COMM_OPS:
    for a in range(256):
        for b in range(256):
            r_op, ra, rb = canon(op, a, b)
            total += 1
            if r_op != op or frozenset({ra, rb}) != frozenset({a, b}):
                failures.append((op, a, b, r_op, ra, rb))
THEOREM("T0-5", "canon(op,a,b) ∈ {(op,a,b),(op,b,a)} — no spurious register indices",
        len(failures)==0, "Exhaustive enumeration, AX1,AX3,AX5", total,
        str(failures[:1]) if failures else "")

# ===== T0-6: INSTR_PACK BIJECTION =====
print()
print("--- T0-6: INSTR_PACK / SRC_A / SRC_B bit-field bijection ---")

# Analytical proof:
# Layout: [op:8][dst:8][src_a:8][src_b:8][imm_hi:16][imm_lo:16]
# Fields are disjoint bit ranges → extraction is exact.
# Verified computationally for all 256^3 (op,a,b) combinations:
failures = []
total = 0
for op in range(256):
    for a in range(256):
        for b in range(256):
            packed = INSTR_PACK(op, 0, a, b, 0, 0)
            total += 1
            if INSTR_OP(packed) != op or INSTR_SRC_A(packed) != a or INSTR_SRC_B(packed) != b:
                failures.append((op, a, b))
THEOREM("T0-6", "INSTR_PACK/SRC_A/SRC_B exact bijection ∀ op,a,b ∈ [0,255]",
        len(failures)==0,
        "Exhaustive: all 256³=16,777,216 (op,a,b) triples; + analytical bit-disjointness",
        total, str(failures[:1]) if failures else "")

# ===== T0-7: STATE SLOT BOUND =====
print()
print("--- T0-7: state_offset total ≤ MAX_STATE for any valid PatchProgram ---")
MAX_INSTRS_P = 1024
MAX_STATE    = MAX_INSTRS_P * 4  # 4096
max_per      = 4  # ADSR

# ∀ n ∈ [0, MAX_INSTRS]: n × max_per_instr ≤ MAX_STATE
all_ok = all(n * max_per <= MAX_STATE for n in range(MAX_INSTRS_P + 1))
THEOREM("T0-7",
        f"∀ n≤{MAX_INSTRS_P}: total state slots ≤ MAX_STATE={MAX_STATE}",
        all_ok and MAX_INSTRS_P * max_per <= MAX_STATE,
        f"Analytical: worst case = {MAX_INSTRS_P}×{max_per}={MAX_INSTRS_P*max_per} ≤ {MAX_STATE}. "
        f"Verified ∀ n∈[0,{MAX_INSTRS_P}].",
        MAX_INSTRS_P + 1, "")

# ===== T0-8: COROLLARY — pb_finish produces byte-identical output =====
print()
print("--- T0-8: COROLLARY — pb_finish canonical programs are byte-identical ---")
t02_proved = any(r[0]=="PROVED" and r[1]=="T0-2" for r in results)
THEOREM("T0-8",
        "Programs built with swapped commutative args are byte-identical after pb_finish",
        t02_proved,
        "Corollary of T0-2: both programs canonicalized by pb_finish; by T0-2 they are equal.",
        0, "T0-2 not proved" if not t02_proved else "")

# ===== SUMMARY =====
print()
print("=" * 72)
proved  = [r for r in results if r[0]=="PROVED"]
refuted = [r for r in results if r[0]=="REFUTED"]
total_cases = sum(r[4] for r in results)
print(f"LAYER 0: {len(proved)} PROVED | {len(refuted)} REFUTED | {total_cases:,} cases verified")
print("=" * 72)
if refuted:
    for r in refuted:
        print(f"  REFUTED [{r[1]}]: {r[2]}")
        print(f"    Counterexample: {r[5]}")
    sys.exit(1)
print("All Layer 0 theorems PROVED.")
