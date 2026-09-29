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

**Building while the app is open works.** Windows will not overwrite or delete a
running executable, but it will rename one, so a step before the link moves the
old `sketchbook.exe` aside (`cmake/MoveAsideRunning.cmake`) and the new one is
written under the usual name. The running copy carries on; launching again gets
the new build. A moved-aside copy is deleted by the first build after nothing is
running it. So there is one build directory, and nobody has to close anything.

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
  LaTeX <-> GLSL relationship down to something that can be written in a few
  paragraphs. When a decision is made, record it here and delete whatever it
  supersedes.

## Standing constraints

### A value never recompiles

**Moving a value never rebuilds anything.** This comes first because it is what
the program is for: a knob is how you find out what a statement means by pushing
on it, and pushing only teaches you anything while the picture answers at display
rate. A compile is not a small cost to be tidied later -- `mum grids.sketch`
measured **205 ms** for one, and the driver's shader cache only saves that when
it has seen the exact same source before, which a changed number never is.

So **every number a variable is given is a uniform, at all times**: the one the
app injects for `float a;`, one typed into a declaration, `float a = 0.4;`, and
one typed into an assignment further down, `a = 0.4;` -- at the top of a box or
inside one of its functions. The shader then does not contain the number at all,
which is what lets a knob move it, the play button swing it, and *typing* a new
one take effect, all without the shader changing.

Two things follow, and a new way of choosing a value has to respect both:

- **A drag writes the number into the text as it goes**, so what you read is what
  runs. That is an edit of the line and nothing more: the uniform has already
  been set. Undo gets one step per drag.
- **A rebuild that comes out the same is not compiled.** `Program::Rebuild`
  compares the shader it assembled with the one running, and if they match --
  which they do whenever only these numbers changed -- it hands the new numbers
  over as uniforms and stops there. Undoing a drag goes through this too: the
  undo step gives the box its old lines back rather than building a new box.

The exception is **`const`**, and it is not a choice: GLSL requires a const to be
initialised by a constant expression, and only a constant can size an array. So
`const int n = 40;` stays a number in the shader, has no knob, and changing it is
editing the program. Numbers *inside* expressions -- the `3.0` in `sin(3.0 * x)`
-- are also still compiled in; they have no window to be moved from.

**A compile shows as the busy cursor.** It runs on the thread that draws, so no
frame can say so while it lasts; the system cursor is the one thing still drawn
(`busy.*`). A compile of a few milliseconds is over before it can be seen, so in
practice it appears for the slow ones.

The checks: drag a knob and nothing compiles; type a new number into a
declaration and nothing compiles; undo either and nothing compiles.

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
- **The atlas is packed by each glyph's own rectangle**, not by rows the height
  of the size that was asked for (`BuildFont`, in `fontstore.*`). raylib's
  `LoadFontFromMemory` does the latter, which assumes no glyph is taller than
  its nominal size -- true of prose, wildly untrue of KaTeX's Size1..Size4,
  where a big brace or a `\bigwedge` is several times it. Such a glyph was
  placed at an offset the bounds check called legal and then written past the
  end of the bitmap, and read past it again when the atlas was cut up: an
  access violation in `memcpy`, on whichever rung the zoom happened to land on
  when the atlas came out half height. So the font is built here rather than by
  raylib, with the skyline packer, and a glyph that still would not fit is made
  empty rather than left as a read off the end.
- **Nothing is measured off an atlas.** A glyph's advance and the box round its
  outline come from the font's own tables (`hmtx`, `glyf`), which are exactly
  proportional to the size asked for; the atlas bitmap is only where the pixels
  come from. raylib's advances are whole pixels at the rung they were
  rasterised at, so measuring them gave a line a different width on every rung
  -- and since the zoom picks the rung, **a window changed size as the canvas
  zoomed**, by a few percent in width and, once rows were rounded to whole dots,
  by a whole dot in height. A window's size in world units must not depend on
  the zoom: render a box at several zooms and divide by the zoom to check.
- **Per-line LaTeX layout is cached** and rebuilt only when that line's text or
  the em size actually changes (`CodeBlock::EnsureCache`).
- **Moving a value costs a uniform, not a build** (*A value never recompiles*).
  A shader is compiled on the thread that draws, because that thread owns the
  graphics context, so what keeps a compile out of a gesture is that no gesture
  causes one. A video's decoding is the one thing on a thread of its own.
- **Drawing is culled to the screen**, not to the canvas: the grid iterates only
  the dots in view.

The rule for new work: keep the cost proportional to what is *on screen*, not to
what is on the canvas, and never redo work whose inputs have not changed.

### Deleting leaves nothing behind

Things are made and thrown away constantly here -- boxes, number lines, plots,
pictures, whole canvases at a time -- so **a deleted thing has to stop costing
anything**. Not almost nothing: nothing. Four rules, each learnt from something
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

"No trace" includes the disk. Nothing a box does writes a file -- its shader is
compiled in memory -- and the files the app does write are the canvas, the
recent list, and an untitled canvas's own file. So the check is easy to run:
open a canvas, close it, and the working directory should be empty.

**`CodeBlock::Measure` is memoised, and it had to be.** It is asked for a dozen
times a frame per box -- by every panel, by the placement, by every hit test --
and measured, that was *half of all the CPU the app spent*: 1.0 ms of a 1.9 ms
frame on a canvas of twenty boxes. So the answer is kept, along with a `Shape`
saying what it is the answer to, and a pan only translates the rows it already
has rather than laying them out again.

The rule that made it safe is the one worth keeping: **a signature is compared
whole**, so nothing can be forgotten by halves. Two things are deliberately
*not* in it -- which algebra a box works in, and whether it is prose -- because
both are read off the lines, and so cannot change without the line cache
changing too. That same argument is what lets `EnsureCache` skip asking for the
algebra on the common path, where asking meant parsing every line of every box a
dozen times a frame.

The suspicion recorded here before was wrong, and it is worth saying so: it
named the syntax highlighter (`highlight::*`) as a soft spot, and it costs **0.02 ms a
frame**. It re-tokenises only the code rows actually on screen, and there are
rarely many. `tex::DrawLayout` never showed up at all. **Measure before
optimising** is the rule, and this is what it is for.

Where it stands now, on the sketches in `C:\sketches`: 1.6 to 2.5 ms of CPU a
frame, worst frame 4 to 7 ms, against an 8.3 ms budget at 120 Hz. The one real
spike is the frame a canvas is *loaded* on -- 100 to 200 ms, every shader on it
compiled at once -- and that is a load, not an interaction.

### Every edge is smooth

The window is 4x multisampled, and anything drawn onto it as geometry -- a line,
a card, a marker, a mesh -- comes out smooth for nothing. **Two ways of drawing
do not get that, and every window using either has to arrange it itself:**

- **Into a render target.** raylib's render textures are single-sampled, so a
  window that draws into one and puts the result on screen loses the window's
  multisampling entirely. The space window does this, and draws into a
  multisampled framebuffer that is resolved into its texture (`glext`'s
  `MakeMultisample` and `Resolve`, since raylib can blit a framebuffer but
  cannot make a multisampled one).
- **In a fragment shader.** Multisampling smooths the edges of geometry; an edge
  a fragment shader decides for itself -- a band it discards outside, a ray that
  hits or misses -- is not the edge of any geometry and stays a staircase. So
  such an edge is written as *coverage*: how much of the pixel is inside,
  measured with `fwidth` or found by looking again, never yes or no.

A new window is checked the same way these were: magnify a screenshot with no
smoothing and look at an edge.

### raylib is not permanent

raylib is a convenience, not a foundation. Assume it will be swapped one day and
keep it extractable.

It is already out of the way in the half of the program that matters most: the
mathematics and the toolchain do not know it exists. `mathline.*`,
`mathprogram.*`, `compiler.*`, `process.*`, `selection.h`, `filedialog.*`, `video.*` and a
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

So: **behind every piece of LaTeX on the canvas there is some GLSL that
compiles.** The LaTeX is what you read; the GLSL is what runs. Clicking a LaTeX
box turns it into its code; clicking away turns it back.

The pivotal move is what happens to abstract statements. A mathematician writes

> x + 2 = y,  x, y ∈ ℝ

which is *about* real numbers in general. A program cannot be about reals in
general, so **we choose values for x and y**. Choosing is not a compromise, it
is the whole mechanism: concrete values are what make a thing visualizable, and
visualization is the point.

### Settled decisions

**The code is the source of truth.** The LaTeX is generated from it, by parsing a
restricted subset. Nobody hand-writes the LaTeX.

**Declarations stay abstract; values are injected.** `float a;` is valid input,
and it renders as `a \in \mathbb{R}`. To actually compile, the app injects an
initialiser so it becomes `float a = 0.2385;`. Every variable gets one,
including function arguments.

**A function is declared abstract the same way**, with a prototype --
`float u(vec3 q);` -- and what is injected is a body. That is one of the main
things the app is for; see *Functions nobody wrote a body for*.

The injected text is **not part of the editable source**. It is generated, per
type, by an `exampleGenerator` for that type. So each supported type needs two
things: how to render it as LaTeX, and how to make an example value of it.

Injections are shown, in a distinct colour, when you *hover* the LaTeX. They
disappear when you *click to edit*, because they are not yours to edit.

On top of the injected starting value, free variables get **knobs** — a slider
for a real. That is the "choosing values" mechanism made physical.

**Boxes need not be whole programs.** Fragments should compile, with the app
supplying scaffolding for common patterns; whole programs should also be
possible.

**Only some code has a mathematical reading.** In a box that mixes the two, the
parts that do have a LaTeX equivalent become LaTeX inline, expanding or
contracting that region of the box. The rest stays as code.

**A function reads as an arrow, and what it returns reads as an equality.** A
definition spread over several lines used to have no reading at all: the head
was code, the `return` was code, and only the working in between was
mathematics -- which is backwards, since the head and the return are the two
lines that say what the function *is*. So the head is now the arrow,
`swirl : R^2 -> R^3`, which is the whole of what a signature claims; and the
`return` is the equality that settles it, `swirl(p) := ...`, set where the
answer actually is.

**And the braces are kept, set as mathematics.** They were dropped at first, on
the grounds that the arrow had already said where the function began -- but that
left a definition with no visible extent, and the lines of the body looked like
lines of the box. So `\{` closes the signature and `\}` stands on its own line,
in the maths face rather than the mono one: the same two characters the code
has, saying the same thing, and no longer punctuation borrowed from C. A
mathematician would set a single tall brace down the side of the body instead,
and that is the better answer -- it wants layout across rows, which a box does
not have, since every row here is typeset on its own.

That needed the one thing this file never had: **a line read in the light of the
lines around it.** A `return` is not a statement about anything until you know
which function it is the answer of, so `mathline::Parse` takes an `Inside`
saying which function encloses the line. The spans that decide it are worked out
once, by `mathline::FindFunctions` -- which the toolchain was already using to
hoist functions above `main`, so "where does this function end" is one answer
asked from two directions rather than two answers that can disagree.

The brace that closes a **loop** inside a function is not the brace that closes
the function, and stays code. That is the whole of what `Inside::closes` is for,
and it is the difference between the end of a definition and a block still being
held open.

A function with two `return`s reads as two definitions of the same thing, each
true only under a condition that nothing on screen shows. That is left as it is:
the honest answer is a piecewise brace, which wants the same layout down the
side of several rows that the braces above want, and both can wait for it.

**A part of a value is a subscript, and a constructor is not a function.** `p.x`
is written `p_{x}`, which is what a mathematician has always written; `vec3(a,
b, c)` is the tuple `(a, b, c)`; and `Complex(a, b)` is `a + bi`. The name in
front of the brackets is how a language spells a value it has no notation for,
and mathematics has the notation. The first of these is what makes the rest
possible at all: a function of a vector talks mostly about the parts of its
argument, so without member access its lines had no reading to give.

The dot is the only character here that means two things, and which one is
settled by what follows it: a digit makes it a decimal point, anything else
makes it a reach into a value.

**An absolute value is bars.** `abs(x)` is `|x|`, the same bars `length(p)` is
written with but single: a number's size and a vector's length are one idea,
told apart by the doubling.

**Numbers read as numbers.** `2.`, `2.0` and `2` are the same number, and it is
written `2`. The trailing zeros and the trailing point are how a float is
spelled in code, like the `f` suffix that was already being dropped -- none of
it is mathematics. `1.5` keeps its digits; `100` is left alone.

**A sigil is spelled the way the thing is written.** `Cl(3,0,1);` names the
algebra, and it is spelled `Cl` because that is what the algebra is called and
what it is set as -- `Cl_{3,0,1}`. It was `Algebra(...)` first, which named the
category rather than the thing.

**A renamed spelling is renamed, not aliased.** There is one name for a thing at
a time, and the sketches are rewritten when it changes. Keeping the old one
working was tried and undone: two spellings for one thing is two things to read
and one of them is a lie about what the program calls it, and the cost of not
doing it is a rewrite of a handful of text files. The same goes for `RToR`,
which was `RealFunction`.

**`:=` for a rule, `=` for a fixed value.** In code `=` always *makes* the left
side equal to the right, which is what a mathematician writes `:=` for. But a
line whose right-hand side is a bare number is not a rule, it fixes a value once
-- and "let a = 0.4" is how that is written. So the test is whether the
right-hand side mentions anything: `c := \frac{a+b}{2}` is worked out from a
and b, `a = 0.4` is chosen. The distinction is worth having on screen because it
is the difference between a value somebody *chose* and a value that *follows*:
the first has a knob of its own, and the second moves only when what it is made
of does. (It used to say that `a = 0.4` never moves at all. That changed when a
typed number became a knob -- see *A value never recompiles*.) TeX has no glyph
for it, so it is composed.

**A call is a mention**, even with nothing in its brackets: `r = randChanging();`
is a new number every frame, which is as far from fixed as a line gets, so it is
`r := randChanging()`. A constructor is the exception -- `vec3(1.0, 0.0, 0.0)`
only spells a value, and is as fixed as the numbers in it.

**A name that is a word gets a thin space beside it.** Juxtaposition is how a
product is written, and `speed * time` set tight is `speedtime`, which reads as
one name. Single letters stay tight: `ab` is what everyone expects.

**The LaTeX never shows a number the app chose.** A number somebody typed is
part of what they said, and `float a = 0.4;` reads `a = 0.4`; the one injected
for `float a;` is not, and that line stays `a \in \mathbb{R}`. Chosen values
live in the knobs and in the hovered code view.

**The flip is per-line.** Each line of a box is independently LaTeX, hovered
code, or code being edited. A line with no mathematical reading just stays code.

