#pragma once
/*
 * SHMC Patch IR — Explicit signal graph (DAG)
 *
 * Sits between DSL parsing and PatchBuilder.
 * Enables optimization passes before code emission:
 *   - dead node elimination
 *   - constant folding
 *   - common subexpression elimination (future)
 *   - structural hash of graph topology (not instruction bytes)
 *
 * Usage:
 *   PatchIR ir;
 *   patch_ir_init(&ir);
 *   int n0 = patch_ir_add_node(&ir, OP_OSC, REG_ONE, 0, 0);
 *   int n1 = patch_ir_add_node(&ir, OP_ADSR, 0, 0, adsr_param);
 *   int n2 = patch_ir_add_node(&ir, OP_MUL, n0, n1, 0);
 *   patch_ir_set_out(&ir, n2);
 *   PatchProgram prog; patch_ir_emit(&ir, &prog);
 *
 * Verification guarantee:
 *   render(patch_ir_emit(ir)) == render(equivalent pb_* sequence)
 *   proved by audio bit-identity test in verify_patch_ir.c
 */
#include "opcodes.h"
#include "patch.h"

#define PIR_MAX_NODES 256

/* Node sources: N_SRC_REG = actual register index (0-3 = reserved),
 *               N_SRC_NODE = reference to another node by node index+4 offset */
#define PIR_SRC_REG(r)   ((int)(r))              /* 0..3 = reserved regs */
#define PIR_SRC_NODE(n)  ((int)(n) + 4)          /* node index + offset */
#define PIR_IS_NODE(s)   ((s) >= 4)
#define PIR_NODE_IDX(s)  ((s) - 4)

typedef struct {
    Opcode  op;
    int     src_a;    /* PIR_SRC_REG or PIR_SRC_NODE */
    int     src_b;    /* PIR_SRC_REG or PIR_SRC_NODE */
    int     param;    /* immediate (cutoff_idx, adsr_idx, fm_depth, etc.) */
    int     param2;   /* second immediate (BPF Q, MIXN weight index) */
    int     out_reg;  /* assigned during emit() — -1 until then */
    int     alive;    /* 1 = reachable from out node, 0 = dead */
} PIRNode;

typedef struct {
    PIRNode nodes[PIR_MAX_NODES];
    int     n_nodes;
    int     out_node;   /* index of the node connected to OP_OUT (-1 = unset) */
} PatchIR;

/* Init / build */
void patch_ir_init(PatchIR *ir);
int  patch_ir_add_node(PatchIR *ir, Opcode op, int src_a, int src_b, int param);
int  patch_ir_add_node2(PatchIR *ir, Opcode op, int src_a, int src_b, int p1, int p2);
void patch_ir_set_out(PatchIR *ir, int node_idx);

/* Optimization passes (each verifiably correct) */
void patch_ir_mark_alive(PatchIR *ir);        /* mark reachable nodes from out */
int  patch_ir_dce(PatchIR *ir);              /* dead code elimination, returns removed count */
void patch_ir_canonicalize(PatchIR *ir);     /* sort commutative operands by topo order */

/* Code emission */
int patch_ir_emit(const PatchIR *ir, PatchProgram *prog);  /* 0=ok, -1=error */

/* Structural hash of graph topology */
uint64_t patch_ir_hash(const PatchIR *ir);

