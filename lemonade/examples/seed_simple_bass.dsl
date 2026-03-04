# Simple bass line — good starting seed for search
# Two-note walking bass, SAW oscillator, slow attack

PATCH bass {
  saw ONE
  adsr 0 4 24 6
  mul $0 $1
  lpf $2 22
  out $3
}

MOTIF walk {
  note 36 4 12
  note 43 4 10
}

SECTION main 8.0 {
  use walk @ 0 x4 patch bass
}

SONG demo 120.0 {
  play main
}