**Indentation survives the flip.** Indenting is how code says a line is inside
something, and the typeset reading has the same thing to say: a `return` set
flush left does not look like it belongs to the function above it. So a row's
indent is *measured off its own code*, in monospaced cells, which means a line
sits at the same place on the page in either reading and does not jump sideways
when it flips -- the horizontal half of the rule the height already followed.
Prose is exempt, where a leading space is a space between words.

**Editing a box means editing all of it.** Giving a box the caret turns the
whole thing into code, and hides the injected values throughout: while you are
editing, what you see is your own text.

**The toggle does not flip the box, it settles it.** A box starts in neither
state but in *auto*: mathematics while it is being read, code while it is being
written, which is the rule just above. Pressing the toggle answers the question
for good -- the box reads that way whether or not it has the caret, and whether
or not you come back to it tomorrow. It used to be a flip that only applied
while the box was focused, so pressing `code` and then clicking away put the
mathematics straight back: a toggle that springs out the moment you let go of it
has not done anything.

The button says what *pressing* it would give you, which means it has to ask
what the box is doing rather than what was last chosen -- in auto those are two
different questions. Three states, two of them reachable by pressing, and the
third only where you have not pressed yet.

**Tab is about lines, and so is Ctrl+/.** With a highlight, Tab moves every line
it touches one step in and Shift+Tab one step out; Ctrl+/ comments them all, or
takes the comments off again when they are all already commented. Both keep the
highlight afterwards, where every other edit puts it down -- they are edits
*about* what is highlighted, so pressing Tab twice moves the same lines twice.
With nothing highlighted there is no block to move, and Tab is the ordinary step
of indentation at the caret.

Two details worth keeping. The comments go in at the **shallowest indentation**
of the lines they cover, so a block keeps its shape; and a half-commented stretch
**becomes wholly commented** rather than toggling line by line, since that is the
state the next press can undo. They are line comments, one per line: a block
comment spanning several lines would have to be understood by the part of the app
that reads declarations off lines, which reads each line on its own.

**Ctrl+A selects the box**, on the same rule that decides which clipboard Ctrl+C
means: a caret in a box is what says these keys are about the text.

**Alt+Delete takes the whole line**, the way Ctrl+Delete takes the whole word --
or every line a highlight touches. Taking the first line of a text box keeps its
sigil, since deleting a line of words is not asking for the box to stop being
words.

**Pasted text keeps its own indentation.** Typing Enter carries the current
line's indentation onto the next one, which is what writing wants; doing it to
text arriving from somewhere else adds this line's indentation to every line of
what arrives, and it stacks -- a line deeper on every line, so ten lines pasted
back from a text editor came in as a staircase.

**Dragging across a box highlights its text**, and so does holding shift while
the caret moves -- shift means only "leave the anchor where it is", which is one
rule covering the arrows, word movement and Home/End alike. The caret and the
anchor are the same kind of thing, so "is anything highlighted" is one
comparison and needs no flag; typing over a highlight replaces it, and moving
the caret without shift puts it down.
It exists for its own sake, but the reason it was built is the gesture above:
selecting part of a name is how you take hold of what that name refers to.

**The grid is only up while it is being used.** There is therefore no switch for
it: a control for something that already governs itself is a control that can
only be wrong, and the one that used to be in the Escape menu could turn the
dots off and leave a resize with nothing to aim at. The dots are a placement aid,
not scenery, so the canvas is bare while you are reading it and the dots fade in
while something is being placed: a window carried, a window resized, or the
canvas zoomed -- zoom because it is what decides how fine the lattice is.
Panning is none of those and leaves the canvas alone. They linger for half a
second afterwards, since a resize is a run of separate wheel clicks and a grid
that came and went between them would be a flicker, and they fade rather than
blink, because the dots appearing is information rather than an event.

**There are two dots per line of type.** The spacing is not an arbitrary
lattice laid over the canvas: it is half the height of a line at the default
size, measured from the font at startup rather than written down
(`kDotsPerLine`). So the grid is ruled paper, and a box put down on it has every
line boundary on a row of dots.

**A card's padding is one dot**, on every side, so the two things that used to
compete -- the card's corner on a dot, and the text on the dots -- are the same
thing. It used to be one dot per line with a padding that was no whole number of
anything, and the rule had to choose: snapping the text left the corner visibly
off the grid, and snapping the corner left the text off it. Making the dots finer
until the padding could *be* one was the way out of choosing. A row the
mathematics makes taller than a line -- a fraction, a big operator -- is rounded
up to a whole number of dots, so the lines under it stay on the grid too.

The dots are therefore twice as dense as they were, and the on-screen band the
spacing is kept in was halved to match (`kGridMinPx`, `kGridMaxPx`), so at the
default zoom the dots you see are the dots the padding is. Drawing them all at
1920x1080 costs half a millisecond, and only while they are up.

**Every window sits on the grid, and boxes never overlap.** A window's top-left
corner snaps to the nearest dot of the grid *as drawn* -- the in-between dots
only once they have faded at least halfway in (`SnapStep`), since a corner on a
dot you cannot see does not look like it is on anything -- so placement gets
finer as you zoom in. That holds for boxes, number lines, plots and
pictures alike (`SnapToGrid`, in `grid.*`, next to the code that draws the
dots). Boxes
additionally never overlap: one put down on top of another steps clear along
whichever edge is nearest, a whole dot at a time, and the number lines and plots
docked under a box count as part of it. Every box placement goes through one
function (`App::PlaceBlock`), so dragging, typing a new box and loading a file
cannot disagree about the rules.

**Home frames what is on screen.** Every window whose middle is on screen --
the lasso's test, so what counts as "on screen" is what counts as "circled" --
is gathered, and the camera glides until they are centred and fill the view
with a little margin, zooming in or out as it takes. It is Home because that is
what "show me all of it" is in Blender and most 3D viewers, and because it is
not a character: the other conventions, Shift+1 and F, would start a box, since
typing on the bare canvas is how a box is begun. With a caret in a box Home is
the start of the line, as it always was.

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

**Nothing is hovered while something is held.** A knob being dragged, a 3D
window being turned, a window carried or cropped, the canvas panned, a lasso
drawn, a finger down: the pointer is busy doing that, and whatever it passes over
on the way is not being pointed at. **The Escape menu counts too**: while it is
up the pointer is choosing from it, and the canvas under the scrim stays still. Before this, sweeping a knob up across a box
flipped the line under the cursor to code, rims came and went, markers lit up,
and another box's windows jumped to the row the cursor happened to cross -- the
canvas answering questions nobody asked, in the middle of a gesture that wants
the screen to hold still.

The mechanism is one flag and one function (`hover.h`). The app sets the flag
once a frame, after input, from the list of gestures that is already written out
where each one is handed the pointer. Every *hover* reads `hover::Pointer()`,
which is the real pointer normally and nowhere while the flag is up; the
gesture's own code keeps reading the real one. So what the held thing says about
*itself* is untouched -- the knob's own boxes and wires, the carried window's
rim, the crop handle under the hand, the drop target swelling -- because none of
that was ever hover.

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
**viridis** by its position on the canvas -- the boxes in order, and each box's
names in the order it declares them (`Canvas::colourOrder`, and `VariableColor`
in `theme.h`), and
everything standing for that variable wears it: the handle or marker on its
number line, its curve on a plot, its name written beside its marker. Viridis
because it is perceptually even -- two colours the same distance apart on the
map look equally different -- and the walk along it is by the golden ratio, so
neighbouring variables land far apart and a name added at the end recolours
nothing. A name added *earlier* -- in a box further up the list, or above
another in its own box -- moves everything after it along by one step. That is
the price of the canvas having one list: two names in one box used to be able to
wear the same colour, each being first in a box of its own, and a name used in
several boxes now wears the same colour in all of them.
The dark end is skipped, since the cards are already dark.

Colour therefore no longer says whether a variable is free or derived, and
**nothing else does either**. Every value's marker is the same shape. Shape used
to say it, a ring you could drag and a filled dot you could not, and that was
taken out as more than it was worth: every viz would have had to draw two
things. Editability may want showing some other way, or may not need showing at
all. A **blink** says *this one, now*: a visualization in use flashes white,
half a second on and half a second off. A blink because colour is already spoken
for, and because on a shared axis several markers wear colours from the same
map. A hard switch rather than a fade, since a throb reads as
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

**And it works from the other end.** Point at a mention of a variable in the
text -- or highlight one -- and the same things light up: the name wherever else
it is written, the wires converging, and the viz blinking at the far end. It is
the *same set*, gathered by one function in the app, so hovering a window and
hovering its name cannot come to disagree about what "this variable" lights up.
Pointing is a hover like any other, so a gesture sweeping across a box leaves
its names alone. Taking hold of a name in
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

**A wire's side is chosen once, when the value is picked up.** Which side a wire
leaves by -- the left edge of a name or the right, the left of the card or the
right -- is a genuine choice with no middle: a bundle must gather on *one* side
or its strands would set off in opposite directions and never meet. And a
choice re-taken every frame is a choice that can change its mind halfway
through a drag, which is exactly what it did: dragging a knob across the card
flipped the whole bundle to the other side in a single frame.

So it is not re-decided. The side is taken when the value is taken hold of and
kept until it is let go; everything else about the wire follows from it, so
freezing that one bool freezes the layout. **A choice re-taken every frame is a
choice that can change its mind mid-gesture** is the rule, and this is the
cheapest thing that obeys it -- one bool per held value, and four lines.

Making the geometry *continuous* instead was tried and undone. It worked, but it
was a second mechanism for a problem the freeze already covers -- while a
gesture is running nothing is re-decided, and while merely hovering the value
does not move -- and the wiring is meant to stay simple.

**A name written five times is still one variable, so the wires converge.**
They gather at a junction just clear of the *names* -- not of the card, which on
a wide box would be a long way from anything, and a wire setting off for an edge
nowhere near the thing it points at looks like it is avoiding it. Level with the
middle of the mentions, on the side the window is on, and one wire runs from
there to the window. The strands arrive at the junction along the same horizontal tangent, so
near it they lie on top of one another and the bundle reads as a single cable
fanning out. Five separate cables leaving the same handle would say there were
five things to point at, when the whole claim being made is that there is one.
A name written once needs no junction and gets none.

**A derived value is the end of a little machine, and pointing at it draws the
machine.** `float d = a + b * c;` is not one thing pointing at another: it is
`b` and `c` meeting at a `*`, whose answer meets `a` at a `+`, whose answer is
`d`. So that is what appears -- wires out of the windows of `a`, `b` and `c`,
each in its own colour, converging on a little bullet with the operator written
in it, and out of the last bullet wires in `d`'s colour running to every place
`d` is written and to `d`'s own windows.

Three rules make it read:

- **The wires leave the vizes, not the mentions.** The window *is* the value; a
  mention is only where it is spoken of. This is the same rule the plain wire
  already followed, and it matters more here: a diagram drawn between mentions
  would be a diagram of the text rather than of what the text computes.
- **A wire out of an operator is black.** Colour is identity, and the working is
  not any one variable's -- it belongs to nothing in particular until it arrives
  somewhere that does. So the intermediate wires have no identity, and the last
  one takes the result's colour.
- **The operator is written as the code writes it**: `*`, `+`, `/`, `|`, `^`.
  A call gets a bullet with its own name in it, `sin` or `grade0`. A wiring
  diagram is about what the machine does, so it is spelled the way the machine
  is.
- **A bullet has a front.** It is flat at the back and pointed at the front, and
  it points at whatever it feeds -- so the wires coming in arrive square against
  the flat end and the one it produces leaves the point. A shape with a front
  says which way the working is going without anything being labelled, which is
  the whole reason it is not a circle.
- **What it made is one thing**, so one wire leaves the point and runs a little
  way before it forks to the places that thing turns up. Several wires leaving
  the point at once would say several things had been made.

The chain is laid out with the **deepest at the bottom and the last bullet
nearest the box**, because the working runs *towards* what it makes. Rungs the
other way up have every bullet pointing back over its own inputs.

A **literal is not a node**. `c = (a + b) / 2` has no wire for the `2`, because a
number has nowhere to come from; the `/` simply has one input. That is honest
rather than a gap: the thing you might want to point at is not there to point at.

**It hangs below the box and everything docked to it**, in the free canvas,
rather than in the space between the values and the result. Tempting as
"between" is, a number line sits directly under its own box, so between is a few
pixels and the bullets would land on top of the very markers they are wired to.
Each operator gets a rung by how deep it is, and sits across at the mean of what
feeds it.

**And whatever feeds a highlighted value is in scope.** The rule that stands a
box's other windows down while the caret is in it would otherwise take away the
inputs' markers the moment you highlighted the result -- leaving a machine with
nothing coming into it. Being made of something is a way of being what you are
looking at.

What this is drawn from is **not the parser's own tree** but a small graph made
for it (`mathline::Wiring`): leaves that name variables, and operator nodes fed
by other nodes. Nothing outside the parser needs to know about precedence, or
about the difference between a call and a product -- only what feeds what, and
what to write in the bullet where two things meet.

**The curve leaves the visualization, not the pointer.** The handle on a number
line *is* the variable; the pointer is only where your hand happens to be. That
matters now that a box's reals share one axis: three wires all setting off from
the cursor would be a knot, where three wires each leaving their own marker are
three separate statements. A plot's wire leaves the middle of the plot, since a
curve is a whole function and has no one place it is.

The exception is a **picture**, which reports the pointer -- and for exactly
the same reason. There the pointer is not where your hand is, it is the point
the function is being asked at: it *is* the value.

**The pointer over a picture is the argument of the function it draws** -- the
`p` of `vec3 ripple(vec2 p)`. Not a metaphor: that is the value the function is
being evaluated at. So it is reported as the thing under the cursor like any
variable, its name lights up where it is written -- in the definition's own
lines only, since another function's `p` is a different `p` -- and the curve
runs from the cursor to it. It is not one of the box's variables, so it takes
the next colour along from them.

Finding the mention is easy in code, where the font is monospaced and a name is
a column. In LaTeX it needs help, and the way it gets it is worth knowing: when
`mathline` writes the LaTeX it wraps every identifier in `\sbref{k}{...}`, a
command that typesets as exactly its argument and does nothing else. The tag
rides through parsing onto the node, and through layout onto every item the node
produced. So the renderer can ask "where did the third identifier end up?" and
get an answer in pixels, without the typesetter knowing anything about
variables.

### What a window is showing

A variable is **mutable unless it is `const`** -- as it is in GLSL -- so
it can hold one value on one line and a different one three lines further down.
A window standing for it therefore has to answer *when*, and there are two
answers depending on whether the box is being read or being written.

