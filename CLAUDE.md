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
  re-rasterise every glyph every frame (`fontstore.*`). The LRU **may not evict
  anything used since the frame began**: a glyph hands its caller a pointer
  straight into an atlas texture, so throwing one away mid-frame both dangles
  that pointer -- the letters come out as smudges -- and asks for a whole face
  to be rasterised again inside the frame budget. Over budget with nothing
  evictable, the cache simply runs large for a frame.
- **Per-line LaTeX layout is cached** and rebuilt only when that line's text or
  the em size actually changes (`CodeBlock::EnsureCache`).
- **Compiling and running happen on worker threads**, and repeated knob moves
  coalesce into one queued re-run rather than a queue of them
  (`mathprogram::Program`).
- **Drawing is culled to the screen**, not to the canvas: the grid iterates only
  the dots in view.

The rule for new work: keep the cost proportional to what is *on screen*, not to
what is on the canvas, and never redo work whose inputs have not changed.

### Deleting leaves nothing behind

Things are made and thrown away constantly here -- boxes, number lines, plots,
pictures, whole canvases at a time -- so **a deleted thing has to stop costing
anything**. Not almost nothing: nothing. Five rules, each learnt from something
that was getting it wrong:

- **A destructor cannot free what is on the GPU.** Releasing a shader or a
  texture needs the graphics context, and destructors run after the window has
  gone. So whoever destroys a box releases it first, by hand
  (`CodeBlock::ReleaseGpu`), and `~CodeBlock` complains rather than leaking in
  silence if they forgot. Loading a canvas is one of those places, and was
  quietly leaking every shader on the old canvas until this was written down.

- **Every pointer into the blocks stops pointing before the blocks stop
  existing.** The panels hold raw `CodeBlock*` -- that is the point of `Forget`
  -- and so do `focus_`, `dragBlock_` and the selection drag. A gesture in
  flight is the dangerous case: Ctrl+X while a selection is being carried would
  free the very boxes the drag is holding, which is why the keys that delete
  stand down while a drag is on.

- **Work in flight still owns things.** A compile that finishes after nobody
  wants it any more has already written an executable, so `~Program` *takes*
  the build's result rather than only waiting for it -- dropping the future
  unread left that file on disk for good. Opening a canvas from the command
  line did exactly that, every time, because it throws away the starting box
  while its first compile is still running.

- **Anything that grows with use needs a bound.** The undo stack keeps 200
  steps and drops the oldest; the font atlases are an LRU on a size ladder; a
  video decodes at most a handful of frames ahead of the one being shown, and
  the thread filling that queue blocks rather than running on. A thing that
  grows with how long the app has been open is a leak with extra steps.

- **A child process is a thing that has to stop.** A video window holds an
  `ffmpeg` reading down a pipe, and closing the window has to end it -- not when
  the file runs out, and not at exit. So the decoder goes when the window does,
  and the check is the same shape as the one for the disk: open a canvas with a
  video on it, throw the video away, and no `ffmpeg` should be running.

"No trace" includes the disk. A box that compiles leaves a temporary executable
behind it, and it goes when the box stops needing it rather than at exit -- so
the check is easy to run: open a canvas, close it, and the working directory
should be empty.

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
`mathprogram.*`, `compiler.*`, `process.*`, `filedialog.*`, `video.*` and a
type's own file (`types/RealFunction.*`, `types/examples.h`) include no raylib
header and use no raylib type. `video.*` is the newest of them and was written
to the rule: it hands back frames as bytes, and `media.cpp` is where those bytes
become a texture. The Win32 files additionally *cannot* include it,
since `<windows.h>` and raylib both declare `Rectangle`, `CloseWindow` and
`ShowCursor`.

What is coupled is rendering and input: roughly a hundred raylib calls across
`blocks.cpp`, `app.cpp`, `ui.cpp`, `media.cpp`, `types/RealViz.cpp`,
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

**Numbers read as numbers.** `2.`, `2.0` and `2` are the same number, and it is
written `2`. The trailing zeros and the trailing point are how a float is
spelled in code, like the `f` suffix that was already being dropped -- none of
it is mathematics. `1.5` keeps its digits; `100` is left alone.

**`:=` for a rule, `=` for a fixed value.** In code `=` always *makes* the left
side equal to the right, which is what a mathematician writes `:=` for. But a
line whose right-hand side is a bare number is not a rule, it fixes a value once
-- and "let a = 0.4" is how that is written. So the test is whether the
right-hand side mentions anything: `c := rac{a+b}{2}` recomputes when a or b
move, `a = 0.4` never moves. The distinction is worth having on screen because
it is exactly the difference between the lines that respond to a knob and the
lines that do not. TeX has no glyph for it, so it is composed.

**The LaTeX never shows numbers.** It is the mathematician's statement and stays
abstract. Concrete values live in the knobs and in the hovered code view.

**The flip is per-line.** Each line of a box is independently LaTeX, hovered
code, or code being edited. A line with no mathematical reading just stays code.

**Editing a box means editing all of it.** Giving a box the caret turns the
whole thing into code, and hides the injected values throughout: while you are
editing, what you see is your own text. A toggle on the focused box switches it
back to reading as mathematics, with only the caret's line left as code.

**Dragging across a box highlights its text**, and so does holding shift while
the caret moves -- shift means only "leave the anchor where it is", which is one
rule covering the arrows, word movement and Home/End alike. The caret and the
anchor are the same kind of thing, so "is anything highlighted" is one
comparison and needs no flag; typing over a highlight replaces it, and moving
the caret without shift puts it down.
It exists for its own sake, but the reason it was built is the gesture above:
selecting part of a name is how you take hold of what that name refers to.

**The grid is only up while it is being used.** The dots are a placement aid,
not scenery, so the canvas is bare while you are reading it and the dots fade in
while something is being placed: a window carried, a window resized, or the
canvas zoomed -- zoom because it is what decides how fine the lattice is.
Panning is none of those and leaves the canvas alone. They linger for half a
second afterwards, since a resize is a run of separate wheel clicks and a grid
that came and went between them would be a flicker, and they fade rather than
blink, because the dots appearing is information rather than an event.

**There is one dot per line of type.** The spacing is not an arbitrary lattice
laid over the canvas: it is the height of a line at the default size, measured
from the font at startup rather than written down. So the grid is ruled paper,
and a box put down on it has its lines sitting in the gaps between rows of dots
rather than straddling them.

That last part needs one more thing: **it is the text that lands on the grid,
not the card's corner.** A box's padding is not a whole number of anything, so
snapping the corner would leave the lines wherever the padding happened to put
them. Snapping the first character instead makes the alignment true, and the
corner lands half a padding off a dot, which nobody can see.

**Every window sits on the grid, and boxes never overlap.** A window's top-left
corner snaps to the nearest dot of the grid *as drawn* -- the text's corner, for
a box -- so placement gets finer as you zoom in. That holds for boxes, number lines, plots and
pictures alike (`SnapToGrid`, in `grid.*`, next to the code that draws the
dots). Boxes
additionally never overlap: one put down on top of another steps clear along
whichever edge is nearest, a whole dot at a time, and the number lines and plots
docked under a box count as part of it. Every box placement goes through one
function (`App::PlaceBlock`), so dragging, typing a new box and loading a file
cannot disagree about the rules.

