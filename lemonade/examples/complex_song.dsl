# complex_song.dsl — Multi-layer seed for advanced search
# 3 patches: bass SAW+lpf, lead TRI+hpf, pad NOISE+lpf
# 4 motifs: walking bass, lead melody, countermelody, accent
# 2 sections: verse 16 beats, chorus 8 beats
# 1 song: verse x2 chorus x2 at 120 BPM
PATCH bass {
  saw ONE
  adsr 2 8 20 4
  mul $0 $1
  lpf $2 18
  out $3
}

PATCH lead {
  tri ONE
  adsr 1 6 24 8
  mul $0 $1
  hpf $2 8
  out $3
}

PATCH pad {
  noise
  adsr 12 16 28 14
  mul $0 $1
  lpf $2 22
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
  use bass_line @ 0.0 x2 patch bass t=0 v=1.0
  use lead_melody @ 0.0 x1 patch lead t=0 v=0.8
  use counter_mel @ 4.0 x2 patch lead t=7 v=0.6
}

SECTION chorus 8.0 {
  use bass_line @ 0.0 x1 patch bass t=5 v=1.0
  use lead_melody @ 0.0 x1 patch lead t=5 v=1.0
  use accent @ 0.0 x2 patch lead t=12 v=0.9
  use counter_mel @ 2.0 x1 patch pad t=0 v=0.5
}

SONG full_track 120.0 {
  play verse x2
  play chorus x2
}