**A window shows the value at the line the box is being read at.** That line is
the box's **executed up to here**:

- the **caret's** line, while the box is selected;
- otherwise **the end**, which is what the whole block came to.

It used to follow the pointer as well, and to remember where the caret had been
left. Both made an unselected box's windows depend on where the mouse happened to
be passing, so a number line flickered between values as the pointer crossed the
card on its way somewhere else. A box nobody has selected is not being read a
line at a time.

**A hovered declaration shows the literal it is given**, not the value at the
reading line: `float a;` hovered reads `float a = 0.4;`, what the knob chose,
even when a line further down has made `a` something else by the end.

Nothing is run again to answer that. The shader writes **every real's value
after every line** into one shader storage buffer, in the single invocation that
runs the box, and the *when* is chosen afterwards by reading a different row.
Moving the caret is free.

That is what a buffer buys over the pixel-per-value the probe started as: no
packing to undo, no four-byte limit, and the whole history rather than one
number each. It wants GL 4.3, which is why the context is 4.3.

A slot for a variable **not in scope yet** is left NaN rather than zero, because
zero is a value and "not yet" is not; a window whose variable has not been
declared where you are reading keeps showing what it last was.

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

That edge is gone. A `float a;` assigned over further down used to look like a
knob that did nothing, because its window reported the end of the block where
the assignment had already replaced what the knob chose. Read at the line above
the assignment, it now shows what the knob chose; read below it, what replaced
it. Both are true, and which one you get is which one you asked for.

**A variable is not a uniform. Its literals are.**

That distinction is what makes a knob work on something mutable. A variable in a
box is an ordinary GLSL local and can be assigned over like any other; what gets
a uniform is each **literal** assigned to it -- including the one the app
injects for `float foo;`, which is a literal like any other, just not one you
wrote. So a `foo` declared on line 2 and assigned numbers on lines 9 and 13 has
uniforms `foo_2`, `foo_9` and `foo_13`, and those lines read

    float foo = foo_2;      foo = foo_9;      foo = foo_13;

**Dragging moves the literal that governs what you are looking at** -- the last
one at or before the line the box is being read at. Read above line 9 and the
knob moves `foo_2`; read below it and the same knob moves `foo_13`. That is the
only choice that makes sense: the number under your finger is the one the value
on screen actually came from, and moving any other would change nothing you can
see.

It also means one variable can have several knobs along a block, each live over
the stretch it governs, without any of them being a different variable.

An earlier note here said a free real could not be assigned over, because it
*was* the uniform. That was a symptom of getting this wrong, and it is gone.

**A variable not yet declared where you are reading is not shown at all.**
Before its declaration it does not exist, so a window for it would be reporting
a value it does not have; it disappears rather than showing a stale one.

### Things that move by themselves

**`time` is the seconds since the box was last compiled.** It is a uniform every
box's shader has, so a picture, a curve, a surface or a derived value can all be
written in terms of it. Since compiling, rather than since the app opened,
because editing a box is starting it again, and a clock that did not restart
with it would make the first frame of an edit arbitrary.

**`rand()` is a number in [0, 1), and every call is a different one.** GLSL has
no state that lasts from one call to the next except a global, so that is what it
keeps: a hash state, moved on by each call, seeded by the pixel. So `rand() +
rand()` is two numbers, neighbouring pixels differ, and nothing changes from one
frame to the next -- a picture made of it holds still. **`randChanging()`** is the
same with the frame mixed into its seed, so it is new every frame. Two states, so
asking for one never moves the other. There is no standard name for the second;
Shadertoy folk just hash in the frame counter.

When a box is *reporting* its values rather than drawing, every pixel of the
report runs the same body for a different variable, so they are all seeded alike
-- otherwise `r = rand()` would be a different number on every pixel of its own
report.

**A box that says `time` or `randChanging` is re-read every frame.** Nothing is
re-run for a box whose inputs have not moved, and for these the input is always
moving -- so its derived values are asked for again each frame, and its markers
move on their own. A box that does not say them costs what it always did.

**Hovering a knob brings up a play button under it.** Pressing it swings the
value as `centre + amplitude * sin(t)`: centre where it was when pressed, `t` the
seconds since, and the amplitude a quarter of its axis's span, so it sweeps half
the row and stays on it. A swing rather than a ramp, because a value that runs
off in one direction runs off the axis, and a swing keeps coming back past where
it started, which is usually where the interest is.

- **While it plays, the button stands under the middle of the swing**, and shows
  whenever the pointer is on that row. A marker that is moving is not one anybody
  can be expected to chase in order to stop it.
- **Pressing again stops it where it is.** Grabbing the marker takes it over, the
  same way: the hand wins.
- **It rewrites the number as it goes**, exactly as a drag does, so the line
  says what is running: a uniform set and a line retyped, and nothing compiled.
- **Everything playing pauses while any box has the caret.** A line changing
  under somebody's caret -- or under the undo they are about to press -- is a
  line they cannot write in. The swing picks up where it left off.
- **Playing is not saved.** A canvas opens with nothing moving, and the number
  in the text is wherever the swing had got to. Pressing play is an undo step
  like any press; the swing after it is a change nobody made, folded into where
  things are, and where it stops is written down at once -- so one undo after
  stopping goes back to the value before it began.

### Undo, for the whole canvas

**An undo step is the whole canvas, as the text it would be saved as.** Not a list
of operations with an inverse each: carrying, merging, closing, pulling out,
resizing, throwing a slide away and typing a new box are all changes, and every
one would need its inverse written and kept right forever. The file already says
everything that matters, so a step is a snapshot of it (without the camera --
where you are looking is not something an undo should move) and undoing is loading
it back. A snapshot of the demo deck is 4KB and a tenth of a millisecond.

Ctrl+Z and Ctrl+Y, or Ctrl+Shift+Z. **The caret decides which undo**, the way it
decides which clipboard: inside a box they are the box's own, and a whole session
of writing in a box is one canvas step, taken when the caret leaves it.

A step is taken **once things have settled** after something was done: nothing
held, and a moment since the last click, key or wheel. That is what makes a drag
one step and a run of wheel clicks one step. And **a change nobody made is not a
step**: a build coming back, an axis framing itself, a window appearing because a
box now has a variable. Those are folded into where things are, or undo would
spend a keypress going back past them to no visible effect.

**Restoring keeps every box whose text has not changed.** Loading a canvas builds
every shader on it, which is a fifth of a second on a big one; undoing a move
should not cost that. So a box whose lines are exactly what the step says is kept
-- its shader, its caret history and all -- and only moved, resized and given its
knob values back; only an edited box is built again. Positions are taken exactly
as written, not snapped again, since today's zoom may have a coarser lattice than
the one they were put down on.

Pictures and videos are loaded again from disk on every undo, which is the next
thing to keep rather than rebuild if a canvas full of them makes undo slow.

### Display windows and vizes

Two words, because the difference between them is the whole of where this is
going.

A **dw** -- a *display window* -- is a window that shows values on a scale of
its own. The number line and the plot were the first; the space window, the
surface and the volume have come since. A dw knows about
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

### Vizes are bootstrapped

**The app's own vizes are written the way a user's would be: in GLSL, on a
canvas.** That is the aim. There are already many kinds of viz, and there will be
many more: a ball, an arrow, a line through space, a plane, a conic swept into
a tube, a field. Across all of them, two things hold. A viz:

- **is drawn** in a chosen dw, or a chosen set of them, and how it is drawn
  depends on the value of the variable it stands for;
- **has a position** in that dw, which is where its wires attach, and that
  position can depend on the value too.

Both come from GLSL functions that belong to the viz, and **both are in one
window**. A viz is one box, not a pair.

**One window per type and dw.** A point in space and the same type in some other
dw are two windows, since how a thing is drawn is a question about where it is
drawn.

**A window's parts are functions with standard names:**

| | |
| --- | --- |
| `mesh` | makes the geometry: vertices, normals, texture coordinates |
| `vert` | the vertex shader: where each vertex goes, given the value |
| `frag` | the fragment shader, and optional |
| `position` | where the viz is in its dw, which is where wires attach |
| `grab` | how far the pointer is from taking hold of it, in pixels |

The window's first line names both halves of the pair, `Viz(Tri, Space);`, and
a dw must be named.

**The geometry does not depend on the value.** `mesh` runs once, to fill the
buffers, and again when it is edited; the value arrives in `vert`. A viz whose
number of parts depends on the value, such as a bivector that is sometimes one
line and sometimes two, is meshed at its most, and `vert` collapses the parts
it does not need.

**A viz with more than one piece numbers them**: `mesh2`, `vert2`, and optionally
`frag2` and `position2`, and so on up to 4. An arrow is a shaft and a head. A
window that defines a fifth piece gets a warning, not a fifth piece.

**A viz with no `frag` is lit with phong**, which is the app's, not something
written in `viz.sketch`. A point of Cl(3,0,1) is a lit ball and needs nothing
more, and many are like it. A dw's own furniture, such as the space window's
sky, walls and camera, stays the app's too.

**The app's vizes live in the repo, in one sketch per algebra**:
`Cl(3,0,1) vizes.sketch` and `Cl(2,1,1) vizes.sketch`. A `Tri` means a different
thing in each, and a canvas works in one algebra, so their vizes cannot share a
canvas. These hold the built-in vizes, the ones every canvas has without asking.
A canvas may also define vizes of its own, and those are local to it.

**A built-in viz is compiled the first time a dw on screen asks for it**, not at
startup, because a viz of an algebra type is an algebra compile, 60 to 200 ms. Edit
one and it is run again straight away.

**Functions are drawn by vizes too.** A picture, a surface, a volume, a curve on
a plot and a field on a number line all draw a function written in another box,
and all of them are to come out of C++ as well. **A function's viz is compiled
into the box that defines the function**, as more modes of that box's one
shader, so a box still builds one shader and a knob still only sets a uniform.
The price is on edits: changing a function viz rebuilds the boxes with functions
of that type.

### How the vizes drifted

This is recorded so that it does not happen again. Viz boxes arrived with a
fallback: *a type with no viz boxes keeps the drawing the app was born with*.
The fallback became the main road.

- **Every window added since then was drawn in C++**, in its dw's own file: the
  space window's balls, the surface, the volume, the picture, the colour
  window, and the beads of a spread. None of it asks the canvas for a viz.
- **The viz box could not have said any of it.** It is 2D and works in screen
  pixels, with no normals, no lighting and no fragment shader of its own.
- **Nobody extended it instead, and nobody said that was a choice.**
- **Only two places ask for a viz**: the number line and the plot.
- **The plot's viz has not worked since a function became GLSL.** Its mesh half
  compiles on its own, so it cannot call the function it is meant to draw.

The rule for new work: **drawing a type is a viz, and a viz is GLSL in a
vizes sketch.** If a new window needs a drawing that a viz cannot express yet,
extend what a viz can say, or stop and ask. That is never a reason to draw it in
C++.

### Writing a viz inside the app

This is what exists now, and it is what the section above replaces: two boxes
per viz, 2D, with the mesh laid down again for every value.

**The first two questions can now be answered on the canvas.** A box whose first
line is `Viz(float);` is a **viz box**: it says how a value of that type is
drawn rather than declaring a value of its own. It has no variables and grows no
number lines.

There are two of them per viz, paired by the type they name.

**The mesh half is a compute shader.** A viz box compiles on its own, into a
compute program that writes its vertices into a buffer, and is dispatched once
per value with `value` as a uniform.

**Which half is which is told by what it does**: the mesh half is the one that
**lays geometry down**. Nothing else calls `vertex` or `line`, and a half that
calls neither is the one that places what the other made.

The two ways of laying geometry down append into
shader storage buffers through an atomic counter -- one buffer for triangles,
one for the segments of a path, each with its count in front of it.

**It only runs when the value moves.** The geometry is a function of the value,
so nothing is recomputed while nothing has changed.

**And the geometry never comes back.** The place shader reads the buffer the
mesh half wrote, indexing it by `gl_VertexID`, and the draw goes round raylib's
batch with an empty vertex array bound -- a core-profile draw needs *some*
vertex array even when it reads nothing from one. What makes that safe is a
**memory barrier**: what a compute shader writes is not visible to the vertex
fetch that follows until one says so, and without it the only way to wait was to
copy the whole mesh back to the CPU, which is the stall the buffer existed to
avoid.

raylib wraps no barrier, so `glext.*` fetches the entry point itself -- Win32,
raylib-free, like the rest of that layer. It is the same shape of thing
`KHR_parallel_shader_compile` will need, which is a good reason it exists as a
file rather than as one call.

What still crosses back is **two counts, four bytes each, and only when the
value has moved**: a draw call has to be told how many vertices there are, and
short of an indirect draw there is nowhere else for that number to come from.

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
Viz(float);                                Viz(RToR);
p = sbMap(vec2(value, 0.0)) + p*sbScale;   p = sbMap(p);
```

for the marker on a number line and the curve on a plot respectively. The mesh
halves are the ring, and the sampling:

```
Viz(float);                                Viz(RToR);
for (int i = 0; i < 20; ++i) {             float px = value(-8.0f), py = -8.0f;
  float s = 6.2832f * i / 20.0f;           for (int i = 1; i < 129; ++i) {
  float e = 6.2832f * (i + 1) / 20.0f;       float y = -8.0f + 16.0f*i/128.0f;
  vertex(7*cos(s), 7*sin(s));                float x = value(y);
  ...                                        line(px, py, x, y);
}                                            px = x; py = y;
                                           }
