# sketchbook

An infinite pan/zoom canvas for **exploring mathematics through programming**.
C++20 on raylib. See README.md for how the existing pieces work; this file is
about *what we are trying to build* and how to work on it.

## Build and run

```sh
cmake --preset mingw          # or --preset default for MSVC
cmake --build --preset mingw
./build/sketchbook.exe
```

Dev flags for capturing a frame without a human at the keyboard:
`--screenshot <file.png> --frames <n> --width <w> --height <h>`.

## Working agreement

- **Ask questions often.** Hamish is still working out what this program is.
  Questions should be short, concrete, and understandable without re-reading the
  codebase. Mix immediate ("what should the caret do here?") with long-range
  ("should a value ever be shared between two equations?").
- **Expect churn.** Things will be changed, changed back, and rewritten from
  scratch. Prefer designs that are cheap to throw away over designs that are
  general. Do not build abstraction for a requirement that has not been stated.
- **The goal is a statement, not a feature list.** We are trying to get the
  LaTeX <-> C++ relationship down to something that can be written in a few
  paragraphs. When a decision is made, record it here and delete whatever it
  supersedes.

## Standing constraints

### Performance matters

This is an infinite canvas that people will pan and pinch with a finger. It has
to hold display rate while doing it, and a stutter is felt immediately. Treat
the frame budget as a real constraint, not something to tidy up later.

What already protects it, and should not be undone:

- **Font atlases are cached on a size ladder** with an LRU, so zooming does not
  re-rasterise every glyph every frame (`fontstore.*`).
- **Per-line LaTeX layout is cached** and rebuilt only when that line's text or
  the em size actually changes (`CodeBlock::EnsureCache`).
- **Compiling and running happen on worker threads**, and repeated knob moves
  coalesce into one queued re-run rather than a queue of them
  (`mathprogram::Program`).
- **Drawing is culled to the screen**, not to the canvas: the grid iterates only
  the dots in view.

The rule for new work: keep the cost proportional to what is *on screen*, not to
what is on the canvas, and never redo work whose inputs have not changed.

The known soft spots, in the order they will start to hurt: `CodeBlock::Measure`
runs several times per frame per box, and placement leans on it again while a
box is being dragged; `highlight::Cpp` re-tokenises every
visible code line every frame; `tex::DrawLayout` re-resolves each glyph as it
draws. All are fine at present scale and all are cheap to memoise when they stop
being fine. Measure before optimising any of them.

### raylib is not permanent

raylib is a convenience, not a foundation. Assume it will be swapped one day and
keep it extractable.

It is already out of the way in the half of the program that matters most: the
mathematics and the toolchain do not know it exists. `mathline.*`,
`mathprogram.*`, `compiler.*`, `process.*`, `filedialog.*` and a type's own file
(`types/RealFunction.*`, `types/examples.h`) include no raylib header and use no
raylib type. The Win32 files additionally *cannot* include it,
since `<windows.h>` and raylib both declare `Rectangle`, `CloseWindow` and
`ShowCursor`.

What is coupled is rendering and input: roughly a hundred raylib calls across
`blocks.cpp`, `app.cpp`, `ui.cpp`, `types/RealViz.cpp`,
`types/RealFunctionViz.cpp`, `viewport.cpp`, `grid.cpp`, `text.cpp`,
`tex/tex_layout.cpp` and `fontstore.cpp`, plus `Vector2`,
`Rectangle` and `Color` used as vocabulary types throughout the render layer.
`fontstore.*` is the deepest hook: it depends on raylib's atlas packing and on
its `fontSize` meaning a stbtt pixel height rather than an em.

So, the rules:

- **New logic goes in a raylib-free file.** If a new module is about
  mathematics, parsing, or talking to the outside world, it should compile
  without raylib. Check by trying.
- **Do not let raylib types into domain headers.** They are fine in the render
  layer; they are not fine in a header describing what a variable or a
  diagnostic is.
- **Funnel drawing through the existing helpers** (`text::Draw`, `tex::DrawLayout`,
  `DrawCard`, `DrawRim`) rather than calling raylib directly from new code, so
  the surface that would have to be reimplemented stays small and listed.

## The core idea (unsettled — this is the thing we are figuring out)

Some code expresses sophisticated mathematics. Written the way a mathematician
would write it, it uses different symbols than the code does: `dot(a,b)` is
`a·b`.

So: **behind every piece of LaTeX on the canvas there is some compileable C++.**
The LaTeX is what you read; the C++ is what runs. Clicking a LaTeX box turns it
into its C++; clicking away turns it back.

The pivotal move is what happens to abstract statements. A mathematician writes

> x + 2 = y,  x, y ∈ ℝ

which is *about* real numbers in general. A program cannot be about reals in
general, so **we choose values for x and y**. Choosing is not a compromise, it
is the whole mechanism: concrete values are what make a thing visualizable, and
visualization is the point.

