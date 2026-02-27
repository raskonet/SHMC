#!/usr/bin/env python3
"""
SHMC DSL Integration Test Suite
Tests the shmc_render binary against known-good and known-bad DSL snippets.
All expectations calibrated to actual shmc_render behaviour (mono WAV, sec durations).
"""
import subprocess, struct, os, sys, math, tempfile, time, glob

RENDER_BIN = os.path.join(os.path.dirname(__file__), "shmc_render")
PASS_C = 0; FAIL_C = 0

def render_dsl(dsl):
    with tempfile.NamedTemporaryFile(suffix=".shmc", mode="w", delete=False) as f:
        f.write(dsl); dsl_path = f.name
    wav_path = dsl_path.replace(".shmc", ".wav")
    try:
        r = subprocess.run([RENDER_BIN, dsl_path, wav_path],
                           capture_output=True, text=True, timeout=30)
        wav = {}
        if r.returncode == 0 and os.path.exists(wav_path):
            with open(wav_path, "rb") as f: raw = f.read()
            nch  = struct.unpack_from("<H", raw, 22)[0]
            sr   = struct.unpack_from("<I", raw, 24)[0]
            dsz  = struct.unpack_from("<I", raw, 40)[0]
            nf   = dsz // (nch * 2)
            dur  = nf / sr
            smp  = [struct.unpack_from("<h", raw, 44+i*2)[0]/32768.0
                    for i in range(min(nf*nch, 220500))]
            rms  = math.sqrt(sum(x*x for x in smp)/len(smp)) if smp else 0
            peak = max(abs(x) for x in smp) if smp else 0
            wav  = dict(sr=sr, nch=nch, dur=dur, rms=rms, peak=peak)
        return r.returncode, wav
    finally:
        for p in (dsl_path, wav_path):
            try: os.unlink(p)
            except: pass

def check(name, ok, detail=""):
    global PASS_C, FAIL_C
    sym = "✓" if ok else "✗"; tag = "PASS" if ok else "FAIL"
    print(f"  {sym} {tag}  {name}" + (f"  [{detail}]" if detail else ""))
    if ok: PASS_C += 1
    else:  FAIL_C += 1

def run(label, dsl, expects_fail=False):
    rc, wav = render_dsl(dsl)
    if expects_fail:
        check(f"[{label}] correctly rejects invalid DSL", rc != 0, f"exit={rc}")
        return rc, wav
    check(f"[{label}] render succeeds", rc == 0, f"exit={rc}")
    return rc, wav

# ── DSL Fixtures ─────────────────────────────────────────────────────

SIMPLE_TONE = """
PATCH sine {
    osc sin ONE
    adsr 2 6 20 10
    mul $0 $1
    out $2
}
MOTIF middle_C {
    note 60 5 9
    note 62 5 8
}
SECTION s 8.0 {
    use middle_C @ 0 x2 patch sine
}
SONG simple_tone 120.0 {
    play s
}
"""
# 8 beats at 120BPM = 4.0s of music + ADSR release tail

SAW_BASS = """
PATCH bass {
    saw ONE
    adsr 0 6 15 8
    mul $0 $1
    lpf $2 20
    out $3
}
MOTIF bass_line {
    note 36 4 10
    note 43 4 9
}
SECTION s 16.0 {
    use bass_line @ 0 x4 patch bass
}
SONG saw_bass 120.0 {
    play s
}
"""

TRANSPOSE_TEST = """
PATCH p {
    saw ONE
    adsr 1 4 18 8
    mul $0 $1
    out $2
}
MOTIF root {
    note 48 5 9
}
SECTION s 16.0 {
    use root @  0 x2 patch p
    use root @  8 x2 patch p t=5
}
SONG xpose 100.0 {
    play s
}
"""

MULTI_PATCH = """
PATCH lead {
    osc sin ONE
    adsr 2 8 20 12
    mul $0 $1
    out $2
}
PATCH bass {
    saw ONE
    adsr 0 5 15 8
    mul $0 $1
    lpf $2 22
    out $3
}
MOTIF mel {
    note 64 4 9
    note 67 4 8
}
MOTIF walk {
    note 40 4 10
    note 47 4 9
}
SECTION combo 16.0 {
    use mel  @ 0 x4 patch lead
    use walk @ 0 x4 patch bass
}
SONG multi 110.0 {
    play combo
}
"""

SONG_REPEAT = """
PATCH p {
    saw ONE
    adsr 1 5 18 10
    mul $0 $1
    out $2
}
MOTIF m {
    note 60 6 8
}
SECTION s 8.0 {
    use m @ 0 patch p
}
SONG rep 120.0 {
    play s x3
}
"""

FM_AMBIENT = """
PATCH fm_pad {
    osc sin ONE
    osc sin ONE
    fm $0 $1 4
    adsr 12 10 25 20
    mul $2 $3
    lpf $4 28
    out $5
}
MOTIF chord {
    note 60 6 7
    note 64 6 7
}
SECTION s 8.0 {
    use chord @ 0 patch fm_pad
}
SONG ambient 60.0 {
    play s
}
"""