```

`C:\sketches\viz.sketch` has the ring, and editing either of its boxes changes
what is drawn on the next rebuild. Its two `RToR` boxes are empty, and the
`RToR` viz could not draw a function anyway (*How the vizes drifted*).

`C:\sketches\spaces.sketch` is one of each window a function's type can ask
for: a picture, a domain colouring, a surface and a volume.

**A viz box is compiled standing alone as well**, with `value` an example of its
type. That means a mistake in a viz is reported on the viz's own lines
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

The elaborate vizes still to come have the same shape. Each is some
geometries, a transform from the value, a distance and a drag: the same four
questions under different names.

### What a viz still cannot say

- **Distance and drag are still the app's.** A viz box cannot yet say how near
  counts as near, or what a drag means. Both are small, and both want the same
  treatment: a third and fourth box, or two more halves of the same one.
- **A viz cannot say what it needs from the value.** A real's marker does not
  depend on its number at all, yet its lines are re-run for every value; a curve
  genuinely does. Nothing distinguishes them, so nothing skips the work.
- **Two viz boxes naming the same type *and* window** is still not a choice --
  the first mesh half and the first shader half win. Naming different windows is
  how you get two.

### A type has more than one viz

A function is a curve on a plot and a field of colour on a number line. Those
are two different answers to "how is one drawn", not one answer used twice --
so **a type may have several vizes, in several windows**, and a viz box may say
which window it is for: `Viz(RToR, Axis)`. The two are held apart as
`Type@Window`, and a box naming no window is the one a window gets when it asks
for a type and nothing more.

### A function as a field

The number line already says what x is at every pixel of its width, and a
function has a value at every x. So a function goes on a number line as a
**field**: not a point, which it has no way to be, but a wash of colour saying
how much of it there is and which way.

**Zero is nothing at all, +1 is opaque viridis green and -1 is opaque viridis
yellow.** Beyond either end it is **clamped**, not rescaled -- and that is the
part worth defending. Rescaling would make the colour mean a different number on
every axis, so two fields could not be compared, which is the whole reason they
are on axes at all. Clamped, green means "at least one" everywhere.

**Each function starts on a row of its own.** Two fields on one row cannot both
be read -- the second is simply painted over the first -- so they begin apart,
and dropping one row onto another is how they come to share a scale, exactly as
it is for values. A box of nothing but functions still gets rows, which is new:
a number line used to mean a box had reals.

### Space and spacetime

The first dws with a camera of their own. A number
line shows a real and a plot shows a function; **a spaceDw shows a point**, which
is what a trivector of a 4D projective algebra is. There are two of them and
they differ in exactly two things: the algebra whose `Tri` they take, and the
sky. **Cl(3,0,1) is space and Cl(2,1,1) is spacetime**, where the third axis is
time. A box working in neither algebra gets neither window.

Like the number line, **a box gets one and all of its points share it** to begin
with: a stack of one-point windows would say nothing about how the points
compare, and the points in a box are read together. And like the number line,
that is only where they start. A point can be pulled out into a window of its
own and dropped onto another -- of the same space; a spacetime point has no place
in a space -- so a window holds whichever points were put in it, and `Sync`
keeps that arrangement: a point already in some window stays there, and only a
new one goes to its box's **home** window, which is also the one that shows the
spread a function of the box is being fed. The canvas file records each window's
points, and a canvas written before this has every window as its box's home.

**A point's colour is its variable's.** It used to be its index in the window,
which happened to agree while every window held its whole box in order, and would
have recoloured a point as it was pulled out.

**A Tri's coefficients are homogeneous coordinates**, and which one is which
axis needs no table. A grade-3 blade of a four-generator algebra *omits* exactly
one generator, and the one it omits is the axis it stands for: e023 omits e1 and
is x, e013 omits e2 and is y, e012 omits e3 and is z, and e123 omits the null
generator e0 and is therefore w. That is the rule stated plainly -- the
coefficient with no null element is the homogeneous one -- and it falls out of
what a trivector is rather than being a convention to look up.

**The cuboid is a window into the space, not a box containing it.** It is a
wireframe with a floor and walls, and it is **turned inside out** so the near
walls are culled and you always see into it, whichever way the camera has come
round. What falls outside it is **discarded in the fragment shader** rather than
merely being out of shot: the walls are where the world stops being drawn.

Two things are exempt from that cut, and for the same reason in both cases --
they are not *in* the space being cut. **A point at infinity** is nowhere in
particular, and cutting it would be cutting the very thing worth seeing; the
walls **are** the limits, so testing them against themselves is a question with
no answer. Each window carries its own switch for whether the cut happens at
all, and it is saved with the canvas.

**Right-drag orbits the camera about the origin, and so does a two-finger drag**
-- which is the tablet's version of the same thing, and it can be, because over
one of these two fingers have nothing else to mean. The wheel moves the camera
in and out, like every other window's wheel is its own. A number line already
claims the right button for its scroll; this claims it for its camera, and says
so, so the canvas leaves its own pan alone for as long as the gesture lasts.

Four things about the GPU here were learnt the hard way and should not be
undone:

- **It draws multisampled.** Into a 4x framebuffer, resolved into the shared
  texture before that is put on screen. A render texture on its own is
  single-sampled, which gave every wall, wire and point a staircase edge the
  rest of the canvas did not have.

- **One render target between all of them**, not one each. It is drawn into and
  blitted at once, so the next window has it straight back -- which is what
  keeps a canvas full of these from being a canvas full of framebuffers. It is
  released when the last one goes, and by hand, since a destructor cannot free
  what is on the GPU.
- **raylib frees a material's shader along with its model.** These models share
  their shaders -- two carry the same one, and the sky's is swapped between two
  as it draws -- so unloading the models would free the same shader two and
  three times over. The models are handed back the default shader first, and the
  real ones are released once each by the code that made them.
- **An indexed mesh keeps its winding in the index buffer.** raylib's cube is
  indexed and its sphere is not, so turning one inside out and uploading only
  the vertices and normals turns the lighting round while leaving the cube
  facing exactly the way it was -- which looks like a solid block rather than a
  room, and hides everything inside it.

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

**Slides come from the Escape menu, and only from there.** "New slide" puts one
at the end of the deck, clear of the last slide with the same gap every pair of
slides has, so a deck grows to the right by itself. There used to be a button
on the canvas as well, standing at the end of the deck and walking along past
each slide it made. It is gone: the canvas is for what is on the slides, and a
control standing among them was one more thing on the canvas that was not the
work.

**And the arrow keys go through it.** Once the camera is framing a slide, left
and right flick to the one before or after -- instantly, because going through a
deck one slide at a time is not a journey and easing into each one would make it
into several. Off a slide the arrows mean nothing yet, and inside a box they are
the caret's. Which slide follows which is decided by nothing but **where they
sit**: the order you can see is the order there is.

**And a two-finger swipe does the same thing**, on the same terms: only from a
slide, and sweeping left goes forward, exactly as it does in a browser. It is
the gesture people already use to go back and forward through pages, and going
through a deck is the same motion.

On a **touchpad there is nothing to recognise**: Windows already reports a
two-finger horizontal swipe as horizontal wheel, which is the very signal a
browser turns into back and forward, and nothing else here uses that axis. It
arrives as a stream of small deltas rather than one, so they are piled up until
they are decisive, and **one sweep turns one page** however far it runs -- the
pile is only put down once the fingers have stopped. (GLFW inverts this axis on
the way through, so a Windows "scroll right" reaches us negative.)

On a **touchscreen** it has to be told apart from the pan and the pinch that two
fingers otherwise are, and it cannot be told apart until the fingers have
travelled. So while the question is open the canvas **holds still**: being *on*
a slide is a tight test, and drifting even slightly would take the camera off
the very slide it is about to step from. If the fingers change their separation
it was a pinch after all, and the gesture goes back to the camera from that
moment rather than from the start.

**A window under the fingers takes the pan, but never the swipe.** Everything
else here gives way to whatever is under the pointer; a swipe cannot, because a
slide is mostly covered by its own title and body, so a swipe that stood down
for a window would stand down almost always. Nothing on this canvas does
anything with two fingers, so there is nothing for it to be taking away.

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
is a *place*, fixed in the canvas, and the deck is a row of places. Clicking one does the only thing it does, which is take the
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
gap, and so does everything standing on them.
A deck is a row, and a row has no holes in it.

The aspect is a deck-wide setting, chosen from the Escape menu as one of the few
shapes a deck is ever made for. It starts at **3:2**, which is also the shape
the app window opens at -- the same number in both places, so that a slide seen
at its own scale is exactly a fresh screenful of it and the cover zoom comes out
at 1.

It is **folded away** in the menu. A deck's shape is chosen once and then left
alone, so four buttons permanently taking a quarter of the panel would be four
buttons in the way of the things that are pressed every day. The header carries
the shape it is currently set to, so folding the choice away never folds away
which one it is -- and whether it is folded is a fact about the menu rather than
about the canvas, so nothing saves it.

### The app opens where you left off

**It opens the canvas last worked on** -- the top of the recent list -- because
going back to it is the commonest thing anybody does here. `--open <file>` opens
that file instead. **New canvas**, at the top of the Escape menu, is the way to
a blank one; the canvas being left is saved first, as it is before any other is
opened.

**A new canvas is empty.** It used to open with a starting card -- two chosen
reals and one derived from them, the smallest thing that shows the whole loop --
and that is a thing to delete before beginning rather than a thing to begin
from. Typing with nothing selected starts a box wherever you are looking, so
nothing is harder to reach; there is simply nothing there until you put it
there. It has no file until something is put on it, and then it is given an
untitled one like any other.

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

**Every window answers the lasso the same way, and now says so once.** The eight
things a panel does with a selection -- catch, clear, ask, find a rim, remember
where everything was, move it, draw its rims, erase it -- were written out six
times, once per kind of window, and are now written once in `selection.h`. All
they ask of a window is that it has a `selected`, a `worldPos` and a `dragFrom`,
and that whoever holds a row of them knows where the n-th one is on screen. What
stays with each kind is the only part that was ever different: **what closing
one means** -- a knob bakes its value into the code, a plot does not, a picture
releases its texture.

A third copy of the lasso hit test went with them, in `spacedw.cpp`, and it had
quietly drifted: it tested against the raw stroke where everything else tests
against the *closed* loop, so a space window was caught by the chord where every
other window was caught by the curve.

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

**The caret decides which clipboard.** Those same three keys copy *text* when
there is a caret in a box, and windows when there is not -- one key, two
clipboards, and no third chord to learn. There is nothing ambiguous to resolve:
a caret means you are writing, and what you are writing is what Ctrl+C is about.

It is the **system** clipboard rather than one of our own, so a line of this
canvas can go into a message and a paragraph from somewhere else can come back.
That is the whole reason to have prose here that other people will read. A
newline in what arrives splits the line, so pasted paragraphs come in as
paragraphs; the caret is a byte offset and the text is UTF-8, so it goes in as
bytes and whole sequences stay whole.

The windows clipboard stays our own, since a box with its knob values is not
something another program has a use for.

**Closing is remembered.** Otherwise the next Sync would put the window straight
back. What is closed is kept on the box, since it is a fact about one of its
variables, and it is saved with the canvas.

**Closing a control writes its value into the code.** A number line for a free
variable is not a view, it is the thing choosing the value -- so throwing it
away has to leave that value somewhere. The initialiser that was only ever
injected becomes the user's own text: `float a;` becomes `float a = 0.4;`. What
was temporary is now written down, which is what losing its knob should mean.
Undo puts it back. Nothing compiles: `float a = 0.4;` reads its number through a
uniform exactly as `float a;` did, so the shader is the one already running.

A typed number is a knob too, so writing one in does not by itself take a knob
away -- **the window stays closed because it was closed**, and a closed window on
a typed number stays closed. Closing a knob whose number was already typed just
closes it: the text already says what it got to.

**And taking the number back out brings the knob back.** Editing `float a =
0.4;` back to `float a;` is asking for the app to choose again, and a hidden
variable that has gone back to having no number written is un-hidden. A closed
window on a *derived* variable stays closed, since nothing about it has changed.

Nothing else has that consequence. A plot, a shader's picture and a derived
variable's marker are all views: closing one takes away the view and nothing
else. The rule is worth stating plainly -- **a window that controls something
bakes what it controlled; a window that only shows something does not.**

### Sharing between boxes

A box is still one shader, but it is no longer a world of its own:

**Everything shareable is visible everywhere, automatically.** No imports and
no names for boxes: a function defined in any box can be called from any other,
and a free value declared in any box can be read by any other -- the way a paper
defines a thing once and then uses it. Any box can be the home of something
shared; there is no special box of definitions.

**What is shared is functions and free values, and nothing else.** A function
-- a definition or a prototype -- is text, and a box that calls it gets it
compiled in. A free value, `float a;` or `float a = 0.4;`, is a uniform, so
another box reads the same uniform and one knob moves both without anything
being compiled. A **derived** value is not shared: `c = a + b` is worked out by
running its box's lines, and sharing it would make the canvas one program with
an order between boxes, which a canvas does not have.

**The algebra is the canvas's.** A canvas works in one Clifford algebra, so
nothing is ever written in two at once. The `Cl(...)` line is still written in a
box and still reads "Consider Cl_{3,0,1}", but it is a statement about the
whole canvas; a canvas that says two different ones is an error on those lines.
A box with no `Cl` line is read and built in the canvas's algebra, which is why
the line cache compares the canvas's algebra as well as the box's own lines.

What follows from those:

- **A knob still never recompiles.** Only editing shared *text* rebuilds the
  boxes that use it, and a rebuild that comes out the same is still not
  compiled -- so a box that does not mention what changed costs nothing.
- **The cost is on edits.** Every box calling an edited function is rebuilt, on
  the drawing thread, and with an algebra that is 60 to 200 ms each.
- **A name defined twice on the canvas is an error in both places**, since with
  no order between boxes "the later one wins" means nothing.
- **Sameness is shown.** A call in one box lights up the definition in another
  and wires to it; a shared value's windows are its home box's windows.
- **A box copied to another canvas** can be left calling something that is not
  there.

**How it works.** Every box's build works out what it *offers* -- its functions
as the GLSL it compiles them to, its free values as the uniform behind the
number they are given where they are declared -- and every name it defines at
the top level. Once a frame the app lists which version of each box's offer it
has; when that list changes it gathers them into one `mathprogram::Canvas`
(`App::ShareAcrossCanvas`), and each box asks whether what it *took* from the
canvas has changed. Only those are built again, and only those whose shader
comes out different are compiled.

A box takes what it mentions and does not define, and whatever that mentions in
turn. Values arrive as a uniform and a global set where every drawing starts;
functions arrive as their declarations first and then their text, so functions
in different boxes can call each other whatever order they are gathered in.

- **A shared value is what it is given where it is declared.** `a = 0.9;`
  further down its home box is part of that box's run of lines, which no other
  box runs.
- **A knob's uniform is named for its box as well as its line**, `k_5_12`, since
  a function from one box is compiled into another and two boxes can each have
  a `k` on line 5.
- **A mistake in shared code is reported at home.** Compiled into another box,
  it can only say that it is in something shared -- pointing at a line of that
  box would be a lie.
- **Pointing is shared too.** A shared name wears its home colour everywhere,
  pointing at a mention of it in any box -- or highlighting one -- lights it up
  from its home, and whatever lights up at home lights up in every box that uses
  it, wired from the same window. Dragging a shared real out of the box that uses
  it pulls out its home's value, since that is where its window is.
- **A derived value from elsewhere says why it is missing.** The driver can only
  say the name is undeclared; when it is a value another box works out, the
  message says so, and that only free values and functions are shared.
- **A box being written in is left alone** until the caret leaves it, or a
  change elsewhere would compile half a thought.

`C:\sketches\sharing.sketch` shows it: one box defines `k` and `bump`, and three
others use them.

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
Justification is **one control, not three**: left, centred and right are one
choice, and a choice with three states is a thing you press until it says what
you want. The icon is the state it is in -- which side is ragged is the whole of
it -- so it is also its own label. Every control in the strip is one gap from
the next, since a strip with a rhythm reads as one strip.

**The strip is drawn after every box**, not with the box it belongs to. It sits
outside the card, in the gap above, which is exactly where a neighbour's card or
a neighbour's number line can land on top of it -- and a control you cannot press
is not a control.
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
read-only marker that moves when its inputs do. Both look the same, and they
are told from each other by colour.

**And a viz is pulled out of the window it is in.** Hold Ctrl and drag a marker
off its number line, a curve off its plot, or a point out of its space window --
or, on a tablet, hold the window with one finger and drag the viz out with
another. Nothing happens while it is still inside the window; the moment it
leaves, it is a window of its own, under the pointer, being carried. Letting go
on another window of the same kind merges them, exactly as dropping any carried
window does, and letting go inside the window it came from does nothing.

That last property is the design, not a detail: **pulling out ends in an
ordinary carry**, so the drop -- onto the canvas, onto another window to merge,
onto the cross -- is the code every window already had. What is new is only how
the carry starts. It is the same move as pulling a variable out of the code,
made from a window instead of from text.

What is under the pointer is asked the same way for the mouse and the finger
(`App::FindPullable`), so the two cannot disagree. Two things about asking:

- **A curve cannot be found from this side.** It is drawn by the GPU, and the
  CPU has no way to evaluate a GLSL function to see where it runs. So a plot with
  several curves is asked on the next frame: while it draws, it reads back the
  few pixels round the pointer before and after each curve, and the last curve to
  change them is the one on top. One tiny readback, once per gesture. A plot with
  one curve needs no question -- press anywhere in it.
- **A field is under the pointer everywhere across its row**, so off every
  marker, the field painted last is the one taken.

**A carry can follow a finger** (`carry.h`). For a mouse, a panel following the
pointer is exactly right. With two fingers down, the system's emulated mouse
follows the *first* one -- the finger holding still -- so a window pulled out by
the second would never move. The panels' carry code asks `carry::` for where the
pointer is and whether it is down, which is the mouse unless the app has handed
the carry to a finger.

Known edge: holding a *number line* with a finger presses on it, and a press on
the bare axis makes a new value. So the tablet gesture on a number line creates a
stray value before the pull begins; plots and space windows have no such press.
Untested on a touchscreen, since there is none here to test it on.

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

**And two values are made into a third by holding both.** A finger on each of two
vizes means *these two, together* -- and that is a different sentence from what
either finger means alone, so the first thing it does is stop them being what
they were about to be.

- **Holding two freezes them.** One finger on a handle drags it; two fingers on
  two handles is not two drags. So whatever the first finger had already started
  doing to a value is *undone* -- it had not chosen anything yet -- and neither
  value moves for as long as both are held.
- **A wire follows each finger back to its viz**, in that value's colour, so
  what is being held is never in doubt. It leaves the viz rather than the
  finger, by the rule that already governs every other wire here: the window is
  the value, and a finger is only where your hand is.
- **Bring the fingers together and a little menu appears where they met**, with
  `+` and `*` in it, and both wires now run to it rather than to the fingers.
  **Keep a finger down**: slide it onto an option and lift, and the line is
  written -- `float c = a * b;`, at the caret, by the same rule that puts a
  value on an axis. Lift with the finger nowhere in particular and the menu
  goes. The menu belongs to the gesture that raised it, so letting go of the
  gesture puts it away.

**"Together" is relative, and generous.** Two fingertips side by side already
report their centres forty or fifty pixels apart, so a threshold in the tens is
one no hand can reach -- it was 46 to begin with, and the gesture could not be
finished. What is asked for now is that the distance be *halved*, floored at 90
pixels and capped at 200, so it is the same squeeze whatever the screen and
whatever the hands.

**And a lost contact does not lose the gesture.** A digitizer asked to track two
fingertips that are nearly touching will drop one and hand it back under a new
id -- which is to say it does this exactly when the gesture is succeeding. So a
finger is followed by its id where that still exists and by where it last was
otherwise, and the question "have they met" is asked *before* the question "are
both still there", so that losing one at the moment they meet still counts as
meeting.

**Both values must be of the same box.** Values from two different boxes are
values of two different programs, and no line could name them both -- so two
fingers spanning boxes are not a gesture, they are a pinch. Lifting before the
fingers meet is likewise nothing: it was not said.

The name is the first letter nothing in the box is using, and the type is `float`
unless one of the two is a function, in which case the answer is one too.

So these gestures are each other's inverse. A variable is pulled *out* of the
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
| `RealFunction.*` | the body the app writes for a function nobody defined, and the key a function of reals is drawn under |
| `Clifford.*` | a whole family of types: it writes out the structs and products of Cl(p,q,r) on demand |
| `RealFunctionViz.*` | how one is shown and how an example of one is made -- the *viz*, plus the plot it is shown in |
| `RealViz.*` | the same two things for a real: the number line (a *dw*) and the marker on it (a *viz*) |
| `Color.*` | the alias, `hsv`, an example colour, and the conversions the window draws with |

There is no `Real.*`, because `float` is already GLSL and needs no definition.
That asymmetry is the point of splitting a type from its viz: only some types
are new vocabulary, but all of them need showing.

**Choosing a value and showing it are one job**, which is why the example
generator sits with the visualization rather than with the type. Both halves
answer the same question: make this abstract declaration concrete enough to look
at. The generators are declared in `types/examples.h`, apart from the viz files,
only because the toolchain has to call them and the toolchain does not include
raylib.

What is still *outside* `src/types/` and would have to move before a type could
really be dropped in: `mathline` knows the type names it reads and their LaTeX,
and `mathprogram` has a `Kind` enum with one entry per type. Those are
the seams to cut when this becomes a real extension point.

### Functions nobody wrote a body for

This is one of the things the app is *for*, and it is easy to lose sight of:
**a function you say things about without saying which function it is.** Much
of the mathematics this is aimed at -- PDEs above all -- is about functions in
general: `u` satisfies this, its derivative is that. So `u` is declared and
never defined, exactly as `a` is declared in `float a;` and never given a
number.

It is written the way GLSL already writes a function with no body, as a
**prototype**, and it reads as the arrow and nothing more -- no brace, because
nothing has been said about what travels along it:

```
float k;                                     k \in R
float u(vec3 q);                             u : R^3 -> R
float v(vec3 q) { return u(q) * k; }         v(q) := u(q) k
```

**The body is injected, exactly as a value is.** A program cannot run a
function in general, so the app chooses one, and the choice is a *body* written
for that signature (`ExampleFunctionBody`, in `types/RealFunction.*`). It is one
of four recognisable shapes -- a wave, a parabola, a bump, a line -- of whatever
single number the parameter offers: the parameter itself for a real, how far out
it is for a point, its size for a complex number; and one shape per component
when the answer has several. Deterministic in the function's name, like every
other example. Hovering the prototype shows it in blue, standing where the
semicolon was; editing hides it, because it is not yours. Everything downstream
treats the result as a function like any other: it gets its plot, its field, its
picture or its volume, and other functions may call it.

**Unless the box gives it a body.** A prototype followed anywhere in the same box
by a definition of the same function is not a function of its own. It is a
forward declaration, which is what GLSL already calls it: it is hoisted as
written -- which is what lets a function *above* that body call it -- and nothing
is injected. So a box can begin `float u(vec3 q);`, say things about `u`, and
later say what `u` is, without a line being deleted.

**There is no type name for any of this.** There used to be one: `RealFunction
f;`, then `RToR f;`. It could only ever name one arrow, and given a body it said
nothing the signature did not. The signature *is* the type, so a declaration
declares a value and a prototype declares a function, and those are two
different shapes of line. `RToR` survives only as the key a function of reals is
drawn under, which is what a `Viz(RToR)` box would name.

A multivector parameter offers no single number to shape the answer with, so a
function *of* one is given a constant. That is a gap, not a choice.

### Clifford algebras

The third type, and the one the tool is most wanted for. It is this app's
answer to **GAmphetamine.js**, and it works the way GAmphetamine does: the
algebra is not a library that gets included, it is **generated** -- concrete
structs and concrete products between them, fully unrolled, no runtime
dispatch.

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

**The signature is written the way it is spoken.** `Cl(3,0,1);` on a line
of its own says p generators squaring to +1, q to -1 and r to 0 -- and that line
is a statement *about* the canvas rather than one *in* a box, so it leaves a
blank behind rather than being compiled, and it reads as the sentence that names
the algebra rather than as code. Every box on the canvas works in it (see
*Sharing between boxes*). A canvas that says nothing works in Cl(3,0,1), 3D
projective geometric algebra, which is what these tools are mostly for.

**`e0` is the null generator, and only an algebra with one has an e0.** The
generators are counted from one, the way the axes of a space always are, so
Cl(2,2) is e1 e2 e3 e4. A degenerate algebra puts its null generator first and
calls it e0, the way every PGA text does, so Cl(3,0,1) has e0 null and e1 e2 e3
positive -- and the rest are numbered the same with or without it. Two or more
null generators would be e0, e1, ... with the rest following on; nothing uses
one, and it has not been thought about. Numbering from nought everywhere was
the old rule, which gave Cl(2,2) an `e0` squaring to +1 and made `e0` mean two
different things. The number of null generators may be left off when it is
nought: `Cl(2,2);` is `Cl(2,2,0);`, and both read as `Cl_{2,2}`.

**Degeneracy is not a special case.** A generator that squares to zero takes
terms out of the products like any other metric, and the dual is a *complement*
rather than a multiplication by the pseudoscalar -- which is the only way it can
survive a pseudoscalar that squares to zero. That is what makes Cl(3,0,1) work
at all, and it is the reason the dual is defined by `b ^ dual(b) = I` rather
than by a product.

The operators are GAmphetamine's, because that is what the notation already is,
and each has a reading as mathematics:

| code | means | written |
| --- | --- | --- |
| `a * b` | geometric product | juxtaposition, `ab` |
| `a | b` | inner product | `a \cdot b` |
| `a ^ b` | outer product | `a \wedge b` |
| `a & b` | regressive product | `a \vee b` |
| `~a` | reverse | `\widetilde{a}` |
| `a >> b` | sandwich | `a b \widetilde{a}` |

**`a >> b` is the sandwich: a versor acting on something.** It is written out as
what it is -- `a b ~a` -- because mathematics has no symbol for it and the
shorthand is worth spending on the page rather than carrying through.

Two things it is not a plain triple product:

- **The sign is superconjugation.** `a b ~a` where either side is even, and
  `-a b ~a` where both are wholly odd. Without that an odd versor acting on an
  odd thing hands back the negative of what was meant.
- **It gives back what it was given: `a >> b` has b's type.** A versor action
  preserves grade -- that is what a versor is -- but the type system works from
  which grades a product *can* make, and cancellation is not something it can
  see: `Even >> Bi` could be a scalar, a bivector and a quadvector as far as the
  grades go, and is a bivector for every versor anybody writes. So the rest is
  dropped. The price is that a non-versor on the left has those parts taken off
  it quietly, and the products are still there to write out by hand where that
  matters.

It is checked against the blade algebra rather than against itself: a quarter
turn takes e1 to e2, and twenty random `Even >> Bi` pairs agree component for
component with the triple product worked out from `BladeProduct`.

**A narrower value fits where a wider one is asked for.** `Even p = 1.0 + b;`
makes a `ScalarBi`, and every grade of that is a grade an `Even` has, so there is
nothing to decide and nothing to lose. GLSL has no implicit conversion between
structs and no constructor to overload, so the algebra writes a widening function
per type (`toEven`, `toBi`) and the declaration is what calls it -- at the top of
a box or inside a function, and for an assignment further down as well.

**Only upwards.** A wider value put into a narrower one would be dropping grades
silently, and losing part of an answer is not a thing to do quietly: that is
still an error, and `grade2(...)` is how you say you meant it. The one place
grades *are* dropped is the sandwich above, where the whole claim of the operator
is that they cancel.

**The reverse of a sum wears one long tilde.** `~(a + b + c)` reads as
`\widetilde{a + b + c}`, and the tilde spans the whole of it -- a small tilde in
the middle of a sum would say it was the reverse of the `b`. That is harder than it
sounds, and how it is done is worth knowing:

- **The fonts already draw it at several widths.** The text tilde, then Size1 to
  Size4, then the AMS one -- half an em up to two and a third -- are TeX's ladder
  for exactly this. The drawing nearest the width wanted is the one used.
- **It is drawn with that drawing's stroke, not stretched.** Every typesetter
  stretches the widest glyph sideways past its end, which thins the stroke on its
  slopes and leaves the ends blunt and heavy. Here the stroke itself is read off
  the outline -- where its middle is and how thick it is, at a hundred points
  across -- and redrawn to exactly the width wanted, as thick across at each point
  as the glyph was: the same pen, travelling further (`FontStore::Stroke`,
  `WideStroke` in `tex_layout.cpp`).
- **Past the widest drawing it grows taller too**, as the 0.35 power of how much
  longer it is, up to 1.8 times. A long wave at a fixed height flattens into a
  line with a kink in it and stops reading as a tilde at all.
- **It is geometry, not a bitmap**: a filled shape the window's multisampling
  smooths like any other, so it is as sharp at ten ems as at one, at any zoom.

`\widehat` has the same ladder in the same fonts and would take the same path in
one word, but nothing writes one yet, so it has not been switched on unlooked at.

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

**Every operator is also a named function.** GLSL has no operator overloading,
so `a * b` cannot be a geometric product there as written. The answer is
GAmphetamine's own alternative spelling, and the operators are rewritten into it
on the way to the shader (*Operators*, below):

| operator | function |
| --- | --- |
| `a * b` | `gp(a, b)` |
| `a \| b` | `ip(a, b)` |
| `a ^ b` | `op(a, b)` |
| `a & b` | `rp(a, b)` |
| `~a` | `rev(a)` |
| `a + b`, `a - b`, `a / k` | `add`, `sub`, `div` |
| `a >> b` | `sw(a, b)` |

They mean the same thing and they *read* the same -- `op(p, q)` sets as
`p \wedge q` exactly as `p ^ q` does -- so a box may use either spelling.

Three things about the generated GLSL are forced by what the language lacks. It
has no default member initialisers, so every function fills in every component
of its answer rather than only the ones with terms; it has no forward
declaration of a struct, so the types are emitted in an order where each is
complete before it is mentioned; and it has no constructor to overload, so a
value is made by listing every component, `Bi(0.4, 0.0, ...)` -- which is what
the injected example value is.

**The precedence is the mathematician's, not C's.** Every algebra operator binds
tighter than `+` and `-`, so `a + b | c` is `a + (b | c)` -- which is what it
reads as. C binds `|` looser than `+`; the rewrite is where the order is
decided, so it is decided the way the page reads. Within the algebra: `*` and `/`
bind tightest, then `^` and `&`, then `|`, then `>>`.

**And both halves follow it**, which they did not. The rewrite that makes the
shader (`glslops`) had the mathematician's ordering first; the reader that makes the LaTeX (`mathline`) was left on C's, where the
products bind *looser* than the sum. So `a + b | c` ran as `a + (b | c)` and read
as `(a + b) | c`: the page and the shader disagreed about what the line meant,
which is the one thing this program must never do. They are one ordering now, and
a new operator goes in both.

**Only what a box uses is written out.** The default is the geometric and inner
products for `Una`, `Bi` and `Even`; anything else a box mentions is
added, and so is anything the products between them turn out to need. That
closure is over *products* only. Closing over addition as well would ask for the
union of every pair, which is the entire lattice of grade-sets -- 30 types and
three and a half megabytes of code nobody wanted. A sum of two unlike things is
written when it is wanted: name the type and the operator appears with it, and
where the exact type was never asked for the sum widens to the nearest one that
exists rather than going missing.

**The algebra is considered, not named.** The line that chooses it is not a
statement in the program, it is the sentence a text opens a section with, and it
is set as one: `Cl(3,0,1);` reads

>   Consider Cl_{3,0,1}

and every type after it names the algebra outright:

| type | reads |
| --- | --- |
| `Una`, `Bi`, `Tri` | an exterior power, `\bigwedge^{k} Cl_{3,0,1}` |
| `Even` | `Cl_{3,0,1}^{+}` |
| `Odd` | `Cl_{3,0,1}^{-}` |
| `Multivector` | `Cl_{3,0,1}` |
| `ScalarQuad` and other compounds | the direct sum of their parts |

It used to bind a letter -- `A = Cl^{3,0,1}`, with a script A so an ordinary
variable called `A` would not collide -- and say every type in terms of `A`. That
is a definition, and there is nothing to define: a name for the algebra is one
more thing to look back up, and the full name costs a subscript.

**The signature goes under, because the grading goes over.** The `+` and `-` are
the Z/2 grading, which is what even and odd *are*, and a superscript already
holding the signature leaves them nowhere to go. That superscript says something
different from the one on `\bigwedge^{1}`, which is why a single grade always
wears its wedge rather than being shortened to the algebra.

**The exterior power is of the space underneath the algebra, and is written of
the algebra.** Those are two different objects: the quadratic space has
dimension p+q+r, the algebra over it has dimension 2^(p+q+r), and only the
algebra has an even and an odd half. Writing the algebra is what makes its `+`
mean anything; the price is that `\bigwedge^{k} Cl_{3,0,1}` is then an exterior
power of an algebra, which is not literally what is wanted.

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

### Operators

GLSL has no operator overloading and never will. It has *function* overloading
-- `gp(Bi, Bi)` and `gp(Tri, Tri)` are two functions of one name, resolved by
argument type -- and that turns out to be enough, because it means **the rewrite
needs no types**:

    a * b -> gp(a, b)     a ^ b -> op(a, b)     ~a -> rev(a)
    a | b -> ip(a, b)     a & b -> rp(a, b)
    a + b -> add(a, b)    a - b -> sub(a, b)    -a -> neg(a)
    a / b -> div(a, b)

It is purely syntactic (`glslops.*`), and GLSL resolves which product was meant
by itself. The same names are written out for the types GLSL already has --
`float gp(float, float)`, `vec3 add(vec3, float)`, and so on, including the
mixed int/float pairs so nothing is ambiguous -- so a blanket rewrite is safe on
ordinary scalar and vector code and nothing anywhere has to infer a type.

**The operators always mean the algebra.** `~ ^ & |` are never bitwise.
Deciding otherwise would mean knowing the type of every expression, which is a
type checker; declaring it costs one rule and no machinery. Boolean work uses
`&&` and `||`.

**`!` is GLSL's own boolean not.** It used to be the dual, as it is in
GAmphetamine, and that cost `!` its ordinary meaning for something rarely wanted
-- even in PGA, where the dual is most at home. So the operator went: the dual is
written `dual(a)`, and still reads as `a^{*}`. It has to stay as a function,
since the regressive product is defined through it.

**Line breaks survive the rewrite**, so a driver diagnostic still lands on the
line that was written.

**`return` is a keyword, not a name.** It took a while to notice, because
`return sin(x) * 2.0;` is fine: the trouble is only `return (a) * b;`, where an
identifier is followed by a bracket and the rewrite reads a *call* of `return`.
So it is lifted out before the climb starts, and the operator then wraps only
what is being returned.

**A function is a GLSL function.** `float w(float x) { return sin(x); }`
-- written the way GLSL writes one, because that is what it becomes. It is
hoisted above `main`, since GLSL has nowhere else to put a function, and the
hoisted block is padded to one line per line of the box so the two halves keep
the same shape.

So functions compose by calling: `h = f + 2g` is
`float h(float x) { return f(x) + 2.0 * g(x); }`, which is honest about what the
GPU will do and costs nothing at run time. A function's *constants* can then be
uniforms, which is the whole point: dragging a knob reshapes a function at
display rate with nothing recompiled.

**A box's lines appear twice in the shader** -- hoisted above main, and again
inside it -- so mapping a driver's line back to the line that was written takes
two starts rather than one offset (`compiler::ShaderLines`). The body is tried
first, since an error there is about what a line *does*, which is the commoner
thing to be wrong; a line in neither range is in scaffolding nobody wrote, and
the best that can be said is the first line.

### One language

**A box is GLSL.** It used to be C++ first and GLSL if C++ refused it, with a
process launched to print the values back; that is gone, along with the whole
toolchain that served it, and nothing in this app launches a compiler.

**Every value comes back the same way**, through the trace buffer: a real is one
number in it, and a multivector is one per component.

`process.*` stays, because `ffprobe` still has to be run and read: `Capture` runs
a child and reads what it says to the end.

### What a shader compile costs

**A box builds one shader**, with `sbMode` saying which end to use -- its
picture, the report of its values, the functions it draws, the samples it feeds.
It used to build three from the same lines, each with its own driver overhead,
and folding them into one took `realfunction` from 43.8 ms to 4.5 and `glsl`
from 20.4 to 8.7. Two things make that free rather than a trade:

- **A function is drawn from the definitions alone**, so the modes that draw
  one return before the body. A field does not pay for the box.
- **The trace is written only when the box is being asked.** Guarded, because
  the same shader draws the picture, and without the guard every pixel of every
  frame would race to write it.

It also means the vertex half is **ours** rather than raylib's default, because
reporting a value asks about one given point rather than the point being drawn,
and a varying cannot be shadowed by the local that chooses between them.

**What costs now is generated algebra.** `pga` is about 60 ms and `mum grids`
205 ms, cold: a Cl(3,0,1) or Cl(2,2) box is well over 100 KB of GLSL in a single
shader. The driver's cache saves a recompile of exactly the same source and of
nothing else, which is the whole reason a changed number must never change the
source. The lever is still there: emit only the products a box actually calls,
rather than every product between the types it mentions.

**KHR_parallel_shader_compile would have been the answer and is not available
here.** The driver advertises neither `GL_KHR_parallel_shader_compile` nor
`GL_ARB_parallel_shader_compile` in a 4.3 core context -- checked against the
extension string, not inferred from a missing entry point, and sanity-checked
against `GL_ARB_compute_shader`, which it does advertise. The detection is in
`glext.*` and will use the extension wherever one turns up.

### A function is drawn by being called

**A curve covers part of a pixel.** The band either side of the curve used to be
in or out -- `discard` beyond it -- and in or out is a staircase that
multisampling cannot touch. So it fades over one pixel, using `fwidth` to turn
a distance in value units into one in pixels, and weighs the same as before.


A function is GLSL, so a window drawing one *evaluates* it, per pixel, in a mode
of the box's own shader that calls whichever function it is asked for.

That is the whole point. **A function's constants can be uniforms**, so dragging
a knob reshapes a curve at display rate with nothing rebuilt. And there is no
sampling window, so zooming a plot out never runs out of curve.

One shader, two shapes, chosen by a mode:

- **A field across a number line.** The input is the axis position; zero is
  transparent, +1 opaque viridis green, -1 opaque viridis yellow, clamped
  beyond.
- **A curve on a plot.** The input runs up, so the curve is where the output
  actually is what the function says: `|here - f(input)|`, divided by the slope.
  Dividing matters -- a fixed horizontal band thins to nothing exactly where the
  curve is steepest, which is where it is most worth seeing -- and the slope is
  taken from one step either side rather than from any calculus.

KNOWN: a curve still breaks up slightly where it is very steep *and* sharply
curved, because the slope is measured over a fixed step. A smaller step, or a
proper distance estimate, would fix it.

### A function is drawn by being fed

A value has a place to be; a function does not. It is a machine, and the only
way to see a machine is to put something through it. So:

**A spread of samples of the input space is drawn in whatever window that type
goes in, every one of them is put through the function, and the answers are drawn
in whatever window the output type goes in** -- which may well be the same
window. A real to a `Tri` of Cl(3,0,1) is a row of beads along a number line and
a constellation of points in a space window, and those two runs are the same
length and line up index for index.

**A sample is drawn exactly as a value of its type is**, with the same marker at
the same size. Drawing them smaller was tried and taken out: it was one more
thing for every viz to know about, for no gain.

**Pointing at either end boxes both and draws a wire between them.** Pointing at
an answer is as good a way of asking about a sample as pointing at the sample is,
so both ends are searched, and both light up -- the whole claim being made is
that they are two halves of one statement. The boxes are drawn twice, a pale
halo under the value's own colour, because they land on a dark card, on the pale
canvas and on a sky-blue window full of the very dots they are pointing at, and
one stroke cannot read on all three. Nothing here can be grabbed: a sample is a
place the function was *asked*, not a value anybody chose.

**Which function is shown is the one the box is being read inside.** Put the
caret on a signature, on a `return`, on either brace, or on any line between
them, and that function is the one that gets fed. It is the same "executed up to
here" that decides what every other window shows, asked of a span rather than of
a line -- and a box that is nothing but a definition is read inside it by
default, since the end of such a box is inside the function. Read anywhere else
and the spread goes, because nothing is being asked.

**A function is recognised by its shape, not by a type name.** `Tri p(float t)`
is a function exactly as `float f(float x)` is, and `float f(float x);` is one
too, with the app supplying the body. A function runs from its
signature to the brace that closes it, so it may be as many lines as it likes,
and every line of it is hoisted -- which is what makes "the caret is inside this
function" a question with an answer.

**The special cases win where they apply, and there will be more of them.**
Reals to reals is already drawn two ways -- a curve on a plot, a field on an axis
-- and neither of those is sampling. Making this general will be **an ongoing
accumulation of special cases** rather than one rule: a function to a `Tri` will
one day want to be a curve through space rather than a row of beads, a function
of two reals will want a surface, a function to a `Bi` will want lines rather
than points, and every one of those is a thing somebody has to write. Sampling
is the **fallback** -- what a function gets until something better has been said
about its shape.

### Where the spread comes from

`Samples(float, 24);` on a line of its own makes a box a **sampler**: it says how
a type's space is laid out, and how thickly, rather than declaring a value of its
own. It is the third sigil and it behaves like the other two -- a statement
*about* the box, never compiled, leaving a blank line behind, and set as
mathematics: `S_{24} \subset \mathbb{R}`.

Its body lays samples down, exactly as a viz's mesh half lays vertices down, and
for the same reason: a spread of points is geometry, so it is a compute shader
writing into a buffer through an atomic counter.

```
Samples(float, 40);
for (int i = 0; i < sbCount; ++i)
  pick(10.0 * (sbRand(i) - 0.5));