Visualization has not been started yet. It is where this is heading.

### Settled decisions

**The C++ is the source of truth.** The LaTeX is generated from it, by parsing a
restricted subset. Nobody hand-writes the LaTeX.

**Declarations stay abstract; values are injected.** `float a;` is valid input,
and it renders as `a \in \mathbb{R}`. To actually compile, the app injects an
initialiser so it becomes `float a = 0.2385;`. Every variable gets one,
including function arguments.

The injected text is **not part of the editable source**. It is generated, per
type, by an `exampleGenerator` for that type. So each supported type needs two
things: how to render it as LaTeX, and how to make an example value of it.

Injections are shown, in a distinct colour, when you *hover* the LaTeX. They
disappear when you *click to edit*, because they are not yours to edit.

On top of the injected starting value, free variables get **knobs** — a slider
for a real. That is the "choosing values" mechanism made physical.

**Boxes need not be whole programs.** Fragments should compile, with the app
supplying scaffolding for common patterns; whole programs should also be
possible. GLSL should eventually be possible too.

**Only some code has a mathematical reading.** In a box that mixes the two, the
parts that do have a LaTeX equivalent become LaTeX inline, expanding or
contracting that region of the box. The rest stays as code.

**The LaTeX never shows numbers.** It is the mathematician's statement and stays
abstract. Concrete values live in the knobs and in the hovered code view.

**The flip is per-line.** Each line of a box is independently LaTeX, hovered
code, or code being edited. A line with no mathematical reading just stays code.

**Editing a box means editing all of it.** Giving a box the caret turns the
whole thing into code, and hides the injected values throughout: while you are
editing, what you see is your own text. A toggle on the focused box switches it
back to reading as mathematics, with only the caret's line left as code.

**Every window sits on the grid, and boxes never overlap.** A window's top-left
corner snaps to the nearest dot of the grid *as drawn*, so placement gets finer
as you zoom in. That holds for boxes, number lines and plots alike
(`SnapToGrid`, in `grid.*`, next to the code that draws the dots). Boxes
additionally never overlap: one put down on top of another steps clear along
whichever edge is nearest, a whole dot at a time, and the number lines and plots
docked under a box count as part of it. Every box placement goes through one
function (`App::PlaceBlock`), so dragging, typing a new box and loading a file
cannot disagree about the rules.

**A drag in flight owns the pointer.** Hovering a rim claims the pointer, so
without this rule letting go of one window over another window's rim would never
deliver the release, and the dragged window would stay stuck to the cursor.

### Showing that two things are the same object

A canvas is many windows about one program. A variable is written in the
equation, again in the code behind it, and again on whatever window stands for
its value -- and nothing about where those things sit on screen says they are
the same thing. **Making that connection visible is a key part of the program**,
not decoration, and every new kind of window has to answer for it.

Two mechanisms so far, and both should be extended rather than replaced:

**Colour is identity.** Every variable has its own colour, taken from
**viridis** by its position in its box (`VariableColor`, in `theme.h`), and
everything standing for that variable wears it: the handle or marker on its
number line, its curve on a plot, its name written beside either. Viridis
because it is perceptually even -- two colours the same distance apart on the
map look equally different -- and the walk along it is by the golden ratio, so
neighbouring variables land far apart and adding one never recolours the others.
The dark end is skipped, since the cards are already dark.

Colour therefore no longer says whether a variable is free or derived. **Shape**
does: a ring you can drag, a filled dot you cannot. They are different questions
and deserve different channels.

**Using a window boxes the name.** Hover or grab a number line -- and, in time,
any other visualization -- and the *mention* of its variable lights up in the
box it came from, in that variable's colour, wherever it is written: in the
LaTeX or in the code, whichever that line is currently showing, and on every
line that mentions it rather than only its declaration.

Finding the mention is easy in code, where the font is monospaced and a name is
a column. In LaTeX it needs help, and the way it gets it is worth knowing: when
`mathline` writes the LaTeX it wraps every identifier in `\sbref{k}{...}`, a
command that typesets as exactly its argument and does nothing else. The tag
rides through parsing onto the node, and through layout onto every item the node
produced. So the renderer can ask "where did the third identifier end up?" and
get an answer in pixels, without the typesetter knowing anything about
variables.

### Number lines

Every variable gets one: free variables get a draggable handle, derived ones a
read-only marker that moves when its inputs do.

They start **docked** under their box and can be pulled off onto the canvas.
Dropping one onto another **merges** them onto a single shared axis — this is a
*viewing* operation, not a modelling one. The variables are not identified; they
simply share one scale so you can compare them. The merged axis reframes around
every value now on it, so all of them are in view with padding at the ends:
seeing them together is the point of the gesture, and the host's old window need
not contain the arriving value at all. Plots merge the same way, and reframe the
same way -- but only ever outwards, since zooming in on a merge would cut off
the input range you were already reading.