**The camera opens one notch in.** `kDefaultZoom` is a single wheel step above
unity, which is where a box of the default type size is comfortable to read
rather than merely legible. Slides still define themselves at zoom 1, since a
slide is a screenful at its own scale and that is a different question.

**The pointer says what the canvas is doing.** While the right button is down
and the canvas is being dragged, the cursor becomes a **closed hand**: the
gesture is holding the paper and moving it, and an arrow does not say that. It
is drawn rather than chosen, because there is no grabbing-hand cursor in the set
the window system offers -- GLFW's "hand" is the pointing one -- so the system
cursor is hidden and ours takes its place, in two passes so it reads on the pale
canvas and on a dark card alike. Losing the window counts as letting go, so the
cursor can never be left hidden underneath somebody else's app.

**A drag in flight owns the pointer.** Hovering a rim claims the pointer, so
without this rule letting go of one window over another window's rim would never
deliver the release, and the dragged window would stay stuck to the cursor.

A **lasso** is the case this is easiest to get wrong, because a lasso is meant
to sweep across other windows: without the rule, a loop drawn round a box was
cancelled the instant it crossed that box's edge, and the stroke simply stopped
being drawn. Reaching for a *selected* window's rim belongs here too -- the rim
would otherwise claim the press and carry that one window, when a selection is
supposed to move as one. Only the menu opening abandons a gesture; something
else taking the pointer never does.

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
does: a ring you can drag, a filled dot you cannot. And a **blink** says *this
one, now*: a visualization in use flashes white, half a second on and half a
second off. Three questions, three channels -- and a blink because colour is
already spoken for, and because on a shared axis several markers wear colours
from the same map. A hard switch rather than a fade, since a throb reads as
decoration where a blink reads as a signal.

**What lights up is the one thing under the pointer.** Pointing at a number line
lights up a single value: the marker nearest the pointer, within grabbing
distance. Not the row's whole contents -- that would be a claim about every
value on it when the pointer is on one -- and nothing at all over the bare axis,
where a press makes a *new* value rather than taking hold of an old one.

A **derived** marker lights up too, and that is worth saying plainly because it
is not the same question as whether you could drag it. Being read-only means a
press will not move it; it does not mean pointing at it says nothing. Which
thing in the code a dot stands for, and where that thing is written, is exactly
what someone pointing at it wants to know. So the two questions are asked
separately: *what is under the pointer* lights up, and *what a press would take
hold of* is the nearest **free** marker. One search answers both, differing by a
single flag, so they cannot drift apart.

Carrying a row by its rim is the other case, and there the whole row does light
up: you are holding the window and everything on it. A plot has no handles and
nothing finer to point at than the window itself, so hovering one lights every
curve on it -- the granularity follows what the window has to offer.

**And it works from the other end.** Highlight a mention of a variable in the
text and the same things light up: the name wherever else it is written, the
wires converging, and the viz blinking at the far end. Taking hold of a name in
the code and taking hold of its window are the same statement made from
opposite sides, so they had better look the same -- and it is the same gesture
that pulls a variable out of the code onto an axis of its own, read as a
question rather than as a drag.

**Using a window boxes the name.** Hover or grab a number line -- and, in time,
any other visualization -- and the *mention* of its variable lights up in the
box it came from, in that variable's colour, wherever it is written: in the
LaTeX or in the code, whichever that line is currently showing, and on every
line that mentions it rather than only its declaration.

**And the value is joined to what it lights up**, by a curve in the same
colour, drawn in the overlay pass so it runs over every window rather than being
chopped in half by one. The box says *that* this is the same thing; the curve
says which one it is the same thing as, which starts to matter as soon as two
windows are open at once. It leaves both ends horizontally and lands on
whichever side of the name the other end is already on, so it never crosses the
thing it is pointing at.

**A name written five times is still one variable, so the wires converge.**
They gather at a junction just outside the card, on the side the window is on,
level with the middle of the mentions -- and one wire runs from there to the
window. The strands arrive at the junction along the same horizontal tangent, so
near it they lie on top of one another and the bundle reads as a single cable
fanning out. Five separate cables leaving the same handle would say there were
five things to point at, when the whole claim being made is that there is one.
A name written once needs no junction and gets none.

**The curve leaves the visualization, not the pointer.** The handle on a number
line *is* the variable; the pointer is only where your hand happens to be. That
matters now that a box's reals share one axis: three wires all setting off from
the cursor would be a knot, where three wires each leaving their own marker are
three separate statements. A plot's wire leaves the middle of the plot, since a
curve is a whole function and has no one place it is.

The exception is a **shader's picture**, which reports the pointer -- and for
exactly the same reason. There the pointer is not where your hand is, it is the
point the shader is being read at: it *is* the value.

**The pointer over a shader's picture is `fragTexCoord`.** Not a metaphor: that
is the value the shader is being evaluated at, and it is the value the probe
reads the number lines from. So it is reported as the thing under the cursor
like any variable, its name lights up wherever the box writes it, and the curve
runs from the cursor to it. A box mentions it without declaring it, so it takes
the next colour along from the box's own variables.

Finding the mention is easy in code, where the font is monospaced and a name is
a column. In LaTeX it needs help, and the way it gets it is worth knowing: when
`mathline` writes the LaTeX it wraps every identifier in `\sbref{k}{...}`, a
command that typesets as exactly its argument and does nothing else. The tag
rides through parsing onto the node, and through layout onto every item the node
produced. So the renderer can ask "where did the third identifier end up?" and
get an answer in pixels, without the typesetter knowing anything about
variables.

### What a window is showing

A variable is **mutable unless it is `const`** -- in C++ and in GLSL alike -- so
it can hold one value on one line and a different one three lines further down.
A window standing for it therefore has to answer *when*, and there are two
answers depending on whether the box is being read or being written.

**With no caret in the box, a window shows the value at the end of the block.**
Not a convention picked for convenience: it falls out of how the program is
assembled, since every value is printed after the last of the user's lines. A
block is one whole statement, and its windows report what it came to.

**With a caret in the box, only the variables being written about have
windows.** The line the caret is on names some variables; those keep their
windows, and every other window belonging to that box stands down. Highlight a
stretch instead and it is every variable the highlight *touches* -- the same
test that decides what dragging text out of a box takes hold of, so one letter
of a name is enough.

The reason is the mutability above. With the caret on line three, the number a
window would be showing is the one from the end of line seven, which is not what
line three is about -- and a number that looks like an answer to the question
you are asking, but is an answer to a different one, is worse than no number.

A window that stands down **keeps its place** rather than closing the gap, so
nothing shuffles about as the caret moves, and it takes no pointer while it is
not drawn. Two exceptions. A **shader's picture** stands for a whole box rather
than for one variable, so it stays: hiding it while its shader is being typed
would be exactly backwards. And an axis **shared between two boxes** only stands
down the markers belonging to the box with the caret, since the other box is not
the one being edited.