```

`pick` rather than `sample`, which is what the rule behind `vertex` and `line`
would have called it: GLSL keeps `sample` for multisampling and will not have it
as a name. `sbCount` is what the sigil asked for. `sbRand(i)` is a hash rather
than anything random -- the same integer always gives the same number, because a
canvas that spread itself differently every frame would be a canvas nobody could
point at anything on.

**A type with no sampler box gets the spread the app was born with**: authoring
one is how you take it over, not how you switch it on. Only the reals have one -- 24 of
them, evenly across [-4, 4] -- because a spread of reals is the one thing the app
can guess at. What a spread of *points* should be is a question only the person
drawing it can answer.

**The spread gets a row of its own**, like a function's field and for the same
reason. The box's own values are read against a scale framed around them, and a
function's domain is not one of those values: it is where the function is being
asked, which is a different question and wants its own answer. So that row frames
itself around the spread, the first time the spread exists.

**Feeding happens in the box's own shader**, in a mode of its own, one fragment
per sample -- so nothing extra is compiled, which is the rule the three modes
already followed. Unlike drawing a function it comes *after* the body: it runs
once per change over a couple of hundred fragments rather than per pixel per
frame, so it can afford to be exact, and a fed function may read anything the box
computed rather than only what its variables were declared with.

### A box's variables are globals

GLSL has nowhere to put a nested function, so a function the user writes is
hoisted above `main` -- and **a hoisted function cannot see a local**. So a box's
variables are globals, declared in the prologue before the functions, and the
line that used to declare one now only assigns to it.

`h(x) = f(x) + a w(x)` is exactly what this is for: a function whose shape a knob
reshapes, which is the whole reason a function is GLSL at all. It did not work
before this was written down. `realfunction.sketch` did not compile at all --
`undefined variable "a"` -- and so drew no curves and no fields, silently,
because a box whose diagnostic nobody is looking at fails quietly.

Every way of drawing a function returns before the body -- a curve, a field, a
picture, a surface, a volume -- so before any of them every variable is set to
the value it was *given* where it was declared. *Before any of them* is the part
that went wrong: the picture, the surface and the volume were added above that
line, and drew with every variable at nought, so their knobs did nothing and
nothing complained. That is the
right thing for them to go on: what a knob moves is exactly one of those
literals. A function reading a variable the body *computes* is a different
matter, and a drawing of it reads whatever the declaration left -- the body is
not run per pixel, on purpose.

**A free real inside a function gets a knob too**, and so does one given a typed
number, `float k = 0.1;`. `float k;` with no value is the same abstract statement
wherever it is written, and it is answered the same way: an injected literal
standing behind a uniform, which a hoisted function can see as readily as `main`
can. That it did not used to was an oversight rather
than a decision -- a line inside a function was hoisted as written, so `k` was
simply left unset. It stays a local of its function, not one of the box's
globals, and nothing traces it: the one number it can be is the one its knob
chose.

**A derived one does not**, and that one is a decision. `float m = sqrt(ip);`
inside a function has a different value for every input the function is called
with, so there is no single number for a marker to show. What a function does
with its input is shown by feeding it, not by a marker.

**What a function returns is widened into the type it says it returns**, exactly
as a declaration is: `return 1.0 + b;` out of an `Even` function makes a
`ScalarBi`, and it fits. Every `return` on a line, since `if (c) return a; else
return b;` is one line.

This applies to a box's *variables*, which is to say the declarations the app
understands. `vec2 q = vec2(0.5);` is not one of those -- it has no
mathematical reading and is passed through verbatim -- so it stays a local, and a
function cannot see it. A viz box and a sampler box have no variables at all, so
their declarations stay the locals they were written as.

### Reading a box's values back

**A box's body is not a field.** Inside a fragment shader a variable *could*
have a different value at every pixel, and for a while a box's body could say
where it was being read -- `fragTexCoord`, the pointer over the picture -- so
that `r = length(p) * k` meant something different under every pixel. That is
gone: a picture is a function of the plane, and what varies across it is that
function's argument. The body runs the same in every pixel that reports it, and
every one of its values is a number.

A read is a stall, so it only happens when something it depends on has moved --
a knob, or a clock.

**The app supplies the ends; the box is the middle.** The fragment-shader
interface, one uniform per literal, the box's variables as globals, and a
`void main() {` ... `}` round the lines. The shader is assembled **line for
line** with the box -- a line the app handles itself leaves a blank behind
rather than vanishing -- so a diagnostic lands on the row it is about.

Compiling means talking to the graphics context, which `mathprogram` knows
nothing about and must keep knowing nothing about. So the program assembles the
source and stops in `State::Shading`; the host compiles it on the thread that
owns the context and calls `ShaderCompiled`. Driver logs are worded differently
by every vendor, and the two shapes in the wild are parsed in
`compiler::ParseGlslLog`.

### What goes in, and what comes out

**A box draws because it defines a function of two variables.** It used to draw
because its text mentioned `finalColor`, which is a fact about the file rather
than about the mathematics: it said a box was a shader, when what is worth
knowing is that a box computes a *map from the plane*, and a picture is what one
of those looks like.

So both halves of a function's type are read, and between them they decide
everything:

**The input decides the window -- and it is a different *kind* of window each
time.** One variable is a curve on a plot. Two is a square of pixels. Three is a
volume, looked *through* rather than at. They are separate kinds rather than one
window in three moods, because **a display window is largely its camera**, and
these three have three different ones:

| | camera |
| --- | --- |
| R to R, on a plot | none: there is nothing to turn |
| R2 to R3 or C, a picture | none either: a colour has nowhere to go but its pixel |
| R2 to R, a surface | orthographic, and it turns |
| R3 to anything, a volume | perspective, and it turns |

The projections are not a preference. **A surface is a graph**, and a graph must
draw the same quantity the same size wherever it stands -- under a perspective
the same height would look different depending where on the sheet it was, which
is the one thing a graph must not do. **A volume is a place you are looking
into**, and there the near things being bigger is exactly what says they are
near; take it away and a cloud has no depth in it at all. Same reason, opposite
answers, because they are not the same kind of thing.

So the wheel means different things over them too: over a surface it changes the
patch of the plane on show, and over a volume it moves the eye, because under a
perspective camera those are two separate things and moving is the one that says
something -- going in takes you *into* the cloud.

**Dragging right turns the right-hand side towards you**, the way a hand on a
globe does, in every window that turns. It used to be the other way about, which
is the hand *behind* the globe pushing it -- a real convention, and the wrong one
for a thing you are holding rather than standing inside.

**In every window that turns** is the whole of it, and it was not, for a while:
the surface and the volume were turned round and the space window was left
behind, so two of the three agreed and the third did not. The sign of the drag
is not what to compare -- all three add it -- because a camera built by rotating
a point about +y orbits the opposite way from one built with a `-sin(yaw)` in
its eye, and those are the two constructions in the tree. What has to match is
where the camera ends up.

**The output decides the colour.** A real is viridis, signed, and it is the same
map a real gets in every other window, so green means the same number wherever
it is read. A `vec3` *is* a colour and is used as one. A complex number has an
argument as well as a size, so it gets the two channels that answer to that:
hue for the argument and lightness for the size, black at a zero and white as it
runs away. That is domain colouring, and it is why the next paragraph exists.

| | R1 out | R3 out | C out |
| --- | --- | --- | --- |
| **R2 in** | a surface | a picture | a picture |
| **C in** | -- | a picture | a picture |
| **R3 in** | a volume | -- | a volume |

**`Complex` is its own type, and not `vec2`.** They are the same two numbers and
they are not the same space: one is a *place* and the other is a place with a
multiplication on it, and a function of the second is worth drawing differently
from a function of the first. Nothing has to guess which was meant, because the
box said. It is written out for the shader only when a box mentions it, exactly
as an algebra is, and its products answer to the names the operator rewrite
produces -- so `z * w` is a complex multiplication, `~z` is the conjugate (which
is what the reverse of a one-generator algebra is), and there is nothing extra
to learn.

### Colours

**`Color` is a vec3 that means a colour.** The shader is told `#define Color
vec3` -- only when a box mentions one, like an algebra -- and nothing else, so
`.rgb`, swizzles, `mix`, `clamp`, `pow` and every other built-in come for
nothing, and so does the operator rewrite, which already writes `add`, `gp` and
the rest for vec3. `hsv(h, s, v)` makes one, hue in turns.

**A new struct when the algebra differs; an alias when only the meaning does.**
`Complex` had to be a struct because `z * w` is not a component-by-component
product. A colour has no algebra of its own -- adding two is adding light,
scaling one dims it, multiplying two is a filter -- so what makes one different
is only what it means, and meaning is exactly what the app reads off the declared
type. This is not the aliasing *A renamed spelling is renamed* forbids: that is
two names for one thing, and a colour and a vector are two things GLSL stores
the same way. The price is that the driver's messages say vec3.

**Its knob is three numbers behind one `vec3` uniform** -- `Color c;` and a typed
`Color c = Color(0.2, 0.5, 0.9);` alike, the second rewritten as it is dragged.
So a `Literal` carries `components` as well as its one `value`, a colour is saved
as three records, `val c.r` and so on, and a colour shared from another box
travels as a vec3 uniform. Derived colours are traced part by part, the way a
multivector's components are.

**There is no reading as mathematics for one yet**, by decision: a line declaring
a colour, and the signature of a function taking or giving one, stay code. A
function of the plane giving a `Color` is a picture, as one giving a vec3 is --
and one giving a vec3 still is a colour picture, which was kept.

**Its window has three views, chosen from a dropdown** that appears to the right
of it while the pointer is on it -- where later choices about the window will go
too. The dropdown is asked before anything else on the canvas, since it stands
over whatever is beside the window, and drawn after every window for the same
reason.

- **Picker** -- a hue ring, and inside it a triangle whose corners are black,
  white and the hue it points at: what drawing programs have long used for
  choosing. Drag round the ring for hue, inside the triangle for saturation and
  value. Every colour sits on the ring at its hue; the one the triangle points at
  -- the one last taken hold of -- is also in the triangle. The triangle is a
  mesh with a colour at each corner, which the GPU blends in exactly the
  proportions the colour model says.
- **RGB cube** and **HSV cylinder** -- turned on the right button like every
  window with a camera, and orthographic, since a colour space is a graph of
  three numbers rather than a place to look into. A drag moves a colour in the
  plane facing you. The cube's edges and the cylinder's walls run through the
  colours they stand for.

**Colour is identity, and here colour is also the value**, so a marker's fill is
the colour it *is*, and its rim, a ring outside a gap, the colour saying *which*
variable it is. A colour outside 0..1 is drawn where it is, outside the solid.

**A function giving a colour, fed a spread**, puts its answers in the window as
markers the colour they are, ringed in the function's colour. They go inside the
hue ring in the picker, and at their place in the cube or cylinder. `C:\sketches\colour.sketch` has both.

### A surface is a picture you can turn

**R2 to R1 is the case with somewhere else to put the answer.** A colour has
nowhere to go but the pixel it is at; a *number* has a third direction going
spare, and using it is the difference between a picture of a landscape and the
landscape. So a real-valued function of the plane opens as the flat viridis
picture -- exactly what it would have been -- and the right button turns it into
relief. What looked like a texture turns out to have been a topographic map seen
from directly above, which is a thing you find out rather than a thing you are
told.

**Orthographic, where the window that shows points is perspective.** That is not
a preference. A `spaceDw` shows *points*, and there perspective is what tells you
which of two is nearer. A surface is a graph, and under a perspective the same
height would draw a different size depending on where it stood -- which is the
one thing a graph must not do. Two windows, two projections, and each for the
reason the other could not use.

The eye never goes below the plane. A height field from underneath is a hole,
and the flat picture is one end of the range rather than the middle of it.

### And a volume is one you look through

A function of three variables has no surface to stand on and no pixel to put the
answer at, so it is drawn as what it is: a quantity that is everywhere at once,
thick where there is a lot of it and clear where there is none. Viridis green
for positive and yellow for negative, or hue for a complex one, accumulated
front to back so what is nearer hides what is behind it.

It starts *turned*, where a surface starts flat -- a volume looked at square is
a wash with no depth in it, so there is no discovery to save for later.

**A surface's edges are supersampled, and only its edges.** Whether a ray hits
is yes or no, so a silhouette -- or a near hill hiding a far one -- is a
staircase, and not one multisampling can see. Four rays for every pixel would be
four times a 160-step march on every pixel every frame, which a large window
cannot afford. So each pixel marches once, and notices whether it was *next to
an edge*: either it passed within a pixel and a half of the surface before
landing, or it clipped a crest and came back out before it was that deep. Only
those pixels march four more rays across themselves and average them by
coverage. That is a line a few pixels wide along each edge, not the window.

A pixel and a half rather than one, because the march samples its approach a
dozen pixels apart along the ray and can see it a pixel further off than it was.
Both sides of an edge are looked for, because an edge found only from outside
blends half its pixels and leaves the other half stepped.

**Where the ray crosses is found properly, not stepped to.** A surface's colour
is the height *at the point the ray hit*, so how precisely that point is found is
not a question about smoothness -- it is a question about whether the colour is
right. Taking the step the crossing was detected on banded the surface along the
ray, and the bands moved with the camera, so a thing coloured by height read as
though it were coloured by distance. Eight halvings between the two steps puts
the crossing inside a pixel and the banding is gone.

**A volume's cube is drawn as twelve lines.** The walls are where the function
stops being drawn, and without them a cloud floats in nothing with no way to
tell how big it is or which way it has been turned. They are drawn on this side
rather than marched in the shader, because an edge is a line and lines are what
this side is good at -- the price is that the camera is built twice, once in each
language, and the two have to agree exactly or the box will not sit on the cloud
it is the box of.

**Both are marched in the fragment shader that is already there**, rather than
meshed. A mesh would want a vertex buffer, a second program and a draw path of
its own; the march is a dozen lines, it shares its eye and its film with the
other, and it is the same shape of loop either way -- stop at the first crossing
for a surface, keep going and accumulate for a volume. That the two windows are
separate kinds and the two marches sit in one shader is not a contradiction: a
window is where something is drawn and who may turn it, and that is exactly what
differs; how the pixels are worked out is the box's business either way.

A volume's march is clipped to its cube first, by the usual slab test, so its
steps are spent where something could be rather than on the empty space in
front.

**Closing is remembered per kind.** A box can define a picture and a surface at
once, and throwing one away must not take the other with it, so there is a flag
each rather than the one there used to be.

### Where a window goes before anybody moves it

**Everything a box *makes* flanks the card**: the first to the right, the second
to the left, the third under the first, the fourth under the second, and so on
down the two columns. Alternating rather than filling one side and then the
other, so a box with two windows has one on each side and reads as balanced.

Before this, every kind independently asked for the same spot to the right, and
a box with a picture and a space window put one on top of the other. The order
is settled once, where the panels are chained (`App::Init`), and each kind only
has to know how many slots the kinds before it have taken -- so no one of them
owns the arrangement and adding a fifth is one line. It lives in `dock.h`, with
one height for all of them, since two columns that do not line up in rows are not
columns. Most are square at that height; **a space window is 16:9**, the shape a
view into a space is looked at in, and its shared render target is 16:9 too so
the camera's projection comes out the right shape without a crop.

**The ones a box is *read against* are not in it.** Its number lines and its
plots stay under it, as wide as it is, because a number line is a scale the
box's values are read on rather than a picture the box produced.

Known edges: a box with no variables and no functions never reaches
the compiler, which is the rule that keeps ordinary notes from being built. A
box gets one window of each kind rather than one per function, and each shows
whichever function of its kind the box is being read inside.

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
set of axes, each curve in its own colour.

**A plot does not write its curves' names.** It used to, in the top-left
corner, and a letter in a corner is a second, weaker way of saying which curve
is which -- the colour already says it, and pointing at either end says it
properly, by lighting up the name where it is actually written.

### Non-goals

- **Not a theorem prover.** Nothing here decides whether a statement is true.
- **Not a LaTeX compiler.** Compiling arbitrary LaTeX to code is not on the
  table. Turning *some* LaTeX back into code would be nice and will need heavy
  assumptions about what the LaTeX means.
- **Not a general GLSL pretty-printer.** Rendering arbitrary code as LaTeX is not
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

renders as `a ∈ ℝ`, `b ∈ ℝ`, `c := (a+b)/2` (a real fraction), with one number
line docked underneath carrying a marker for each, in its own colour. Dragging a knob sets a uniform and the derived marker
follows. Clicking that axis anywhere but on a marker writes a new `float d;`
into the box, at a caret the box blinks while you are
pointing -- on its LaTeX as readily as on its code. Hovering a line turns it
back into `float a = -0.778;` with the injected part in blue; clicking it hides
the injection and edits the code. A variable pulled out of the code gets an axis
of its own, axes merge by dragging one grip onto another, and each zooms on its
own wheel.

Editing a box turns the *whole* box into Monokai-coloured code, with a
math/code toggle shown only while the box has the caret. Undo is Ctrl+Z;
Ctrl+left/right move by word and **Ctrl+backspace and Ctrl+delete take one**,
by the same reckoning of where a word ends, so what a Ctrl+backspace removes is
exactly what a Ctrl+left would have skipped over. At the start of a line there
is no word to take and the line break goes instead, which is what the plain key
would have done. Ctrl+C, Ctrl+X and Ctrl+V cut and paste the text
through the system clipboard.

**The pointer and the keyboard are two different claims.** Everything about
where a press lands is settled by asking each window in turn whether the pointer
is over it, and a window that answers yes has taken *the pointer* -- it has not
taken the keys. Typing used to stand down for whatever the pointer happened to
be over, which is a rule with an edge nobody can see: hold backspace, and a box
shrinking under it slides a number line up under a motionless cursor, and the
typing stops in the middle of a word for no reason the typist can point at. A
box with the caret in it keeps the keyboard wherever the mouse is sitting. The
one thing that does take the keys away is a video's trim time being typed into,
because that is a caret too and there cannot be two.

**And the caret cannot back into the sigil.** `\text` is not on screen while a
text box is showing its words, so a backspace at the visual start of the
paragraph used to eat a character of something invisible and turn the box back
into code with nothing to see. The floor the caret is clamped to is the floor
the deletes stop at. A line the compiler
complained about always reveals its code, so the red caret has real characters
to point at; the message sits in a white balloon in a proportional face.

**The balloon shows the first two errors, not just the first.** One mistake
usually has another right behind it -- a misspelled name reports wherever it is
used -- and seeing the second saves a round trip through the compiler to find
it. The caret points at the first, so the second is written with its own line
number; they are wrapped separately, so the two read as two rather than running
into one sentence.

**The log says almost nothing, on purpose.** raylib narrates every texture,
framebuffer and vertex array it loads and unloads, one line each, and on a canvas
that makes and throws away windows all day that is pages of it with nothing in
them. So the log is **filtered rather than turned down** -- turned down would not
work, because raylib tests the level *before* the callback runs and that callback
is the only way a box's shader errors are caught at all. What survives is a
whitelist of this app's own prefixes (`logging.h`), so a new one has to be added
there to be heard; `--verbose` turns the whole filter off and is what the sweeps
run with. A canvas that opens cleanly now prints nothing.

Canvases save and load: Escape menu buttons, Ctrl+S (straight back to where the
canvas came from) and Ctrl+O, plus `--open <file>` on the command line. The
format is plain text, one record per line, in `App::WriteCanvas`. The app
opens on the canvas last worked on, and **New canvas** in the menu gives a blank
one.

**And a canvas saves itself.** Whenever an undo step is taken -- whenever
something you did has settled -- and every few seconds while a box is being
written in, it is written back to the file it came from; and once more before
another canvas is opened and before the app closes, so nothing done in the last
moment is lost either. It uses the undo machinery's idea of a change on purpose:
**opening a canvas is not a change**, and neither is a build coming back or an
axis framing itself, so looking at a file never rewrites it.

A canvas that never came from a file is given one, in
`%APPDATA%/sketchbook/untitled`, named for when it was started, and it goes into
the recent list: nothing made here is ever only in memory. Ctrl+S on one still
asks for a name, and once it has one the untitled file is removed -- it is no
longer a canvas, only a stale copy of one.

**A save is written beside the file and moved over it**, so a crash in the middle
of one leaves the old file whole rather than a new one in pieces.

**A capture does not auto-save.** A run with `--frames` or `--screenshot` opens a
canvas, draws it and quits, and a window that pops up like that takes the
keyboard from whatever you were typing into. The first time that happened, a
stray keystroke became a box and was saved over the canvas it had opened.

Ctrl+S is still there, and still worth having: it is how an untitled canvas gets
a name, and it says "now" when auto-save would have said "in a moment".

**The menu has no title**, since everybody who pressed Escape knows which menu
it is, and its last button takes the app **fullscreen** and back. Borderless
rather than exclusive: the window covers the monitor and loses its title bar
with no change of display mode, so it is instant, disturbs nothing else, and
leaving puts the window back exactly where it was. **And not always on top**:
raylib's own `ToggleBorderlessWindowed` also makes the window topmost, so
alt-tabbing away brought another app to the front *underneath* it and clicks
went to a window you could not see. So it is done by hand -- title bar off,
window over the monitor -- and the app tabs away and back like any other. The button says what pressing
it gives you, like every toggle here, and pressing it closes the menu -- asking
for the whole screen is asking to look at the canvas.

**It is remembered.** Closed while filling the screen, the app opens filling it.
Like the recent list it is a fact about you rather than about a canvas, so it
lives beside that list (`%APPDATA%/sketchbook/settings.txt`, in `settings.*`),
is written the moment it changes, and never goes into a `.sketch`. A launch
that names a size with `--width` or `--height`, and a capture, get the window
they asked for instead.

**The menu lists the canvases opened lately**, most recent first, and one click
goes back to one. The point is that going back to what you were just working on
should not be a trip through a file dialog -- it is the commonest thing anyone
does with this program and it was the slowest.

Three things about the list. It is **a fact about you, not about a canvas**, so
it lives with the user's settings (`%APPDATA%/sketchbook/recent.txt`) rather
than in any `.sketch` -- a canvas has to stay movable, and a canvas carrying a
list of other canvases would not be. It is **written the moment a canvas is
opened or saved** rather than at exit, so a crash does not lose it. And a file
that **is no longer there is left out** rather than offered and then failing: a
place you cannot go back to is not a place. `recent.*` is raylib-free like the
rest of the layer that talks to the outside world.

**A press anywhere off the menu closes it**, and there is no "Esc to close" line:
Escape still works, but clicking away is what everyone tries first.

**A press the menu acted on belongs to the menu**, including the press that
closed it -- whether that was a button or a click off the panel. Without that rule, choosing a recent canvas spends one click on two
canvases: it closes the menu, and then the same press lands on whatever box of
the *newly loaded* canvas happens to be under the pointer and puts a caret in
it.

**A picture beside a canvas is named relative to it.** A canvas and the pictures
on it are one thing, and one thing has to be movable -- a folder of them carried
somewhere else, or a canvas lifted out of the folder it was in, should still find
what is on it. So a file under the canvas's own folder is written down as where
it is *relative to the canvas*; anything further afield keeps its absolute path,
because there is nothing else it could be said relative to. Absolute paths in a
canvas written before this still load, since an absolute path is recognised as
one and left alone.
`C:\sketches\demo.sketch` is a deck of six slides carrying one of
everything -- reals and their knobs, functions and their plots, a shader, an
algebra, prose, a picture and a video -- and is the quickest way to see whether
a change has broken something.

Number lines belong to the canvas rather than to a box, so an axis can carry
variables from two different boxes at once.

Functions work end to end as well. A box of

```
float a;
float f(float x);
float g(float x);
float h(float x) { return f(x) + a * g(x); }
```

renders as `a \in R`, `f : R -> R`, `g : R -> R`, `h(x) := f(x) + a g(x)`, with
plots and fields docked underneath. Hovering a prototype shows the body the app
chose for it, in blue. Dropping one plot on another puts both curves on one set
of axes in different colours, and `a`'s knob reshapes `h` as you drag it without
recompiling. A prototype of any signature works the same way -- `float u(vec3 q);`
is a volume of a function nobody wrote.

Every window is moved by the **rim** around its edge, which appears on hover.
Boxes are sized for the worst case across both readings -- **in height as well
as width**, so flipping a single line never makes the box taller. A row's height
is the taller of its two readings whether or not it is the one showing, which
takes the LaTeX extent from the cache rather than from what happens to be drawn.
(Prose is exempt: there the two readings are of the whole box rather than of a
line, and they do not even have the same number of rows.) So nothing changes
shape when a line flips or a value changes width. They snap to the grid and
push each other apart rather than overlapping.

A box that defines a function of the plane draws it in a square beside the
card, with its free reals wired to uniforms so the knobs reshape the picture
live. `C:\sketches\glsl.sketch` is an example. Pointing at the picture lights up
the function's argument, which is what the pointer stands for.

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
-- one laying down a mesh with `vertex(x, y)`, one placing its vertices
-- are what draws the markers on the number line, and editing either changes
them on the next rebuild. `C:\sketches\viz.sketch` is that, for the ring.
Everything else is still drawn in C++ (*How the vizes drifted*).

A function from a real to a point works end to end. A box of

```
Cl(3,0,1);
float k;
Tri p(float t) {
  float s = t * (1.0 + k);
  return Tri(cos(s), sin(s), 0.5 * t, 1.0);
}
```

gets a number line for `k`, a second row carrying a spread of samples of `t`, and
a space window with the answers in it. Pointing at a bead or at a point boxes
both and joins them. `C:\sketches\function.sketch` is that, with a
`Samples(float, 40);` box beside it saying how the spread is made.

Key files: `mathline.*` (one line of code -> LaTeX, and what shape a function
is), `mathprogram.*` (assembling a box into its shader, and the scaffolding a
fragment is wrapped in), `types/` (one directory per supported type: what it is,
how to make an example of it, how to draw it), `blocks.*` (per-line rendering
and editing, code and prose alike), `selection.h` (the lasso, and the eight
things every panel does with what it caught), `compiler.*` (what is left of the
toolchain: reading a driver's log), `dock.h` (where a window a box makes goes before anybody moves it),
`hover.h` (what the pointer is over, which is nothing while a gesture is held),
`carry.h` (what a carried window follows: the mouse, or one finger),
`busy.*` (the busy cursor while a shader compiles; Win32, raylib-free),
`logging.h` (what the log says, and mostly what it does not),
`shaderwindow.*` (a colour-valued map of the plane, flat), `colordw.*` (a
colour, as a picker, a cube or a cylinder), `surfacedw.*` (a
real-valued one, as relief under an orthographic camera), `volumedw.*` (a
function of three variables, looked through under a perspective one),
`media.*` (pictures and video as windows),
`video.*` (the ffmpeg pipe, raylib free), `resize.*` (the wheel-resize rule and
its snapping), `highlight.*` (Monokai), `viz.*` (the vocabulary above, and the
viz boxes the canvas defines), `filedialog_win32.*` (open/save dialogs).

## Open questions

Kept here so they are not lost between sessions. Answered ones move up into the
sections above.
- **A merged plot no longer reframes.** It used to open out until every curve on
  it was in view; the code that did it read a function's samples, and nothing
  has filled those since a function became GLSL, so it had been doing nothing
  for some time. It is deleted rather than left looking alive. Restoring it
  means asking the GPU what range a function covers -- which the machinery for
  feeding a function could now answer.

- **A box carrying an algebra is a 120 KB shader**, and the standing worry was
  that it would be the first thing to stutter. Measured, it is not: `pga` holds
  a mean of 1.0 ms a frame and a worst of 2.8 ms, and its compile lands in the
  load frame with every other shader on the canvas. The lever is still there --
  emit only the products a box actually calls -- but there is nothing yet asking
  for it.
- **A function takes one argument.** `Tri p(float t)` is recognised; anything
  with two parameters is not a function as far as the canvas is concerned, and
  stays code. A product of two spaces is the obvious next shape and wants a
  spread of pairs rather than a spread of points.

- **A sample cannot be grabbed.** Pointing at one lights the pair; nothing can
  be dragged, and dragging one would mean asking the function to be run
  backwards, which is the solver we are not building. Dragging the *sampler* --
  the box that made the spread -- is the thing that would actually make sense.

- **A viz cannot say how a function is drawn**, only how a value is. The
  sampling above is the app's, in the way the marker and the curve used to be
  the app's, and the same move is available: a box that says how a spread of
  answers is joined up would turn the row of beads into a curve through space.
- A paragraph wraps to a measure, but nothing yet lets one **flow into the
  next** -- a body that outgrows its slide simply runs off the bottom of it.
  Nothing warns, either.

- **Most multivectors have no window.** A `Tri` of Cl(3,0,1) or Cl(2,1,1) is a
  point in a space window, and every multivector is traced, but nothing shows a
  bivector, a motor, or anything else. That is the next thing, and it is the
  whole reason for the rest of it -- a bivector in Cl(3,0,1) is a line in space,
  and a motor is a screw motion, and both of those are things one wants to
  *see*.

- **A multivector has no knob.** Its chosen value is written into the source, so
  changing it would be a compile. One uniform per component would fix it, by the
  rule every real already follows -- and a colour's knob is exactly that, three
  numbers in `Literal::components`, so the machinery now exists.

- **A multivector shows no injected value.** A real shows its number and a
  function shows its body, but a multivector of Cl(3,0,1) has sixteen
  components, and a box sized for the worst case across both readings would be
  wider than the screen even with the injection hidden. So nothing is shown,
  which is honest but unhelpful -- the answer is a window that displays one,
  which is wanted anyway.

- **Video has no sound.** Nothing plays audio, and a clip chosen for what
  someone says in it is not much use silent.

- A picture is not part of the no-overlap rule -- boxes push each other apart,
  pictures do not. A caption sitting on a photograph is normal, so this seems
  right, but it means the two kinds of window follow different placement rules.

- **What is a knob for a function?** A real has a slider on the uniform behind
  its literal. A function nobody defined has its body compiled in, so there is
  nothing to drag -- the only way to change `u` is to give it a body. Options:
  make the *coefficients* of the generated body uniforms (`sin(1.4x)` gets a
  slider for the 1.4), or accept that choosing a function means choosing a body.

- **Composition.** The old TypeScript version left the same question open: "have
  a compose function, but how? Maybe `|`?". `f | g`, `f(g)`, or `compose(f, g)`?
  Whatever it is has to read as `f o g`.

- **The injected body is long**, and boxes are sized for the worst case across
  both readings, so a box with a prototype is wide even when the injection is
  hidden -- and for an answer with several components, a `Complex` or a `Tri`,
  it runs off the side of the screen. Live with it, let hovered code overflow the
  card, or give the body a row of its own?

- `vec2` as a *value*, rather than as what a function takes, is where a 2D knob
  would be needed.

- Should the prologue be visible anywhere? It is the app's half of the bargain
  -- what `Complex` and an algebra are, what operators exist -- and nothing in the
  program shows it to you.

- **Nothing brings a closed window back.** A variable whose number line has been
  thrown away is still in the code and still has a value; there is just no way
  to ask for its window again. The escape menu could offer it, or the name in
  the equation could be clickable.