Each axis zooms and pans **independently of the canvas**. Two lines may merge
even when their variables come from **different boxes** -- an axis belongs to
the canvas, not to a box.

A derived variable's marker is **read-only**. If you want it to move, move one
of its inputs. Dragging it would mean solving backwards, which is the solver we
are not building.

### The types directory

`src/types/` is where a supported type lives, and it is meant to become the
place a **user** drops an extension. Today its contents are compiled into the
app; the eventual form is probably files loaded at runtime. Either way the
directory is the unit: to add a type you add files there, and nothing outside
should have to know.

A type is three concerns, and they are three files:

| file | what it holds |
| --- | --- |
| `RealFunction.*` | the type itself: the C++ the app injects so `RealFunction f;` compiles, and the statement that prints one back |
| `RealFunctionViz.*` | how one is shown (the plot) and how an example of one is made |
| `RealViz.*` | the same two things for a real: the number line, and a tidy number |

There is no `Real.*`, because `float` is already C++ and needs no definition.
That asymmetry is the point of splitting a type from its viz: only some types
are new vocabulary, but all of them need showing.

**Choosing a value and showing it are one job**, which is why the example
generator sits with the visualization rather than with the type. Both halves
answer the same question: make this abstract declaration concrete enough to look
at. The generators are declared in `types/examples.h`, apart from the viz files,
only because the toolchain has to call them and the toolchain does not include
raylib.

What is still *outside* `src/types/` and would have to move before a type could
really be dropped in: `mathline` knows the spelling `RealFunction` and its LaTeX
reading, and `mathprogram` has a `Kind` enum with one entry per type. Those are
the seams to cut when this becomes a real extension point.

### Functions from reals to reals

The second type. `RealFunction` is the C++ behind `f : R -> R`, and it is
literally a `std::function<float(float)>`:

```cpp
struct RealFunction : std::function<float(float)> {
  using std::function<float(float)>::function;
};
```

Nothing to learn and nothing to take on trust: it is exactly what a C++
programmer already means by a function from floats to floats. It is defined in
the prologue, along with the operators that make these functions a **vector
space** -- `+`, `-`, unary `-`, and multiplication by a scalar either way round.
Pointwise `*` and division by a scalar come along too, which makes it an algebra
as well, but the vector space is the part that matters.

So a box reads:

```cpp
RealFunction f;                                  f : R -> R
RealFunction g = [](float x) {                   g(x) = sin(1.4x)
    return sin(1.4f * x); };
RealFunction h = f + 2 * g;                      h = f + 2g
```

Three things follow from the settled decisions, and each needed a choice:

**A concrete function is a lambda.** Not an expression DSL with a magic `x`: a
lambda is what a C++ programmer writes, its parameter cannot collide with a
variable in the box, and it reads as a definition. `mathline` reads a lambda
only as a whole initialiser, and renders it the way a function is defined, as
`f(x) = ...` rather than point-free.

**The example generator produces source, not a number.** `ExampleFunction` picks
one of four recognisable shapes -- a wave, a parabola, a bump, a line -- with a
coefficient, and writes out the lambda. It is deterministic in the variable's
name, like `ExampleFloat`.

**A function reaches the screen as samples.** There is no other way to send one
back from a program: the generated code calls `sb_plot`, which prints the
function's value at `kSampleCount` points across a fixed window, and the plot
draws the polyline. A free *real* reaches the program through argv, so moving
its knob is a re-run; a free *function* cannot, so its example is written into
the source and changing it would be a rebuild. That is why a function has no
knob yet.

### Plots

A real is seen on a number line; a function is seen on a plot. **Input runs up
the vertical axis and output along the horizontal one** -- the transpose of the
usual convention, chosen because these axes are heading towards spacetime
diagrams, where the vertical axis is time.

Both axes are drawn at one scale, so the window is square and a slope of one is
a diagonal. Everything else follows the number lines: a plot starts docked under
its box (below the number lines), is moved by its rim, snaps to the grid, zooms
on its own wheel, and **merges with another plot when dropped onto it**. That
last one is how the vector space becomes visible: f, g and f + 2g against one
set of axes, each curve named in its own colour.

### First proof of concept

Scalar arithmetic: reals, and `+ - * / =`, with all of the above working —
per-line flip, injected values on hover, number lines, merging, per-axis zoom.

Moving a knob **re-runs an already-compiled program** rather than recompiling.
Free variables are passed in as arguments and every variable's value is printed
back. Recompilation happens only when the code itself changes.

### Non-goals

- **Not a theorem prover.** Nothing here decides whether a statement is true.
- **Not a LaTeX compiler.** Compiling arbitrary LaTeX to code is not on the
  table. Turning *some* LaTeX back into code would be nice and will need heavy
  assumptions about what the LaTeX means.
