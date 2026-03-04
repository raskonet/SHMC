# complex_song_v2.dsl — Fixed seed based on audio analysis report
# FIXES vs v1:
#   1. Brightness: ZCR was 0.010, Gaussian peak now at 0.012 (calibrated)
#      LPF cutoffs raised (18→26, 22→28) for more harmonic content
#   2. Temporal spread: was 0.031 (3/64 slots). Now uses spread at
#      beats 0,1,2,3,4,6,8,10,12 across 16-beat section → ~9/64=0.14
#   3. Kept audibility/dynamics/pitch_div which were already excellent

PATCH bass {
  saw ONE
  adsr 2 8 20 4
  mul $0 $1
  lpf $2 26
  out $3
}

PATCH lead {
  tri ONE
  adsr 1 6 24 8
  mul $0 $1
  hpf $2 14
  out $3
}

PATCH pad {
  noise
  adsr 12 16 28 14
  mul $0 $1
  lpf $2 28
  out $3
}

MOTIF bass_line {
  note 36 3 10
  note 36 3 8
  note 38 3 9
  note 40 3 10
  note 41 3 8
  note 43 3 9
  note 45 3 8
  note 43 3 7
}

MOTIF lead_melody {
  note 64 2 11
  note 67 2 10
  note 69 2 12
  note 67 3 11
  note 65 2 10
  note 64 2 11
}

MOTIF counter_mel {
  note 60 3 8
  note 62 2 7
  note 64 3 9
  note 62 2 8
}

MOTIF accent {
  note 72 1 13
  note 72 1 11
  note 74 1 12
}

SECTION verse 16.0 {
  use bass_line @ 0.0 x1 patch bass t=0 v=1.0
  use bass_line @ 8.0 x1 patch bass t=2 v=0.9
  use lead_melody @ 1.0 x1 patch lead t=0 v=0.8
  use counter_mel @ 3.0 x1 patch lead t=5 v=0.7
  use accent     @ 5.0 x1 patch lead t=12 v=0.8
  use lead_melody @ 9.0 x1 patch lead t=3 v=0.7
  use counter_mel @ 11.0 x1 patch pad t=0 v=0.5
  use accent     @ 13.0 x1 patch lead t=7 v=0.6
}

SECTION chorus 8.0 {
  use bass_line  @ 0.0 x1 patch bass t=5 v=1.0
  use lead_melody @ 0.0 x1 patch lead t=5 v=1.0
  use accent     @ 2.0 x1 patch lead t=12 v=0.9
  use counter_mel @ 3.0 x1 patch pad t=0 v=0.6
  use accent     @ 5.0 x1 patch lead t=10 v=0.8
  use counter_mel @ 6.0 x1 patch lead t=7 v=0.7
}

SONG full_track 120.0 {
  play verse x2
  play chorus x2
}
