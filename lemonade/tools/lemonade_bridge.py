#!/usr/bin/env python3
"""
SHMC ↔ Lemonade Bridge  (v2)
Usage:
    # Render only:
    python3 lemonade/tools/lemonade_bridge.py --prompt "jazz loop" --out /tmp/jazz.wav --play

    # Full E2E pipeline (LLM -> DSL -> MCTS evolution -> WAV):
    python3 lemonade/tools/lemonade_bridge.py --prompt "jazz loop" --evolve --play

Defaults to SeedCoder-8B on port 8002. Override with --model / --port / --host.
"""
import http.client, json, subprocess, argparse, sys, os, textwrap, time, re, platform

LEMON_HOST = "localhost"
LEMON_PORT = 8002
LEMON_PATH = "/v1/chat/completions"
API_KEY    = "lemonade"

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RENDERER    = os.path.join(_SCRIPT_DIR, "shmc_render")
EVOLVER     = os.path.join(_SCRIPT_DIR, "evolve")

# ── System prompt ──────────────────────────────────────────────────────
SYSTEM_PROMPT = textwrap.dedent("""
You are SHMC Composer. Output ONLY valid SHMC DSL. No prose, no markdown, no code fences.

RULES:
1. $N is the line output (0-indexed). First op=$0, second=$1, third=$2 etc.
   out $N MUST reference the register of the LAST operation. COUNT CAREFULLY.
2. MIDI pitch MUST be 21-108. Bass=35-52, Lead=60-80, Drums=36 (NEVER 0).
3. Durations: 3=eighth(0.5 beat)  4=quarter(1 beat)  5=half(2 beats)  6=whole(4 beats)
   Velocities: 0-15  (6=soft  8=med  10=loud  12=very-loud)
4. No commas, no brackets in SECTION/SONG lines.
5. ALWAYS use x<N> repeats on use lines so motifs fill the section — no silence gaps.
6. File order: PATCHes → MOTIFs → SECTIONs → SONG

VALID PATCH OPS (ONLY these — nothing else):
  osc [sin|saw|sqr|tri|noise] ONE   — oscillator  (sin default)
  saw ONE / tri ONE / noise          — shorthand oscillators
  fm $A $B <depth>                   — FM synthesis
  lpf $A <cutoff 0-63>               — low-pass filter
  hpf $A <cutoff 0-63>               — high-pass filter
  bpf $A <cutoff 0-63> <q 0-63>     — band-pass filter
  adsr <a> <d> <s> <r>               — envelope (0-31 each)
  mul $A $B                          — multiply (apply envelope)
  add $A $B                          — mix two signals
  tanh $A                            — soft saturation
  clip $A                            — hard clip
  fold $A                            — wavefold (harmonic distortion)
  out $A                             — output (required, last line)

VALID MOTIF OPS:
  note <midi> <dur> <vel>
  rest <dur>                         — silence gap

VALID SECTION OPS:
  use <motif> @ <beat> [x<repeats>] patch <patch> [t=<semitones>] [v=<vel_scale>]

VALID SONG OPS:
  play <section> [x<repeats>]

ADSR reference (times): 0=1ms 4=5ms 8=30ms 12=100ms 16=300ms 20=1s 24=2s
LPF/HPF cutoff:  10=50Hz 15=100Hz 20=180Hz 25=280Hz 30=400Hz 38=700Hz 45=1.2kHz 55=3kHz

DRUM TECHNIQUE — noise oscillators, different cutoffs:
  Kick:  noise + adsr 0 4 0 3  + lpf $x 12  (deep thump)
  Snare: noise + adsr 0 5 0 4  + lpf $x 28  (mid crack)
  Hihat: noise + adsr 0 2 0 1  + lpf $x 52  (bright tick)
  Schedule single-note motifs at exact beat positions:
    use kick_hit @ 0 patch kick   use kick_hit @ 2 patch kick  ...

HARMONY TIPS (important for good music):
  - Pick ONE key. Keep all notes in that scale. E.g. A minor: A B C D E F G (MIDI mod 12 in {9,11,0,2,4,5,7})
  - Voice leading: move notes by small steps (1-3 semitones) between motifs
  - Bass plays root notes. Lead plays melody in same key. Pad fills harmony.
  - For tonal variety use t= transpose on use lines (e.g. t=5 = perfect fourth up)

EXAMPLE (A minor, 90 BPM):

PATCH fm_bass {
    osc sin ONE      # $0 — carrier
    osc sin ONE      # $1 — modulator
    fm $0 $1 5       # $2 — FM
    adsr 1 8 20 10   # $3 — envelope
    mul $2 $3        # $4
    lpf $4 22        # $5
    out $5
}
PATCH lead {
    osc tri ONE      # $0
    adsr 3 8 18 12   # $1
    mul $0 $1        # $2
    lpf $2 42        # $3
    out $3
}
PATCH kick {
    osc noise ONE    # $0
    adsr 0 4 0 3     # $1
    mul $0 $1        # $2
    lpf $2 12        # $3
    out $3
}
PATCH snare {
    osc noise ONE    # $0
    adsr 0 5 0 4     # $1
    mul $0 $1        # $2
    lpf $2 28        # $3
    out $3
}
PATCH hihat {
    osc noise ONE    # $0
    adsr 0 2 0 1     # $1
    mul $0 $1        # $2
    lpf $2 52        # $3
    out $3
}
MOTIF bass_am {
    note 45 4 10
    note 45 4 9
    note 48 4 10
    note 47 4 9
}
MOTIF lead_phrase {
    note 69 3 10
    rest 3
    note 71 3 9
    note 72 4 10
    note 71 5 8
}
MOTIF kick_hit { note 36 4 12 }
MOTIF snare_hit { note 36 4 10 }
MOTIF hat_pair { note 36 3 6   note 36 3 5 }
SECTION verse 16.0 {
    use bass_am   @ 0  x4  patch fm_bass
    use lead_phrase @ 0 x2 patch lead v=0.9
    use lead_phrase @ 8 x2 patch lead t=5 v=0.85
    use kick_hit  @ 0  patch kick
    use kick_hit  @ 2  patch kick
    use kick_hit  @ 4  patch kick
    use kick_hit  @ 6  patch kick
    use kick_hit  @ 8  patch kick
    use kick_hit  @ 10 patch kick
    use kick_hit  @ 12 patch kick
    use kick_hit  @ 14 patch kick
    use snare_hit @ 1  patch snare
    use snare_hit @ 3  patch snare
    use snare_hit @ 5  patch snare
    use snare_hit @ 7  patch snare
    use snare_hit @ 9  patch snare
    use snare_hit @ 11 patch snare
    use snare_hit @ 13 patch snare
    use snare_hit @ 15 patch snare
    use hat_pair  @ 0  x8  patch hihat v=0.6
}
SONG amin_groove 90.0 {
    play verse x4
}
""").strip()


