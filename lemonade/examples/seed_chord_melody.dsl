# Chord + melody — two patches, beat diversity, transpose algebra

PATCH lead {
  saw ONE
  adsr 0 2 20 8
  mul $0 $1
  lpf $2 30
  out $3
}

PATCH pad {
  tri ONE
  adsr 4 6 28 10
  mul $0 $1
  lpf $2 18
  out $3
}

MOTIF melody {
  note 60 4 12
  note 64 4 10
  note 67 4 11
  note 65 4 9
}

MOTIF bass {
  note 48 6 14
  note 52 6 12
}

SECTION verse 16.0 {
  use melody @ 0  x2 patch lead
  use melody @ 8  x2 patch lead t=5
  use bass   @ 0  x4 patch pad
}

SONG demo 110.0 {
  play verse x2
}
