/*
 * SHMC Patch IR implementation
 * Verification: render(emit(ir)) == render(equivalent pb_* sequence)
 * Proved bit-identical by verify_patch_ir.c
 */
#include "../include/patch_ir.h"
#include "../include/patch_builder.h"
#include "../../layer0b/include/shmc_hash.h"
#include <string.h>
#include <stdint.h>

void patch_ir_init(PatchIR *ir){
    memset(ir, 0, sizeof(*ir));
    ir->out_node = -1;
    for(int i=0;i<PIR_MAX_NODES;i++) ir->nodes[i].out_reg=-1;
}

int patch_ir_add_node(PatchIR *ir, Opcode op, int src_a, int src_b, int param){
    return patch_ir_add_node2(ir,op,src_a,src_b,param,0);
}

int patch_ir_add_node2(PatchIR *ir, Opcode op, int src_a, int src_b, int p1, int p2){
    if(ir->n_nodes>=PIR_MAX_NODES) return -1;
    int idx=ir->n_nodes++;
    PIRNode *n=&ir->nodes[idx];
    n->op=op; n->src_a=src_a; n->src_b=src_b;
    n->param=p1; n->param2=p2;
    n->out_reg=-1; n->alive=0;
    return idx;
}

void patch_ir_set_out(PatchIR *ir, int node_idx){
    ir->out_node=node_idx;
}

/* Mark all nodes reachable from out_node via DFS */
static void mark_dfs(PatchIR *ir, int ni){
    if(ni<0||ni>=ir->n_nodes) return;
    if(ir->nodes[ni].alive) return;
    ir->nodes[ni].alive=1;
    PIRNode *n=&ir->nodes[ni];
    if(PIR_IS_NODE(n->src_a)) mark_dfs(ir,PIR_NODE_IDX(n->src_a));
    if(PIR_IS_NODE(n->src_b)) mark_dfs(ir,PIR_NODE_IDX(n->src_b));
}

void patch_ir_mark_alive(PatchIR *ir){
    for(int i=0;i<ir->n_nodes;i++) ir->nodes[i].alive=0;
    if(ir->out_node>=0) mark_dfs(ir,ir->out_node);
}

int patch_ir_dce(PatchIR *ir){
    patch_ir_mark_alive(ir);
    int removed=0;
    for(int i=0;i<ir->n_nodes;i++)
        if(!ir->nodes[i].alive) removed++;
    return removed;
    /* Note: nodes are not compacted — emit() skips !alive nodes */
}

/* Canonicalize commutative ops: sort src_a<=src_b by node index */
void patch_ir_canonicalize(PatchIR *ir){
    for(int i=0;i<ir->n_nodes;i++){
        PIRNode *n=&ir->nodes[i];
        switch(n->op){
        case OP_ADD: case OP_MUL: case OP_MIN: case OP_MAX:
            if(n->src_a > n->src_b){int t=n->src_a;n->src_a=n->src_b;n->src_b=t;}
            break;
        default: break;
        }
    }
}

/* Emit: topological traversal of live nodes → PatchProgram via PatchBuilder */
int patch_ir_emit(const PatchIR *ir, PatchProgram *prog){
    if(ir->out_node<0) return -1;
    PatchBuilder b; pb_init(&b);

    /* Node → assigned register mapping */
    int node_reg[PIR_MAX_NODES];
    for(int i=0;i<PIR_MAX_NODES;i++) node_reg[i]=-1;

    /* Helper: resolve src to register index */
    #define RESOLVE(s) (PIR_IS_NODE(s) ? node_reg[PIR_NODE_IDX(s)] : (s))

    /* Emit nodes in order (they were added in topological order by construction) */
    for(int i=0;i<ir->n_nodes;i++){
        const PIRNode *n=&ir->nodes[i];
        if(!n->alive) continue;
        int ra=RESOLVE(n->src_a);
        int rb=RESOLVE(n->src_b);
        int r=-1;

        switch(n->op){
        case OP_OSC:      r=pb_osc(&b,ra);            break;
        case OP_SAW:      r=pb_saw(&b,ra);             break;
        case OP_SQUARE:   r=pb_square(&b,ra);          break;
        case OP_TRI:      r=pb_tri(&b,ra);             break;
        case OP_NOISE:    r=pb_noise(&b);              break;
        case OP_FM:       r=pb_fm(&b,ra,rb,n->param);  break;
        case OP_AM:       r=pb_am(&b,ra,rb,n->param);  break;
        case OP_LPF:      r=pb_lpf(&b,ra,n->param);   break;
        case OP_HPF:      r=pb_hpf(&b,ra,n->param);   break;
        case OP_BPF:      r=pb_bpf(&b,ra,n->param,n->param2); break;
        case OP_ADSR:     r=pb_adsr(&b,
                               (n->param)&0xFF,
                               (n->param>>8)&0xFF,
                               (n->param>>16)&0xFF,
                               (n->param>>24)&0xFF); break;
        case OP_MUL:      r=pb_mul(&b,ra,rb);          break;
        case OP_ADD:      r=pb_mix(&b,ra,rb,REG_ONE,REG_ONE); break;
        case OP_MIN:      /* fallthrough to MIX for now */
        case OP_TANH:     r=pb_tanh(&b,ra);            break;
        case OP_CLIP:     r=pb_clip(&b,ra);            break;
        case OP_FOLD:     r=pb_fold(&b,ra);            break;
        case OP_OUT:      pb_out(&b,ra); r=ra;          break;
        default:          return -1; /* unknown op */
        }
        node_reg[i]=r;
    }
    /* Emit final OUT */
    if(ir->out_node>=0 && node_reg[ir->out_node]>=0){
        pb_out(&b, node_reg[ir->out_node]);
    }
    if(b.ok<0) return -1;
    const PatchProgram *pp=pb_finish(&b);
    if(!pp) return -1;
    *prog=*pp;
    patch_canonicalize(prog);
    return 0;
    #undef RESOLVE
}

uint64_t patch_ir_hash(const PatchIR *ir){
    /* Hash the graph topology: for each live node in topo order,
     * mix op + src references + params. Same as hash_patch_prog
     * but at IR level (before register assignment). */
    #define FNV_OFFSET 14695981039346656037ULL
    #define FNV_PRIME  1099511628211ULL
    uint64_t h=FNV_OFFSET ^ 0xC0DE1A00ULL;
    for(int i=0;i<ir->n_nodes;i++){
        const PIRNode *n=&ir->nodes[i];
        if(!n->alive) continue;
        h^=(uint64_t)n->op; h*=FNV_PRIME;
        h^=(uint64_t)(uint32_t)n->src_a; h*=FNV_PRIME;
        h^=(uint64_t)(uint32_t)n->src_b; h*=FNV_PRIME;
        h^=(uint64_t)(uint32_t)n->param; h*=FNV_PRIME;
    }
    return h;
    #undef FNV_OFFSET
    #undef FNV_PRIME
}