- **Not a general C++ pretty-printer.** Rendering arbitrary C++ as LaTeX is not
  on the table either. Only a chosen subset will have a mathematical reading.

### Long-term goals (context only, do not build for these yet)

1. Reading papers and textbooks that are short on illustrations.
2. A sketchbook for one's own mathematical thinking.
3. Presenting mathematics to other people.

All three want the same thing: **LaTeX equations linked to live
visualizations.**

## Where things stand

The scalar proof of concept is working end to end. A box of

```
float a;
float b;
float c = (a + b) / 2;
```

renders as `a ∈ ℝ`, `b ∈ ℝ`, `c = (a+b)/2` (a real fraction), with three number
lines docked underneath. Dragging a knob re-runs the compiled program and the
derived marker follows. Hovering a line turns it back into `float a = -0.778;`
with the injected part in blue; clicking it hides the injection and edits the
code. Lines merge onto a shared axis by dragging one grip onto another, and each
axis zooms on its own wheel.

Editing a box turns the *whole* box into Monokai-coloured code, with a
math/code toggle and a cross to delete it, both shown only while the box has
the caret. Undo is Ctrl+Z; Ctrl+left/right move by word. A line the compiler
complained about always reveals its code, so the red caret has real characters
to point at; the message sits in a white balloon in a proportional face.

Canvases save and load: Escape menu buttons, Ctrl+S (straight back to where the
canvas came from) and Ctrl+O, plus `--open <file>` on the command line. The
format is plain text, one record per line, in `App::SaveCanvas`.

Number lines belong to the canvas rather than to a box, so an axis can carry
variables from two different boxes at once.

Functions work end to end as well. A box of

```
RealFunction f;
RealFunction g;
RealFunction h = f + 2 * g;
```

renders as `f : R -> R`, `g : R -> R`, `h = f + 2g`, with three plots docked
underneath drawing the curves. Hovering a declaration shows the lambda the app
chose for it, in blue. Dropping one plot on another puts both curves on one set
of axes in different colours. Reals and functions mix in one box: `float a;` with
`RealFunction g = a * f;` gives a knob that reshapes a curve as you drag it,
still without recompiling.

Every window is moved by the **rim** around its edge, which appears on hover.
Boxes are sized for the worst case across both readings, so nothing changes
shape when a line flips or a value changes width. They snap to the grid and
push each other apart rather than overlapping.

The canvas pans on the **right button** and zooms on the wheel; one finger pans
and two pinch. The left button belongs to the contents of the canvas -- a click
places the caret, a rim drag moves a window, a handle drag chooses a value --
and never moves the canvas itself. Nothing on the canvas uses the right button,
so panning works wherever the pointer happens to be, even over a window that has
claimed the wheel for its own scale.

Key files: `mathline.*` (C++ subset -> LaTeX), `mathprogram.*` (assemble /
compile / re-run, and the scaffolding a fragment is wrapped in), `types/` (one
directory per supported type: what it is, how to make an example of it, how to
draw it), `blocks.*` (per-line rendering and editing), `highlight.*` (Monokai),
`filedialog_win32.*` (open/save dialogs).

`bool testFunc()` with its red/blue square still works, but only as a fallback
for a box that declares no variables. It is scaffolding from before this
direction existed and can go whenever it stops being useful.

## Open questions

Kept here so they are not lost between sessions. Answered ones move up into the
sections above.

- **What is a knob for a function?** A real has a slider because its value goes
  down argv. A function's example is compiled in, so today there is nothing to
  drag -- the only way to change `f` is to type a different lambda. Options: put
  knobs on the *coefficients* inside the generated lambda (`sin(1.4x)` gets a
  slider for the 1.4), or accept that choosing a function means a rebuild.

- **Composition.** The old TypeScript version left the same question open: "have
  a compose function, but how? Maybe `|`?". `f | g`, `f(g)`, or `compose(f, g)`?
  Whatever it is has to read as `f o g`.

- **The injected lambda is long**, and boxes are sized for the worst case across
  both readings, so any box declaring a function is wide even when the injection
  is hidden. Live with it, or let hovered code overflow the card?

- Functions are sampled over a fixed window, so zooming a plot out far enough
  runs out of curve. The alternative is to let the plot's view drive the
  sampling and re-run on every zoom step, exactly as a knob does.

- `vec2` is the next type after that, and it is where a 2D knob would be needed.

- Should the prologue be visible anywhere? It is the app's half of the bargain
  -- what `RealFunction` is, what operators exist -- and right now nothing in the
  program shows it to you.

- GLSL: what would that even look like? Far off, but it shapes how much we
  assume about the target language.
- Undo is per box and is lost when a box is deleted; there is no canvas-level
  undo. Is that enough?
- Nothing warns before discarding unsaved work on load or on close.
