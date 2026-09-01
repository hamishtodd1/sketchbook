# Bundled fonts

All fonts here are redistributable under permissive licences.

## KaTeX_*.ttf — SIL Open Font License 1.1

The maths faces are the KaTeX distribution of Computer Modern, the typeface
family Donald Knuth designed for TeX. They come from the
[KaTeX](https://github.com/KaTeX/KaTeX) project, which builds them from the AMS
Type 1 originals.

Why these and not a general-purpose serif: they are split the way TeX needs them
to be. `Math-Italic` is the maths variable alphabet, `Main-Regular` is the
upright roman with the operators and relations, and `Size1`..`Size4` hold
progressively larger cuts of the same delimiters and big operators. Growing a
`\left(` to fit its contents is then a matter of picking the right cut rather
than stretching a glyph out of shape.

- `KaTeX_Main-Regular`, `-Bold`, `-Italic` — upright roman, digits, operators,
  relations, uppercase Greek, accents
- `KaTeX_Math-Italic` — maths variables and lowercase Greek
- `KaTeX_AMS-Regular` — AMS symbol extras
- `KaTeX_Caligraphic-Regular` — `\mathcal`
- `KaTeX_Size1`..`Size4-Regular` — grown delimiters, radicals and big operators

Copyright (c) 2013-2020 Khan Academy and other contributors.
Licensed under the SIL Open Font License, Version 1.1:
<https://openfontlicense.org/>

## JetBrainsMono-Regular.ttf — SIL Open Font License 1.1

Used for code. Copyright 2020 The JetBrains Mono Project Authors
(<https://github.com/JetBrains/JetBrainsMono>).
Licensed under the SIL Open Font License, Version 1.1.
