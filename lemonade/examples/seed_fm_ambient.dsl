# FM ambient texture — high pitch diversity, long notes

PATCH fm_pad {
  saw ONE
  saw ONE
  adsr 8 4 28 12
  fm $0 $1 3
  mul $3 $2
  lpf $4 20
  out $5
}

MOTIF hi {
  note 72 6 8
  note 76 6 7
  note 79 5 9
}

MOTIF lo {
  note 48 6 12
  note 55 6 10
}

SECTION space 16.0 {
  use hi @ 0  patch fm_pad
  use hi @ 4  patch fm_pad t=7
  use hi @ 8  patch fm_pad t=3
  use lo @ 0  patch fm_pad
  use lo @ 8  patch fm_pad t=5
}

SONG ambient 90.0 {
  play space x2
}