Known edge: a `float a;` assigned over further down now looks like a knob that
does nothing, because its window reports the end of the block, where the
assignment has already replaced what the knob chose. That is honest rather than
broken, and it is the first argument for `const` meaning something here.

### Display windows and vizes

Two words, because the difference between them is the whole of where this is
going.

A **dw** -- a *display window* -- is a window that shows values on a scale of
its own. The number line and the plot are the two there are. A dw knows about
scales, panning, zooming, framing, merging, docking and being carried; it knows
nothing whatever about what a real or a function looks like.

A **Viz** is how *one type* is drawn inside a dw. `RealViz` is how a real looks
on a number line, `RealFunctionViz` is how a function looks on a plot,
`ScalarBivectorViz` is one of the ones still to come -- and a **shader box's
picture is one as well**, with a full-screen quad for its mesh and all its work
in a fragment shader. Saying so is what makes the shape general: a viz is a mesh
plus a pair of shaders, and the kinds we have differ only in which of the three
is carrying the weight. The names are already the
file names in `types/`; what is new is insisting on the distinction, because a
dw is the app's and **a viz is meant to be the user's**.

A viz answers four questions, and every one of them is code somebody wrote:

| | |
| --- | --- |
| **mesh** | its geometry, in its own units |
| **place** | where each of those vertices ends up, given the value |
| **distance** | how near the pointer counts as being on it |
| **drag** | what value the pointer is now asking for |

The canvas can now answer the first two, for both types. The last two are still
the app's: `AxisPanel::GrabbableAt` is the third and the `Drag::Handle` case of
`AxisPanel::HandleInput` is the fourth, and a function has neither, which is why
a curve cannot be dragged.

### Writing a viz inside the app

**The first two questions can now be answered on the canvas.** A box whose first
line is `Viz(float);` is a **viz box**: it says how a value of that type is
drawn rather than declaring a value of its own. It has no variables and grows no
number lines.

There are two of them per viz, paired by the type they name, and which is which
needs no new rule at all -- a box is already C++ first and GLSL if C++ refuses
it, and the two halves are written in different languages.

**The C++ half turns a value into geometry**, and it runs *inside the program
that has the value*. That is forced rather than chosen: a `std::function` cannot
leave the process that made it, so the only place a function can be sampled is
the program it was built in. So a viz's lines are injected there, once per value
of its type, with `value` bound to it -- which turns out to be the good answer
rather than merely the possible one:

- **sampling and building the mesh become the same editable snippet.** There is
  no separate "how is this type sampled" to write and keep in step.
- **it costs nothing.** That program was going to be compiled and run anyway;
  the vertices ride back on the same run as the values. Nothing extra is
  launched when a knob moves.

Two ways of laying geometry down, and no more:

| | |
| --- | --- |
| `vertex(x, y)` | three calls make a triangle |
| `line(ax, ay, bx, by)` | one segment of a path |

A path is not a ribbon of triangles, because its width has to be constant on
screen while the mesh is in value units, which the placing shader is free to
stretch by different amounts along each axis. Two primitives is the smaller
answer than one primitive plus a per-vertex offset.