def call_lemonade(prompt: str, model: str, temperature: float,
                  no_think: bool = True) -> str:
    user_content = (prompt + " /no_think") if no_think else prompt
    payload = json.dumps({
        "model":       model,
        "temperature": temperature,
        "max_tokens":  2048,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user",   "content": user_content}
        ]
    }).encode()

    conn = http.client.HTTPConnection(LEMON_HOST, LEMON_PORT, timeout=300)
    try:
        conn.request("POST", LEMON_PATH,
                     body=payload,
                     headers={"Content-Type": "application/json",
                              "Authorization": f"Bearer {API_KEY}"})
        resp = conn.getresponse()
        raw  = resp.read().decode()
    finally:
        conn.close()

    if resp.status != 200:
        raise RuntimeError(f"Lemonade returned HTTP {resp.status}: {raw[:400]}")

    data = json.loads(raw)
    return data["choices"][0]["message"]["content"]


def sanitise(raw: str) -> str:
    """Strip think-blocks, markdown fences, balance braces."""
    text = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = [l for l in text.splitlines() if not l.strip().startswith("```")]
    text = "\n".join(lines).strip()
    # Close any unclosed braces (truncated LLM output)
    open_b  = text.count("{")
    close_b = text.count("}")
    if open_b > close_b:
        text += "\n}" * (open_b - close_b)
    return text


