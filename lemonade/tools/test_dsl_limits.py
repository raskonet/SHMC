#!/usr/bin/env python3
"""
Formal verification: DSL input validation limits.
Each test proves a specific LLM pathology is rejected before it reaches the renderer.
"""
import subprocess, os, sys, tempfile, time

RENDER_BIN = os.path.join(os.path.dirname(__file__), "shmc_render")
PASS_C=0; FAIL_C=0

def run(label, dsl, expect_fail, detail=""):
    global PASS_C, FAIL_C
    with tempfile.NamedTemporaryFile(suffix=".shmc",mode="w",delete=False) as f:
        f.write(dsl); path=f.name
    wav=path.replace(".shmc",".wav")
    try:
        r=subprocess.run([RENDER_BIN,path,wav],capture_output=True,text=True,timeout=15)
        ok=(r.returncode!=0) if expect_fail else (r.returncode==0)
        sym="✓" if ok else "✗"; tag="PASS" if ok else "FAIL"
        print(f"  {sym} {tag}  [{label}]  {'rejected (expected)' if expect_fail else 'accepted'}")
        if not ok:
            print(f"        stderr: {r.stderr[:120].strip()}")
        if ok: PASS_C+=1
        else:  FAIL_C+=1
    finally:
        for p in (path,wav):
            try: os.unlink(p)
            except: pass

def make_patch(note=60): return f"""PATCH p {{
    saw ONE
    adsr 1 5 18 10
    mul $0 $1
    out $2
}}
MOTIF m {{
    note {note} 4 9
}}
SECTION s 8.0 {{
    use m @ 0 patch p
}}
SONG x 120.0 {{
    play s
}}"""

print("\n" + "="*60)
print("  SHMC DSL Limits Verification Suite")
print("="*60)

print("\n── BPM limits ─────────────────────────────────────────────")
run("BPM=19 rejected (below 20)",  make_patch().replace("120.0","19.0"),  True)
run("BPM=20 accepted",             make_patch().replace("120.0","20.0"),  False)
run("BPM=300 accepted",            make_patch().replace("120.0","300.0"), False)
run("BPM=301 rejected (above 300)",make_patch().replace("120.0","301.0"), True)

print("\n── MIDI pitch limits ───────────────────────────────────────")
run("MIDI=20 rejected (below A0=21)",  make_patch(note=20),  True)
run("MIDI=21 accepted (A0)",           make_patch(note=21),  False)
run("MIDI=108 accepted (C8)",          make_patch(note=108), False)
run("MIDI=109 rejected (above C8)",    make_patch(note=109), True)

print("\n── Section length limits ───────────────────────────────────")
long_sec = """PATCH p {
    saw ONE
    adsr 1 5 18 10
    mul $0 $1
    out $2
}
MOTIF m { note 60 4 9 }
SECTION s 257.0 {
    use m @ 0 patch p
}
SONG x 120.0 { play s }"""
ok_sec = long_sec.replace("257.0","256.0")
run("Section 257 beats rejected",  long_sec, True)
run("Section 256 beats accepted",  ok_sec,   False)

print("\n── Repeat limits ───────────────────────────────────────────")
big_rep = """PATCH p { saw ONE; adsr 1 5 18 10; mul $0 $1; out $2 }
MOTIF m { note 60 4 9 }
SECTION s 8.0 { use m @ 0 patch p x65 }
SONG x 120.0 { play s }"""
ok_rep = big_rep.replace("x65","x64")
run("Repeat x65 rejected", big_rep, True)
run("Repeat x64 accepted", ok_rep,  False)

print("\n── Transpose limits ────────────────────────────────────────")
def make_transpose(t):
    return f"""PATCH p {{ saw ONE; adsr 1 5 18 10; mul $0 $1; out $2 }}
MOTIF m {{ note 60 4 9 }}
SECTION s 8.0 {{ use m @ 0 patch p t={t} }}
SONG x 120.0 {{ play s }}"""
run("Transpose -25 rejected",  make_transpose(-25), True)
run("Transpose -24 accepted",  make_transpose(-24), False)
run("Transpose +24 accepted",  make_transpose(24),  False)
run("Transpose +25 rejected",  make_transpose(25),  True)

print("\n── Velocity scale limits ───────────────────────────────────")
def make_vscale(v):
    return f"""PATCH p {{ saw ONE; adsr 1 5 18 10; mul $0 $1; out $2 }}
MOTIF m {{ note 60 4 9 }}
SECTION s 8.0 {{ use m @ 0 patch p v={v} }}
SONG x 120.0 {{ play s }}"""
run("vel_scale -0.1 rejected",  make_vscale(-0.1), True)
run("vel_scale 0.0 accepted",   make_vscale(0.0),  False)
run("vel_scale 4.0 accepted",   make_vscale(4.0),  False)
run("vel_scale 4.1 rejected",   make_vscale(4.1),  True)

print("\n── Existing tests still pass ───────────────────────────────")
run("Normal blues (should still work)", """PATCH bass {
    saw ONE
    adsr 0 6 15 8
    mul $0 $1
    lpf $2 20
    out $3
}
MOTIF walk {
    note 45 4 9
    note 52 4 8
}
SECTION s 16.0 {
    use walk @ 0 x4 patch bass
}
SONG blues 100.0 {
    play s x2
}""", False)

print(f"\n{'='*60}")
print(f"  RESULT: {PASS_C}/{PASS_C+FAIL_C} PASSED  |  {FAIL_C} FAILED")
print(f"{'='*60}\n")
sys.exit(0 if FAIL_C==0 else 1)