**The GLSL half places it**, and it is the *vertex* shader. It is handed `p`,
the vertex in the viz's own units, and is expected to leave it somewhere in
screen pixels. The window tells it `value` (the number being drawn, for a real),
`sbArea` (the rectangle it draws in, its middle at the view's centre), `sbView`
(what that rectangle spans, as centre and extent) and `sbScale` (how big chrome
is drawn at this zoom). And it is given `sbMap`, which takes a point in value
space to a point on screen -- provided rather than left to be written out,
because every viz needs it and a place box should be one line.

It is:

```
Viz(float);                                Viz(RealFunction);
p = sbMap(vec2(value, 0.0)) + p*sbScale;   p = sbMap(p);
```

for the marker on a number line and the curve on a plot respectively. The mesh
halves are the ring, and the sampling:

```
Viz(float);                                Viz(RealFunction);
for (int i = 0; i < 20; ++i) {             float px = value(-8.0f), py = -8.0f;
  float s = 6.2832f * i / 20.0f;           for (int i = 1; i < 129; ++i) {
  float e = 6.2832f * (i + 1) / 20.0f;       float y = -8.0f + 16.0f*i/128.0f;
  vertex(7*cos(s), 7*sin(s));                float x = value(y);
  ...                                        line(px, py, x, y);
}                                            px = x; py = y;
                                           }
```

`C:\sketches\viz.sketch` is exactly that, both of them, and editing any of the
four boxes changes what is drawn on the next rebuild.

**A viz box is compiled standing alone as well**, with `value` an example of its
type. That is what tells the two halves apart -- the mesh half is the one C++
will have -- and it means a mistake in a viz is reported on the viz's own lines
rather than surfacing in somebody else's box. Its mesh stands in until a real
value has one.

**A viz box *starts* as code rather than as mathematics.** There is no equation
reading of a mesh or of where to put one, so that is the useful side to open on.
It is only a default: pressing the toggle reads it as mathematics like anything
else, line by line, and the lines with no reading stay code.

The general rule, which this is a case of: **every code box has the math/code
toggle**, and only a box beginning `\text` does not -- there the box is words,
and what it toggles instead is whether you are shown its source. What differs
between kinds of code box is which side the toggle starts on, never whether it
is there.

**A type with no viz boxes keeps the drawing the app was born with.** Nothing has
to be authored for a canvas to work -- authoring one is how you take it over. So
the built-in ring, dot and polyline are still there, and still the fallback.

**Winding does not decide whether you can see it.** raylib culls back faces, and
screen space turns the y axis upside down, so a ring written the way anyone would
write one -- anticlockwise, in the coordinates it was thought in -- comes out
clockwise and vanishes. A flat shape has no back face, so culling is simply off
while a viz draws. This cost an hour to find and is exactly the kind of thing an
author must never have to know.

The mesh goes straight into raylib's own batch, so a viz is drawn through the
same path as everything else on the canvas and gets `mvp` filled in for free.

**A shader box's picture is a viz too.** That is worth saying because it is the
observation that makes the shape general: there the mesh is a single full-screen
quad, the vertex half is trivial, and all the work is in a *fragment* shader.
So a viz is a mesh plus a pair of shaders, and the kinds we have differ only in
which of the three is doing the work -- geometry for a marker, a path for a
curve, a fragment for a field. Unifying them is not done, but the vocabulary now
says what unifying them would mean.

`vdw2` is where the elaborate ones live -- `BiViz22.js`, `TriPViz.js`,
`ScalarFieldViz.js` and the rest -- and its shape is the one to grow towards:
there a viz is `{geometries, transform, getDist, onDrag}`, which is the same four
questions under different names.

### What a viz still cannot say

- **Distance and drag are still the app's.** A viz box cannot yet say how near
  counts as near, or what a drag means. Both are small, and both want the same
  treatment: a third and fourth box, or two more halves of the same one.
- **A viz cannot say what it needs from the value.** A real's marker does not
  depend on its number at all, yet its lines are re-run for every value; a curve
  genuinely does. Nothing distinguishes them, so nothing skips the work.
- **One viz per type.** Two viz boxes claiming the same type is not an error and
  not a choice -- the first mesh half and the first shader half win.

### Slides

A slide is not a document that lives somewhere else. It is a **region of this
canvas with a permanent rim round it** -- a piece of the same infinite space,
declared worth looking at on its own. Clicking that rim glides the camera until
the slide fills the window, so **presenting is navigating**: the same canvas,
framed. Nothing is exported, nothing is a copy, and a live plot on a slide is
the same live plot it was before.

A slide's size comes from the camera's own default. Its height is one screenful
at zoom 1, and its width that times the aspect the deck is being made for, so a
slide seen at its own scale is exactly a screenful. Arriving at one **covers**
the window rather than fitting inside it, which is what puts the rim just off
screen; if the window is not the deck's shape, the sides are what get lost.

**A deck runs left to right**, the way a sentence does and the way every deck
anyone has ever clicked through does. It is a row of places you can scroll along
and zoom out of, rather than a list beside the work.

The **new slide button lives on the canvas**, not in a toolbar, and it stands at
the **end of the deck** -- in the gap just past the last slide, rather than out
in the middle of where the next one is going to be. That is what makes it read
as the place the deck stops. Clicking it puts a slide clear of the last one,
with the same gap between them that every pair of slides has, and walks the
button along to the new end; so the next click lands the next slide further
right again, and the button is always where the deck runs out. The very first
slide, when there is no deck yet, is made where the button already is.

**And the arrow keys go through it.** Once the camera is framing a slide, left
and right flick to the one before or after -- instantly, because going through a
deck one slide at a time is not a journey and easing into each one would make it
into several. Off a slide the arrows mean nothing yet, and inside a box they are
the caret's. Which slide follows which is decided by nothing but **where they
sit**: the order you can see is the order there is.

**And the camera goes to look at it**, on the same glide a click on the rim
would have given. Making a slide is a way of saying "I am working here now", so
the canvas takes you there rather than leaving you where you happened to be
standing when you asked for it.

**A slide owns nothing but its rim.** Its title and its body are ordinary text
boxes, put on the canvas where the slide would have drawn them and thereafter
belonging to the canvas: they move, they are edited, they resize, they are
thrown away on the drop target, exactly like every other window. That is the
whole of what a slide is -- a frame declaring a region worth looking at -- and
it is why nothing has to be exported: what is on the slide is what was already
there.

**And a slide does not move.** It is not a container and it does not travel: it
is a *place*, fixed in the canvas, and the deck is the row of places the button
walked along. Clicking one does the only thing it does, which is take the
camera there. Nothing on a slide belongs to it, so there is nothing for a slide
to carry -- and a thing that stays still is a thing you can navigate by.

**A slide's text is put where the slide says, and everything else moves.** This
is the one placement on the canvas that does not step aside: a title that moved
to clear a box would no longer be on its slide, and since the body runs the full
width of the slide, the nearest way clear of a box in the middle of the canvas
is straight up off the top of it. So the words go where they belong and whatever
was already there is what steps clear. The body is put under the title by the
height the title actually turned out to be, rather than by a guess, since
snapping can move either of them by half a dot.

**Throwing a slide away.** Pointing at a slide's rim brings up a button in that
slide's **bottom-right corner**, just inside the rim's grab band so the two
never want the same pixel. It says *delete slide*: this is the one thing on the
canvas with no gesture to learn and nothing to undo it with, so it is spelled
out rather than drawn. Going to the button keeps the offer open, since reaching
for it would otherwise take it away.

Like every other piece of chrome it is a fixed number of screen pixels at any
zoom -- and a slide too small on screen to hold it is simply not offered one,
rather than being buried under its own button.

Clicking it takes the slide **and everything on it**. What counts as "on it" is
decided by each window's middle, which is exactly how the lasso decides what it
caught -- so a caption half over an edge belongs to whichever slide holds its
middle, and there is one answer to that question rather than two. The deck then
**closes the gap**: every slide to the right comes back by one slide and one
gap, and so does everything standing on them, and so does the new-slide button.
A deck is a row, and a row has no holes in it.

The aspect is a deck-wide setting, chosen from the Escape menu as one of the few
shapes a deck is ever made for. It starts at **3:2**, which is also the shape
the app window opens at -- the same number in both places, so that a slide seen
at its own scale is exactly a fresh screenful of it and the cover zoom comes out
at 1.

### Windows, and closing them

Everything on the canvas is a **window**: a code box, a box of prose, a number
line, a plot, a shader's picture, a photograph, a video. All of them are carried
by the rim around their edge, all land on the grid, all are resized by the
wheel, and all are thrown away the same way.

**Carrying a window brings up a cross at the foot of the app window.** Drag onto
it and the window is *absorbed* -- it stops being drawn, and the cross swells to
say so. Pull off and nothing has happened. Let go while it is absorbed and the
window is gone. There is no close button on anything: one gesture, every window,
and it is the tablet gesture rather than something to be learnt.

**A lasso selects.** Left-dragging the background draws a loop, and everything
whose middle it went round is selected -- the middle rather than any overlap, so
what is caught is exactly what was circled. Selection is shown by the rim, the
same rim that appears when a window is pointed at: there is no second visual
language for "this one".

**A left click anywhere puts the selection down.** On a window, on a picture,
on the bare canvas -- it makes no difference, because a click is a new intent
and whatever was selected is not it. There are exactly two exceptions: the press
that picks the selection up to carry it, and a gesture already under way. The
rule lives in one place rather than each kind of window having an opinion about
it.

**The loop closes with a curve, not a chord.** The gap between where the pointer
is and where the stroke began is bridged by a cubic Bezier, and the four points
that shape it are the four the stroke itself gives: the last two samples say
which way it is heading, the first two say which way it left. So the closure
continues the gesture instead of cutting across it, and a lasso drawn as a hook
or a spiral encloses what it looks like it encloses. The closed loop -- samples
plus curve -- is the shape that both the drawing and the hit test use, so what
is caught is exactly what you watched yourself circle.

A selection then behaves as **one window**. It moves together, by a whole number
of grid steps applied to the group as one vector, so the arrangement inside it
survives the move exactly and everything stays on the lattice; it can be carried
onto the drop target together; and Delete throws it all away, with the same
consequences closing each window by itself would have had.

**Ctrl+C, Ctrl+X and Ctrl+V take the boxes only.** A number line without the box
whose variable it stands for would be a window about nothing, so the clipboard
holds boxes -- their text, their size, and the values their knobs were at. The
copies get their own windows when they compile, which is what was wanted anyway.
They paste where the pointer is, and arrive selected, so a paste can be put
where it belongs without hunting for it.

**Closing is remembered.** Otherwise the next Sync would put the window straight
back. What is closed is kept on the box, since it is a fact about one of its
variables, and it is saved with the canvas.

**Closing a control writes its value into the code.** A number line for a free
variable is not a view, it is the thing choosing the value -- so throwing it
away has to leave that value somewhere. The initialiser that was only ever
injected becomes the user's own text: `float a;` becomes `float a = 0.4;`, and
the box rebuilds. What was temporary is now permanent, which is exactly what
losing its knob should mean. Undo puts it back.

**And taking the literal back out brings the knob back.** Closing a knob is what
wrote its value into the code, so editing that value away is asking for the knob
again -- a hidden variable that has become free once more is un-hidden. The
invariant is simply that a free variable always has somewhere to be moved from.
A closed window on a *derived* variable stays closed, since nothing about it has
changed.

Nothing else has that consequence. A plot, a shader's picture and a derived
variable's marker are all views: closing one takes away the view and nothing
else. The rule is worth stating plainly -- **a window that controls something
bakes what it controlled; a window that only shows something does not.**

### Words

Not everything on this canvas is mathematics. A slide has a title; a diagram
has a caption; a page has a sentence saying what it is for. So a box can be
**prose** instead of code, and the switch is one thing you write:

**`\text` at the start of a box makes it a text box.** That sigil is the whole
mechanism. It is not a mode, a menu or a second kind of object -- it is a
character sequence in the box's own first line, so deleting it turns the box
back into code. The rule that already governs everything else here governs this
too: what you can do you can undo by editing the same text back.

The prose is set the way `\text` is always set -- upright roman, spaced as words
rather than as a product of letters -- and it wears **no card**. A caption is
ink on the canvas, not a window sitting on it, which is exactly what a slide's
text already looked like before it became a window.

**A text box is written in as words.** Not as its source: clicking one puts the
caret between two letters of the typeset line, typing puts characters there, and
nothing about the box changes appearance. What you are writing is what it is
going to be, which is the whole reason to have prose on this canvas at all --
and it is the one place the "editing a box shows you its code" rule is set
aside, because here the code *is* the words.

That leaves the sigil with nowhere to be seen, so the strip carries the same
control a code box carries: **a toggle that shows the source**. Pressing it
gives the raw line, sigil and all, which is how a text box stops being one. It
sits with the rest of the strip rather than at the box's right edge, because
showing an unwrapped paragraph makes the box very wide and a toggle that leaves
the screen when pressed cannot be pressed again.

**It stays where it is put.** A box can be left reading as its own source, and
it goes on doing so after the caret has gone elsewhere -- it is saved with the
canvas like any other property of the box. What disappears when the box stops
being pointed at is the *button*, not the state it set. That is the general
shape of chrome here: the control comes and goes, what it chose does not.

The caret is a byte offset in the source either way, so it survives the toggle:
switch to the source and it is still between the same two letters.

**Boldness, italics and justification belong to the box, not to the text.**
There is no markup to learn, nothing to get half-applied to a phrase, and
nothing to escape. The controls sit in a strip **above** the box, and they
appear when you go looking for them -- on the box, on its rim, or in the strip
itself -- which is how a rim appears too.

**A paragraph has a measure.** Prose is a shape, not a list of lines, so a text
box is given a width and the words wrap to it: one line of source becomes as
many rows as it takes. The measure is set by a bar just inside the right edge --
it *is* the right margin -- inset clear of the rim's grab band, so setting it
and carrying the box never want the same pixel. A box that has never been given
one takes twenty ems, which is about sixty characters, which is the width prose
has been set at for five hundred years.

The measure is what the box is as wide as, whether or not any row reaches it.
Sizing a paragraph to its unwrapped source instead would make a box thousands of
units wide to hold four lines of words.

Two things follow, and both are worth stating. Wrapping means **a row is not a
line**: the box's geometry is a list of rows, each naming the line and the slice
of it that it draws, and the slices of a line partition it so every caret
position is in exactly one row. And the arrow keys move by **row**, not by line,
since a wrapped paragraph is one line and many rows.

Known edge: the KaTeX cut of Computer Modern has no bold-italic face, so asking
for both gives bold.

### Resizing

The wheel over a window resizes it, **uniformly**: one number applied to both
dimensions, so nothing is ever stretched out of shape. For a box of type or
prose that number is the size of the type; for a picture it is how big the
picture is. A trackpad pinch arrives as a wheel, so it does the same.

Two rules make it a handle rather than a slider:

**The nearest corner stays where it is.** Whichever corner you are pointing at
is the one that does not move, so the window grows away from your hand rather
than out from under it. A paragraph's measure scales with its type, or resizing
it would reflow the words instead of making them bigger.

**It snaps to its neighbours.** Every nearby window offers two candidate sizes,
its height and its width, and if one of them is close to what the wheel asked
for it is taken exactly. Because the resize is uniform, matching either
dimension fixes the scale -- so "as tall as that one" and "as wide as that one"
are the same question asked twice, and one rule answers both. It is how two
pictures come to be exactly the same height, and nothing else on this canvas
can say that.

Code boxes resize but do not snap: "the same size as that one" says something
about two pictures side by side and nothing at all about two programs. They are
still offered as targets, so a caption can match the box it sits under.

### Pictures and video

Neither is mathematics, so neither lives in `types/` -- an image is furniture,
something to point at while explaining. But both are **windows**, and that is
nearly the whole design: carried by the rim, landing on the grid, caught by the
lasso, thrown away on the drop target, resized by the wheel. Files arrive by
being dropped on the window, or from the Escape menu for a tablet with nothing
to drop with. A `.sketch` dropped the same way opens.

Two things are theirs alone:

**A corner crops.** Dragging a corner handle moves that corner of the frame and
leaves the picture where it is, so what changes is how much of it you can see --
not how big it is. Resizing is the wheel's job and cropping is the corner's, and
keeping the two apart is what makes either predictable. The handles sit just
inside the corners, clear of the rim's grab band, so the two gestures never
compete for the same pixel -- and clear of a video's transport, since a handle
you cannot reach is not a handle.

**A video has a transport**: play, a bar, and two markers saying where the clip
starts and stops, each carrying its exact time as a string you can double-click
and type into. "About there" is not good enough when the point of a clip is one
particular second of it. Playback loops between the markers, because a clip
chosen for one moment should keep showing that moment rather than stopping on a
black frame.

**There is no decoder here and there is not going to be one.** ffmpeg already
answers "what formats does this open", and the app already knows how to run a
child process and read what it says -- that is how a code box is compiled. A
video is the same arrangement with far more output: `ffmpeg` is asked for raw
frames and they are read off a pipe as they arrive, on a worker thread, into a
**bounded** queue. Nothing is decoded to disk first and nothing is held in
memory but the few frames not yet shown. Seeking restarts the child, which is
why scrubbing is a little coarse and playing straight through is not, and why a
scrub asks for a seek every so often rather than once a frame.

Like the rest of the toolchain layer, `video.*` knows nothing about raylib: it
hands back bytes, and whoever wants a texture out of them owns the GPU.

### Number lines

**A box gets one, and all of its reals share it.** Not one axis per variable: a
stack of one-value axes says nothing about how those values compare, and the
values in a box are read together. So they go on one scale, and that scale
**frames itself** the first time everything on it has a value, so that every one
of them is in view. Free variables get a draggable handle, derived ones a
read-only marker that moves when its inputs do; they are told apart by shape,
and told from each other by colour, exactly as before.

**A variable earns an axis of its own by being pulled out of the code.** Select
part of where it is written -- any part of any mention of it -- and drag it out
of the box. It comes away as an axis carrying just that variable, following the
pointer, and is dropped wherever the button comes up. That is the same drop
every carried row gets, so it can equally be dropped onto another axis to merge,
or onto the cross to be thrown away.

The gesture is worth stating as a rule, because it will be wanted for every kind
of window eventually: **the code is where things come from.** A window is not
asked for from a menu; it is pulled off the name of the thing it is about, and
it lands where you let go.

**And a variable is made by putting one down on the axis.** Press the bare part
of an axis -- anywhere but on one of its markers -- and a new variable appears
there, in the code of the box that axis belongs to. Pointing at that spot first
says where: the box blinks a caret at the line the declaration would land on,
which is what makes the gesture aimable rather than a surprise.

So the two gestures are each other's inverse. A variable is pulled *out* of the
code onto the canvas; a variable is pushed *into* the code from the canvas.
Neither the text nor the windows is the place things are made -- they are two
views of one program and you can work in either.

**Making one and moving one are the same gesture.** The press that creates it
carries straight on into holding its marker, so from the first frame it behaves
exactly as though the marker had been there all along and you had grabbed it.
That is what decides the declaration: it is `float d;`, **free**, not a literal.
A value you have just put down is one you are about to move, and only a free
variable has a handle to move it by -- a number written into the source could
only be dragged by rebuilding the box every frame. So the number goes where
every free variable's number goes, into the knob, and closing the axis is what
would write it down. The name is the first letter nothing in the box is using.

**Where it lands is where the caret was left**, or the end of the box if the
caret has never been put anywhere. A blank line is written on rather than pushed
down, since a blank line is somewhere to write. That the caret shows even while
the box is reading as mathematics is the point of it: what is being pointed at
is a *line*, not a place between two letters, so on a typeset row the caret sits
at the end of the statement rather than pretending to a column that reading does
not have.

**And it joins the axis it was put on**, at the scale it was put at, rather than
the axis its box would otherwise have given it. It is in view by construction,
so that axis does not reframe -- reframing would move every other value aside to
make room for one that needed none.

**Which is why an axis scrolls on the right button.** The left button is now
spoken for all the way along a number line -- a marker to grab, or a place to
put a new one -- so there is nothing left for it to pan with, and the gesture
that moves everything else on the canvas is the one it takes instead. A number
line is the one thing on the canvas that claims the right button, so it is the
one thing the canvas has to be told about.

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
| `Clifford.*` | a whole family of types: it writes out the structs and products of Cl(p,q,r) on demand |
| `RealFunctionViz.*` | how one is shown and how an example of one is made -- the *viz*, plus the plot it is shown in |
| `RealViz.*` | the same two things for a real: the number line (a *dw*) and the marker on it (a *viz*) |

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

### Clifford algebras

The third type, and the one the tool is most wanted for. It is the C++ answer to
**GAmphetamine.js**, and it works the way GAmphetamine does: the algebra is not
a library that gets included, it is **generated** -- concrete structs and
concrete products between them, fully unrolled, no templates and no runtime
dispatch. That is the same bargain everything else here makes, and it is why a
box of geometric algebra still compiles in the time it takes to let go of a key.

**A type is a set of grades.** Not an arbitrary set of basis blades: grades.
That one restriction is the whole trick, because the product of two grade-sets
is a grade-set -- so every product has an *exact* result type, and every result
type has a name. `Bi` is grade 2, `Even` is every even grade,
`ScalarQuad` is what it says (GAmphetamine calls that one a study). A name
is a run of grade words, so a compound type needs no declaring: write it and it
is a type. Nothing is ever widened to `Multivector` to make the types line up.

The names are the count and nothing else. The "-vector" on the end of
"bivector" says nothing that **Bi** does not, and a compound spells out every
grade it holds -- `ScalarQuad` is a name where `ScalarQuadvector` is a sentence.
They run **ascending**, because that is the order a name is written back out in
and a type has to be spelled the way it was generated: `BiTri` is one, `TriBi`
is not.

**The signature is written the way it is spoken.** `Algebra(3,0,1);` on a line
of its own says p generators squaring to +1, q to -1 and r to 0 -- and that line
is a statement *about* the box rather than one *in* it, so it leaves a blank
behind rather than being compiled, and it reads as the sentence that names the
algebra rather than as code. A box that says nothing works in Cl(3,0,1),
3D projective geometric algebra, which is what these tools are mostly for. The
generators are indexed the way every PGA text indexes them, the null ones first,
so Cl(3,0,1) has e0 null and e1 e2 e3 positive.

**Degeneracy is not a special case.** A generator that squares to zero takes
terms out of the products like any other metric, and the dual is a *complement*
rather than a multiplication by the pseudoscalar -- which is the only way it can
survive a pseudoscalar that squares to zero. That is what makes Cl(3,0,1) work
at all, and it is the reason the dual is defined by `b ^ !b = I` rather than by
a product.

The operators are GAmphetamine's, because that is what the notation already is,
and each has a reading as mathematics:

| C++ | means | written |
| --- | --- | --- |
| `a * b` | geometric product | juxtaposition, `ab` |
| `a | b` | inner product | `a \cdot b` |
| `a ^ b` | outer product | `a \wedge b` |
| `a & b` | regressive product | `a \vee b` |
| `~a` | reverse | `\widetilde{a}` |
| `!a` | dual | `a^{*}` |

**Grade selection is `grade2(M)`**, and it reads as Hestenes writes it:
`\langle M \rangle_{2}`. Written out per grade rather than as one
function taking a number, because the *type* of the answer depends on which
grade is asked for -- the grade-2 part of an even element is a bivector, and
there is no single type that could stand for all of them.

Only the grades a type actually has get a selector, so asking an `Even` for its
grade 3 does not compile. That it would be zero is beside the point: it is a
mistake, and it is one the type system can see.

One departure from Hestenes: **the subscript is always there**, including on the
scalar part. He drops the zero, and dropping it makes the most easily missed of
the projections the one that looks like no projection at all.

**The same algebra is written for GLSL**, because a box that draws is exactly
the kind of box that wants one. GLSL has no operator overloading, so `a * b`
cannot be a geometric product there -- and that is the whole difficulty, since
a box's text is *one* text, tried as C++ and then as a shader.

The answer is GAmphetamine's own alternative spelling. Every product also exists
as a **named function**, in both languages:

| operator | function |
| --- | --- |
| `a * b` | `gp(a, b)` |
| `a \| b` | `ip(a, b)` |
| `a ^ b` | `op(a, b)` |
| `a & b` | `rp(a, b)` |
| `~a` | `rev(a)` |
| `!a` | `dual(a)` |
| `a + b`, `a - b`, `a / k` | `add`, `sub`, `div` |

They mean the same thing and they *read* the same -- `op(p, q)` sets as
`p \wedge q` exactly as `p ^ q` does. So a box written in the function form runs
either way, and a box written with operators is C++ only. That is the rule:
**operators are a convenience, functions are the portable spelling.**

Three things differ inside the generated GLSL, all forced: it has no default
member initialisers, so every function fills in every component of its answer
rather than only the ones with terms; it has no forward declaration of a struct,
so the types are emitted in an order where each is complete before it is
mentioned; and its constants are struct constructors rather than the little
lambdas C++ uses. That last one is why C++ is also given an
every-component-at-once constructor -- `Bi(0.4, 0.0, ...)` is then one spelling
that makes a value in either language, which is what the injected example value
needs to be.

C++ binds `|` looser than `^`, and `^` looser than `+`, so `a + b | c` means
`(a + b) | c`. That is a fact about C++ and not something to paper over: the
parser here follows the same precedence the compiler will, because the compiler
is what actually runs it. Write the brackets.

**Only what a box uses is written out.** The default is the geometric and inner
products for `Una`, `Bi` and `Even`; anything else a box mentions is
added, and so is anything the products between them turn out to need. That
closure is over *products* only. Closing over addition as well would ask for the
union of every pair, which is the entire lattice of grade-sets -- 30 types and
three and a half megabytes of code nobody wanted. A sum of two unlike things is
written when it is wanted: name the type and the operator appears with it, and
where the exact type was never asked for the sum widens to the nearest one that
exists rather than going missing.

**The algebra gets a name, and everything else is said in terms of it.** The
line that chooses it is not a statement in the program, it is a statement of
mathematics, and it is set as one: `Algebra(3,0,1);` reads

>   A = Cl^{3,0,1}

with a **script A**, chosen so that an ordinary variable called `A` is a
different letter and there is nothing to collide with. After that the types need
no signature of their own:

| type | reads |
| --- | --- |
| `Una`, `Bi`, `Tri` | an exterior power, `\bigwedge^{k} A` |
| `Even` | `A^{+}` |
| `Odd` | `A^{-}` |
| `Multivector` | `A` |
| `ScalarQuad` and other compounds | the direct sum of their parts |

The `+` and `-` are the Z/2 grading, which is what even and odd *are*. That
superscript says something different from the one on `\bigwedge^{1} A`,
which is why a single grade always wears its wedge rather than being shortened
to `A`.

**`A` is the algebra, and the exterior power is of the space underneath it.**
Those are two different objects: the quadratic space has dimension p+q+r, the
algebra over it has dimension 2^(p+q+r), and only the algebra has an even and an
odd half. Naming `A` as the algebra is what makes `A^{+}` mean anything; the
price is that `\bigwedge^{k} A` is then an exterior power of an algebra,
which is not literally what is wanted.

It is a standard abuse and the usual justification holds: a Clifford algebra is
isomorphic to the exterior algebra of its space *as a graded vector space*,
differing only in its product, so the grade-k part of one is the grade-k part of
the other. The alternative that would need no excuse is Hestenes' grade
projection -- which is now here as `grade2(M)` for taking a value apart, though
the *types* are still named with the wedge.

Known conventions, stated so they can be argued with: the inner product is the
symmetric one, keeping grade |r - s|; the dual is the Poincare complement; and
`a & b` is `undual(dual(a) ^ dual(b))`, which makes `^` the meet of planes and
`&` the join of points, as in ganja.

### Two languages

A box is **C++ by default**. If the C++ compiler refuses it, the same lines are
tried as **GLSL** -- because a box that says `vec2 p = fragTexCoord;` was never
trying to be C++. Nothing declares which language a box is in; the box is
whichever one will have it.

Both languages get the same treatment, which is the point: the user writes the
middle of something and the app supplies the ends. For C++ that is includes, the
argv reader and a `main` that prints every value back. For GLSL it is the
fragment-shader interface raylib expects, one uniform per knob, and a `void
main() {` ... `}` around the lines. The two bodies are assembled **line for
line** with each other -- a declaration that becomes a uniform leaves a blank
line behind rather than vanishing -- so a diagnostic from either language lands
on the same row of the box.

**A free real reaches a program through argv and a shader through a uniform.**
Either way its knob moves it without rebuilding: the C++ path re-runs the
executable, the GLSL path just draws the next frame. That is the same bargain,
and it is why dragging a knob on a shader box is instant.

**A shader reports values back too**, by rendering them instead of the picture.
A second shader is compiled from the same lines with different ends: the
varying replaced by a point given as a uniform, and the user's own write to
`finalColor` overwritten by ours, which packs the bits of one variable into the
four bytes of a pixel. It draws into a target one pixel wide per derived
variable -- `int(gl_FragCoord.x)` picks which -- and those pixels are read back.
Exact, not scaled: the four bytes of the pixel are the four bytes of the float,
so a number line shows the value and not an approximation of it.

The real difficulty was never the reporting. It is that **inside a fragment
shader a variable is a field, not a number**: `r = length(p) * k` has a
different value at every pixel, so "the value of r" is not a question until a
point has been chosen. So the point is a uniform, and pointing at the picture is
what chooses it -- hover the preview and the number lines read the field under
the cursor; look away and they read its middle. That makes the preview a way of
*reading* a shader rather than only looking at one, and it is the same idea as
lighting up a variable's name: two windows, one thing.

A read is a stall, so it only happens when something it depends on has moved --
a knob, or the pointer over the picture.

Compiling GLSL means talking to the graphics context, which `mathprogram` knows
nothing about and must keep knowing nothing about. So the program assembles the
source and stops in `State::Shading`; the host compiles it on the thread that
owns the context and calls `ShaderCompiled`. Errors come back the same way C++
errors do -- driver logs are worded differently by every vendor, and the two
shapes in the wild are parsed in `compiler::ParseGlslLog`.

**When neither language will take a box**, the failure reported is the one from
whichever language the text was trying to be, decided by whether it mentions a
word only GLSL has. Without that rule someone writing a shader would be shown a
C++ complaint about `vec2`, which is true and useless.

A GLSL box shows **what it draws**, in a window beside the card. Unlike a number
line or a plot it stands for a whole box rather than for one variable, so there
is one per shader box; in every other way it is an ordinary window, with a rim
to carry it by and the grid to land on.

Known edges: a box with no `float` declarations never reaches either compiler
(the rule that keeps ordinary notes from being built), so a shader with no knobs
does not run yet. `RealFunction` is C++ only.

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

renders as `a ∈ ℝ`, `b ∈ ℝ`, `c = (a+b)/2` (a real fraction), with one number line
docked underneath carrying all three: a ring for each free variable, a dot for
the derived one. Dragging a knob re-runs the compiled program and the derived
marker follows. Clicking that axis anywhere but on a marker writes a new
`float d = 0.1504;` into the box, at a caret the box blinks while you are
pointing -- on its LaTeX as readily as on its code. Hovering a line turns it
back into `float a = -0.778;` with the injected part in blue; clicking it hides
the injection and edits the code. A variable pulled out of the code gets an axis
of its own, axes merge by dragging one grip onto another, and each zooms on its
own wheel.

Editing a box turns the *whole* box into Monokai-coloured code, with a
math/code toggle shown only while the box has the caret. Undo is Ctrl+Z; Ctrl+left/right move by word. A line the compiler
complained about always reveals its code, so the red caret has real characters
to point at; the message sits in a white balloon in a proportional face.

Canvases save and load: Escape menu buttons, Ctrl+S (straight back to where the
canvas came from) and Ctrl+O, plus `--open <file>` on the command line. The
format is plain text, one record per line, in `App::SaveCanvas`.
`C:\sketches\demo\demo.sketch` is a deck of six slides carrying one of
everything -- reals and their knobs, functions and their plots, a shader, an
algebra, prose, a picture and a video -- and is the quickest way to see whether
a change has broken something.

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

A box whose code the C++ compiler refuses is tried as GLSL, and one that
compiles draws itself in a square beside the card, with its free reals wired to
uniforms so the knobs reshape the picture live. `C:\sketches\glsl.sketch` is an
example: `vec2 p = fragTexCoord - 0.5;` is what makes C++ give up, that line
stays code because it has no mathematical reading, and the lines around it still
read as equations. Its derived variables are read back off the GPU, at the point
in the picture the pointer is over.

Prose works: a box whose first line starts with `\text` sets its lines as words
on the bare canvas, with a strip of bold / italic / justification controls above
it and a toggle that shows the source. It is written in **as words** -- the
caret goes between two letters of the typeset line -- and the words wrap to a
measure set by a bar on the right margin. Slides make two of these, a bold title
and a body, sized to the slide, and own nothing else at all. An older canvas
that kept its slide text on the slide record is turned into the same pair of
boxes on the way in.

Pictures and video are windows. Dropping a file on the app window puts one where
the pointer is; the Escape menu does the same for a tablet. A corner crops, the
rim carries, and a video has a play button, a bar and two typed-in trim times,
with the frames pulled off an `ffmpeg` pipe on a worker thread. The wheel
resizes any window uniformly about the corner nearest the pointer, and prose and
pictures snap to the width or height of what is near them.

The canvas pans on the **right button** and zooms on the wheel; one finger pans
and two pinch. The left button belongs to the contents of the canvas -- a click
places the caret, a rim drag moves a window, a handle drag chooses a value, a
press on a number line puts a new value there -- and never moves the canvas
itself. Panning therefore works wherever the pointer happens to be, even over a
window that has claimed the wheel for its own scale, with one exception: a
number line scrolls its own scale on the right button, because its left button
is entirely spoken for. That is the only window that takes it, and it says so,
so the canvas can leave its own pan alone for as long as the scroll lasts. The
wheel belongs to whatever window it is over, since that is what resizes it.

A viz can now be written on the canvas itself. Two boxes saying `Viz(float);`
-- one C++ laying down a mesh with `vertex(x, y)`, one GLSL placing its vertices
-- are what draws the markers on the number line, and editing either changes
them on the next rebuild. `C:\sketches\viz.sketch` is that, working. A type
with no viz boxes keeps the drawing the app was born with.

Key files: `mathline.*` (C++ subset -> LaTeX), `mathprogram.*` (assemble /
compile / re-run, and the scaffolding a fragment is wrapped in), `types/` (one
directory per supported type: what it is, how to make an example of it, how to
draw it), `blocks.*` (per-line rendering and editing, code and prose alike),
`media.*` (pictures and video as windows), `video.*` (the ffmpeg pipe, raylib
free), `resize.*` (the wheel-resize rule and its snapping), `highlight.*`
(Monokai), `viz.*` (the vocabulary above, and the viz boxes the canvas
defines), `filedialog_win32.*` (open/save dialogs).

`bool testFunc()` with its red/blue square still works, but only as a fallback
for a box that declares no variables. It is scaffolding from before this
direction existed and can go whenever it stops being useful.

## Open questions

Kept here so they are not lost between sessions. Answered ones move up into the
sections above.
- A paragraph wraps to a measure, but nothing yet lets one **flow into the
  next** -- a body that outgrows its slide simply runs off the bottom of it.
  Nothing warns, either.

- **A multivector has no window.** It compiles and it runs, but nothing shows
  one: no number line, no picture, and so nothing reported back from the
  program either. That is the next thing, and it is the whole reason for the
  rest of it -- a bivector in Cl(3,0,1) is a line in space, and a motor is a
  screw motion, and both of those are things one wants to *see*.

- A multivector has no knob for the same reason a function has none: its chosen
  value is written into the source rather than sent down argv, so changing one
  is a rebuild. Several floats down argv would fix it.

- **`Algebra(3,0,1);` is a guess at the syntax.** It reads well, it parses, and
  now that it is set as `A = R^{3,0,1}` it no longer *looks* like a statement in
  the program -- but it is still C++ that is silently not compiled, which is a
  small lie. The alternative floated was to hang the signature on the type
  names, `Una301` and `Bi301`, which would make the algebra a property of the
  thing rather than of the box and would let two algebras share one canvas. A
  deck of slides about one algebra would rather say it once, though.

- **A GLSL box carrying an algebra is a large shader.** Cl(3,0,1) with the
  default types is about 150KB of generated GLSL, and shaders are compiled on
  the thread that owns the graphics context -- there is nowhere else to compile
  them. It has not been a problem yet (a box with one costs about a third of a
  second more than one without), but it is the thing that would start to hurt
  first, and the fix if it does is to emit only the products a box actually
  calls rather than every product between the types it mentions.

- **A multivector shows no injected value.** A real shows its number and a
  function shows its lambda, but a multivector of Cl(3,0,1) has sixteen
  components, and a box sized for the worst case across both readings would be
  wider than the screen even with the injection hidden. So nothing is shown,
  which is honest but unhelpful -- the answer is a window that displays one,
  which is wanted anyway.

- **Video has no sound.** Nothing plays audio, and a clip chosen for what
  someone says in it is not much use silent.

- A picture is not part of the no-overlap rule -- boxes push each other apart,
  pictures do not. A caption sitting on a photograph is normal, so this seems
  right, but it means the two kinds of window follow different placement rules.

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

- A shader's derived values are read at one point. Would a number line be
  better as a *range* -- the least and greatest the variable takes across the
  whole picture -- with the point under the cursor marked inside it?

- A shader box needs at least one `float` to be built at all, since a box with
  no variables is left alone on purpose. What is the right rule?

- **Nothing brings a closed window back.** A variable whose number line has been
  thrown away is still in the code and still has a value; there is just no way
  to ask for its window again. The escape menu could offer it, or the name in
  the equation could be clickable.
- Undo is per box and is lost when a box is deleted; there is no canvas-level
  undo. Is that enough?
- Nothing warns before discarding unsaved work on load or on close.
