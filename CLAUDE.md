# sketchbook

An infinite pan/zoom canvas for **exploring mathematics through programming**.
C++20 on raylib. Boxes hold GLSL and read as LaTeX; values get knobs; functions
and values are drawn by vizes in display windows.

Three documents, and which one wins:

- **`DESIGN.md`** -- what the program is and every decision about how it behaves,
  with the reasons. Written for Hamish to read. **It wins**: if the code or
  another document disagrees with it, the other one is wrong, or it is a question.
- **`TECHNICAL.md`** -- how those decisions are kept in the code: mechanisms, file
  names, measurements, and lessons learnt the hard way. **Read the section for an
  area before changing that area.**
- **`README.md`** -- building, and the inputs, for someone using the program.

## Build and run

```sh
cmake --preset mingw          # or --preset default for MSVC
cmake --build --preset mingw
./build/sketchbook.exe
```

Building while the app is open works (the running exe is moved aside). Dev
flags: `--screenshot <file.png> --frames <n> --width <w> --height <h>`,
`--open <file>`, `--verbose`, `--assemble <file>`, `--describe`, `--converse "..."`.
How to test on this machine
without disturbing Hamish's own session is in `TECHNICAL.md`, *Building and
testing* -- read it before launching anything.

## Working agreement

- **Ask questions often.** Hamish is still working out what this program is.
  Questions should be short, concrete, and understandable without re-reading the
  codebase. Mix immediate ("what should the caret do here?") with long-range
  ("should a value ever be shared between two equations?").
- **Expect churn.** Things will be changed, changed back, and rewritten from
  scratch. Prefer designs that are cheap to throw away over designs that are
  general. Do not build abstraction for a requirement that has not been stated.
- **The goal is a statement, not a feature list.** We are trying to get the
  LaTeX <-> GLSL relationship down to something that can be written in a few
  paragraphs.
- **When a decision is made, record it in `DESIGN.md`** and delete whatever it
  supersedes; put how it is done in `TECHNICAL.md`. One place for each thing:
  a fact stated in two places drifts.
- **If a request contradicts `DESIGN.md`, say so before implementing it.** A
  contradiction left standing is how the vizes drifted (`DESIGN.md` §9).
- **`prompt/system.md` tells Claude-in-the-app how the language works.** When the
  language or the windows change, change it too, or the Claude in the app is told
  something false.

## Hard rules

Each is argued for in `DESIGN.md` and explained in `TECHNICAL.md`.

- **A value never recompiles.** Every number a variable is given is a uniform;
  dragging, playing, typing a number and undoing any of them compile nothing.
- **Cost is proportional to what is on screen**, and nothing is redone whose
  inputs have not changed. The frame budget is 8.3 ms. Measure before optimising.
- **Deleting leaves nothing behind**: GPU objects released by hand, pointers into
  blocks cleared first, everything that grows bounded, child processes stopped.
- **Every edge is smooth**: render targets multisampled, shader edges as coverage.
- **raylib stays extractable**: new logic in raylib-free files, no raylib types in
  domain headers, drawing through the existing helpers.
- **How a type is drawn is a viz**, written in a vizes sketch (`vizes/`) or on the
  canvas -- not new C++ in a dw.
- **The page and the shader never disagree** about what a line means.
- **A renamed spelling is renamed, not aliased**; the sketches are rewritten.