def render_dsl(dsl_path: str, wav_path: str) -> int:
    if not os.path.exists(RENDERER):
        print(f"[bridge] ERROR: renderer not found at {RENDERER}")
        print("[bridge] Run:  make  (from project root)")
        return 1
    cmd = [RENDERER, dsl_path, wav_path]
    print(f"[bridge] Rendering: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout: print(result.stdout, end="")
    if result.stderr: print(result.stderr, end="", file=sys.stderr)
    return result.returncode


def evolve_dsl(dsl_path: str, wav_path: str, gens: int, beam: int) -> int:
    if not os.path.exists(EVOLVER):
        print(f"[bridge] ERROR: evolve binary not found at {EVOLVER}")
        print("[bridge] Run:  make  (from project root)")
        return 1
    cmd = [EVOLVER, dsl_path, str(gens), str(beam)]
    print(f"[bridge] Evolving: {' '.join(cmd)}")
    result = subprocess.run(cmd)
    # evolve writes mcts_evolved.wav — rename to requested path
    evolved = os.path.join(os.path.dirname(EVOLVER), "mcts_evolved.wav")
    if result.returncode == 0 and os.path.exists(evolved) and evolved != wav_path:
        os.rename(evolved, wav_path)
    return result.returncode


def play_wav(wav_path: str) -> None:
    system = platform.system()
    if system == "Linux":
        for player in ["aplay", "paplay"]:
            if subprocess.run(["which", player], capture_output=True).returncode == 0:
                print(f"[bridge] Playing with {player} …")
                subprocess.run([player, wav_path])
                return
        print(f"[bridge] No player found (sudo apt install alsa-utils). Open: {wav_path}")
    elif system == "Darwin":
        subprocess.run(["afplay", wav_path])
    elif system == "Windows":
        os.startfile(wav_path)


def main():
    global LEMON_HOST, LEMON_PORT
    ap = argparse.ArgumentParser(
        description="SHMC Lemonade Bridge — prompt → DSL → audio",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 lemonade/tools/lemonade_bridge.py --prompt "dark jazz at 90 BPM" --play
  python3 lemonade/tools/lemonade_bridge.py --prompt "ambient pad" --out pad.wav --evolve --gens 60
  python3 lemonade/tools/lemonade_bridge.py --prompt "blues riff" --dry-run
""")
    ap.add_argument("--prompt",  required=True,  help="Musical description")
    ap.add_argument("--out",     default="/tmp/output.wav", help="Output WAV path")
    ap.add_argument("--dsl-out", default=None,   help="Also save DSL to this path")
    ap.add_argument("--model",   default="Seed-Coder-8B-Instruct-Q4_K_M",
                    help="Lemonade model name")
    ap.add_argument("--temp",    type=float, default=0.25)
    ap.add_argument("--dry-run", action="store_true",
                    help="Generate and print DSL only — skip render/evolve")
    ap.add_argument("--play",    action="store_true",
                    help="Play result when done")
    ap.add_argument("--think",   action="store_true",
                    help="Enable Qwen3 chain-of-thought (slower)")
    ap.add_argument("--host",    default=LEMON_HOST)
    ap.add_argument("--port",    type=int, default=LEMON_PORT)
    # Evolution flags
    ap.add_argument("--evolve",  action="store_true",
                    help="Run MCTS evolution on the generated DSL (better harmony)")
    ap.add_argument("--gens",    type=int, default=60,
                    help="MCTS iterations for --evolve (default 60)")
    ap.add_argument("--beam",    type=int, default=6,
                    help="Beam width for beam-search comparison (default 6)")
    args = ap.parse_args()

    LEMON_HOST = args.host
    LEMON_PORT = args.port

    print(f"[bridge] Prompt:   {args.prompt!r}")
    print(f"[bridge] Model:    {args.model}  temp={args.temp}")
    print(f"[bridge] Evolve:   {args.evolve}  (gens={args.gens})")
    print(f"[bridge] Server:   {args.host}:{args.port}")
    print(f"[bridge] Thinking: {'on' if args.think else 'off'}")
    print("")

    t0      = time.time()
    raw     = call_lemonade(args.prompt, args.model, args.temp,
                            no_think=not args.think)
    elapsed = time.time() - t0
    print(f"[bridge] LLM responded in {elapsed:.1f}s  ({len(raw)} chars)")

    dsl = sanitise(raw)
    print("\n─── Generated DSL ───────────────────────────────────")
    print(dsl)
    print("─────────────────────────────────────────────────────\n")

    dsl_path = args.dsl_out or args.out.replace(".wav", ".shmc")
    with open(dsl_path, "w") as f:
        f.write(dsl)
    print(f"[bridge] DSL saved → {dsl_path}")

    if args.dry_run:
        print("[bridge] --dry-run: skipping render/evolve")
        return 0

    if args.evolve:
        rc = evolve_dsl(dsl_path, args.out, args.gens, args.beam)
        if rc == 0:
            print(f"[bridge] ✓ Evolved audio → {args.out}")
            if args.play:
                play_wav(args.out)
        else:
            print(f"[bridge] ✗ Evolution failed (exit {rc})")
        return rc
    else:
        rc = render_dsl(dsl_path, args.out)
        if rc == 0:
            print(f"[bridge] ✓ Audio written → {args.out}")
            if args.play:
                play_wav(args.out)
        else:
            print(f"[bridge] ✗ Render failed (exit {rc})")
        return rc


if __name__ == "__main__":
    sys.exit(main())
