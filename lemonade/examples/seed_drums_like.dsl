# Percussive noise-based rhythm — beat spread test

PATCH perc {
  noise
  adsr 0 0 0 2
  mul $0 $1
  hpf $2 15
  out $3
}

PATCH kick {
  saw ONE
  adsr 0 0 0 4
  mul $0 $1
  lpf $2 8
  out $3
}

MOTIF hit {
  note 60 2 15
}

SECTION beat 4.0 {
  use hit @ 0   patch kick
  use hit @ 1   patch perc
  use hit @ 2   patch kick
  use hit @ 3   patch perc
}

SONG demo 140.0 {
  play beat x4
}
