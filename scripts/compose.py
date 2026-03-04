#!/usr/bin/env python3
import http.client, json, subprocess, argparse, sys, os, textwrap, time, re, platform

_DIR  = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.join(_DIR, "..")
RENDERER = os.path.join(_ROOT, "bin", "shmc_render")
EVOLVER = os.path.join(_ROOT, "bin", "shmc_evolve")

SYSTEM_PROMPT = textwrap.dedent('''\
You are an expert audio programmer. Output ONLY valid SHMC DSL. No markdown, no prose.

CRITICAL HIERARCHY RULES:
1. PATCH defines sounds. Must end with "out $N". First op is $0, next $1...
2. MOTIF defines melodies/beats using "note" and "rest".
3. SECTION schedules MOTIFs. YOU CANNOT "use" A SECTION INSIDE A SECTION! The "use" command ONLY takes Motif names!
4. SONG schedules SECTIONs using "play <section_name>".

VALID PATCH OPS:
  osc sin ONE, saw ONE, tri ONE, osc noise ONE
  fm $A $B <depth>
  lpf $A <cutoff_0_to_63>
  hpf $A <cutoff_0_to_63>
  bpf $A <cutoff> <q>
  adsr <attack_0_to_31> <decay> <sustain> <release>
  mul $A $B
  add $A $B
  tanh $A, clip $A, fold $A
  out $A

VALID MOTIF OPS:
  note <midi_21_to_108> <dur_3_to_6> <vel_0_to_15>
  rest <dur_3_to_6>

VALID SECTION OPS:
  use <motif_name> @ <beat>[x<repeats>] patch <patch_name> [t=<transpose_semitones>][v=<vel_scale>]

VALID SONG OPS:
  play <section_name> [x<repeats>]

DRUM GUIDE:
  Kick:  osc noise ONE -> adsr 0 4 0 3 -> mul -> lpf 12
  Snare: osc noise ONE -> adsr 0 5 0 4 -> mul -> lpf 28
  Hihat: osc noise ONE -> adsr 0 2 0 1 -> mul -> hpf 52

EXAMPLE:
PATCH lead { 
    saw ONE         # $0
    adsr 2 8 18 10  # $1
    mul $0 $1       # $2
    lpf $2 40       # $3
    out $3 
}
PATCH kick { 
    osc noise ONE   # $0
    adsr 0 4 0 3    # $1
    mul $0 $1       # $2
    lpf $2 12       # $3
    out $3 
}
MOTIF melody { 
    note 60 4 10
    rest 4
    note 64 4 9 
}
MOTIF beat { 
    note 36 4 12 
}
SECTION main_loop 8.0 {
    use melody @ 0 x2 patch lead
    use beat @ 0 x8 patch kick
}
SONG demo 120.0 {
    play main_loop x4
}
''').strip()

def call_llm(prompt, model, temp, host, port):
    payload = json.dumps({
        "model": model, "temperature": temp, "max_tokens": 1024,
        "messages":[{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": prompt + " /no_think"}]
    }).encode()
    conn = http.client.HTTPConnection(host, port, timeout=600)
    conn.request("POST", "/v1/chat/completions", body=payload, headers={"Content-Type": "application/json"})
    res = conn.getresponse()
    raw = res.read().decode()
    conn.close()
    if res.status != 200: raise RuntimeError(f"HTTP {res.status}: {raw}")
    return json.loads(raw)["choices"][0]["message"]["content"]

def sanitise(raw):
    text = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL)
    lines =[l for l in text.splitlines() if not l.strip().startswith("```")]
    text = "\n".join(lines).strip()
    open_b = text.count("{")
    close_b = text.count("}")
    if open_b > close_b: text += "\n}" * (open_b - close_b)
    return text

def play_wav(wav_path):
    system = platform.system()
    if system == "Linux":
        for player in["aplay", "paplay"]:
            if subprocess.run(["which", player], capture_output=True).returncode == 0:
                subprocess.run([player, wav_path])
                return
    elif system == "Darwin":
        subprocess.run(["afplay", wav_path])
    elif system == "Windows":
        os.startfile(wav_path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--out", default="composed.wav")
    ap.add_argument("--model", default="Seed-Coder-8B-Instruct-Q4_K_M")
    ap.add_argument("--temp", type=float, default=0.25)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8002)
    ap.add_argument("--evolve", action="store_true")
    ap.add_argument("--iters", type=int, default=60)
    ap.add_argument("--play", action="store_true")
    args = ap.parse_args()

    print(f"\n[Compose] Prompt: {args.prompt!r}")
    
    t0 = time.time()
    raw = call_llm(args.prompt, args.model, args.temp, args.host, args.port)
    print(f"[Compose] LLM responded in {time.time()-t0:.1f}s")
    
    dsl = sanitise(raw)
    print(f"\n─── Generated DSL ───\n{dsl}\n─────────────────────\n")
    
    dsl_path = args.out.replace(".wav", ".shmc")
    with open(dsl_path, "w") as f: f.write(dsl)
    
    print(f"[Compose] Step 1: Rendering raw LLM output...")
    rc = subprocess.run([RENDERER, dsl_path, args.out], cwd=_ROOT).returncode
    if rc != 0:
        print(f"\n[Compose] ✗ Render failed. The LLM generated invalid DSL. Please try the prompt again.")
        sys.exit(rc)
        
    print(f"[Compose] ✓ Original track saved to {args.out}")
    if args.play:
        play_wav(args.out)

    if args.evolve:
        print(f"\n[Compose] Step 2: Running Evolution (iters={args.iters})...")
        rc_evo = subprocess.run([EVOLVER, dsl_path, str(args.iters), "6"], cwd=_ROOT).returncode
        if rc_evo != 0:
            print(f"[Compose] ✗ Evolution failed.")
            sys.exit(rc_evo)
            
        evolved_out = args.out.replace(".wav", "_evolved.wav")
        mcts_wav = os.path.join(_ROOT, "mcts_evolved.wav")
        if os.path.exists(mcts_wav):
            os.rename(mcts_wav, evolved_out)
            print(f"\n[Compose] ✓ Evolved track saved to {evolved_out}")
            if args.play:
                play_wav(evolved_out)
        else:
            print(f"[Compose] ✗ Could not find evolved wav.")

if __name__ == "__main__": main()