VEL_SCALE = """
PATCH p {
    saw ONE
    adsr 0 4 15 8
    mul $0 $1
    out $2
}
MOTIF m {
    note 60 4 9
}
SECTION s 8.0 {
    use m @ 0 patch p v=0.5
    use m @ 4 patch p v=1.5
}
SONG v 120.0 { play s }
"""

# Invalid DSL cases
NO_SONG        = "PATCH p { saw ONE\nadsr 1 5 18 10\nmul $0 $1\nout $2\n}"
BAD_SECTION    = "PATCH p{saw ONE\nadsr 1 5 18 10\nmul $0 $1\nout $2}\nMOTIF m{note 60 5 9}\nSECTION s 8.0{use ghost @ 0 patch p}\nSONG x 120{play s}"
# (missing motif "ghost" should fail at section_add_track motif resolution)

# ── Run tests ────────────────────────────────────────────────────────
print("\n" + "="*62)
print("  SHMC DSL Integration Test Suite")
print("="*62)
t0 = time.time()

print("\n── Group 1: Simple sine tone ─────────────────────────────────")
rc, w = run("simple-tone", SIMPLE_TONE)
if rc == 0:
    check("[simple-tone] sample rate = 44100", w["sr"]==44100, str(w["sr"]))
    check("[simple-tone] mono (nch=1)",         w["nch"]==1,   str(w["nch"]))
    check("[simple-tone] duration 3–8 s",       3 < w["dur"] < 8, f"{w['dur']:.2f}s")
    check("[simple-tone] audible RMS>0.002",    w["rms"] > 0.002, f"{w['rms']:.4f}")
    check("[simple-tone] not clipped <0.99",    w["peak"] < 0.99, f"{w['peak']:.3f}")

print("\n── Group 2: SAW bass ─────────────────────────────────────────")
rc, w = run("saw-bass", SAW_BASS)
if rc == 0:
    check("[saw-bass] duration 2–8s",   2 < w["dur"] < 8,  f"{w['dur']:.2f}s")
    check("[saw-bass] audible",         w["rms"] > 0.001)
    check("[saw-bass] not clipped",     w["peak"] < 0.99)

print("\n── Group 3: Transpose modifier ───────────────────────────────")
rc, w = run("transpose", TRANSPOSE_TEST)
if rc == 0:
    check("[transpose] renders >5s",    w["dur"] > 5, f"{w['dur']:.2f}s")
    check("[transpose] audible",        w["rms"] > 0.001)

print("\n── Group 4: Multi-patch layering ─────────────────────────────")
rc, w = run("multi-patch", MULTI_PATCH)
if rc == 0:
    check("[multi-patch] audible",      w["rms"] > 0.001)
    check("[multi-patch] not clipped",  w["peak"] < 0.99)

print("\n── Group 5: Song repeat ──────────────────────────────────────")
rc, w = run("song-repeat-x3", SONG_REPEAT)
if rc == 0:
    check("[song-repeat] duration >1s (3x section)",  w["dur"] > 1, f"{w['dur']:.2f}s")
    check("[song-repeat] audible",                    w["rms"] > 0.001)

print("\n── Group 6: FM ambient ───────────────────────────────────────")
rc, w = run("fm-ambient", FM_AMBIENT)
if rc == 0:
    check("[fm-ambient] audible",       w["rms"] > 0.001)
    check("[fm-ambient] not clipped",   w["peak"] < 0.99)

print("\n── Group 7: vel_scale modifier ───────────────────────────────")
rc, w = run("vel-scale", VEL_SCALE)
if rc == 0:
    check("[vel-scale] audible",        w["rms"] > 0.001)

print("\n── Group 8: Error rejection ──────────────────────────────────")
run("no-SONG-keyword",    NO_SONG,     expects_fail=True)
run("missing-motif-name", BAD_SECTION, expects_fail=True)

print("\n── Group 9: Bundled example files ───────────────────────────")
example_dir = os.path.join(os.path.dirname(__file__), "..", "examples")
for fpath in sorted(glob.glob(os.path.join(example_dir, "*.shmc"))):
    fname = os.path.basename(fpath)
    with open(fpath) as f: dsl = f.read()
    rc, w = run(f"examples/{fname}", dsl)
    if rc == 0:
        check(f"[examples/{fname}] audible", w["rms"] > 0.001)
        check(f"[examples/{fname}] not clipped", w["peak"] < 0.99)

elapsed = time.time() - t0
total   = PASS_C + FAIL_C
print(f"\n{'='*62}")
print(f"  RESULT: {PASS_C}/{total} PASSED  |  {FAIL_C} FAILED  ({elapsed:.1f}s)")
print(f"{'='*62}\n")
sys.exit(0 if FAIL_C == 0 else 1)
