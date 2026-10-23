# sketchbook: the design

What this program is, and why it is the way it is. Every decision that has been
settled is here, each with its reason. How a decision is *kept* in the code --
mechanisms, file names, things learnt the hard way -- is in `TECHNICAL.md`.
Where the two disagree, this file wins.

---

## 1. What this is

An infinite pan/zoom canvas for **exploring mathematics through programming**.

Some code expresses sophisticated mathematics. Written the way a mathematician
would write it, it uses different symbols than the code does: `dot(a, b)` is
`a·b`. So: **behind every piece of LaTeX on the canvas there is some GLSL that
compiles.** The LaTeX is what you read; the GLSL is what runs.

The pivotal move is what happens to abstract statements. A mathematician writes

> x + 2 = y,  x, y ∈ ℝ

which is *about* real numbers in general. A program cannot be about reals in
general, so **we choose values for x and y**. Choosing is not a compromise, it is
the whole mechanism: concrete values are what make a thing visualizable, and
visualization is the point.

**The goal is a statement, not a feature list.** We are trying to get the
LaTeX <-> GLSL relationship down to something that can be written in a few
paragraphs. The core idea is still unsettled; it is the thing being figured out.

**Long-term goals** (context only; nothing is built for them yet):

1. Reading papers and textbooks that are short on illustrations.
2. A sketchbook for one's own mathematical thinking.
3. Presenting mathematics to other people.

All three want the same thing: LaTeX equations linked to live visualizations.

**Non-goals.**

- **Not a theorem prover.** Nothing here decides whether a statement is true.
- **Not a LaTeX compiler.** Compiling arbitrary LaTeX to code is not on the table.
  Turning *some* LaTeX back into code would be nice, and would need heavy
  assumptions about what the LaTeX means.
- **Not a general GLSL pretty-printer.** Only a chosen subset of code has a
  mathematical reading.
- **Not a solver.** A derived value cannot be dragged, because that would mean
  running the program backwards.

---

## 2. Principles

These are the rules everything else answers to.

### The code is the source of truth

The LaTeX is generated from the code by parsing a restricted subset. Nobody
hand-writes LaTeX, and **the page and the shader must never disagree** about what
a line means. A new operator, a new reading, a new precedence goes in both halves
or in neither.

### Declarations stay abstract; the app chooses values

`float a;` is valid input and reads `a ∈ ℝ`. To run, the app **injects** a value,
so it becomes `float a = 0.2385;`. Every variable gets one. A function is declared
abstract the same way, with a prototype -- `float u(vec3 q);` -- and what is
injected is a body.

The injected text is **not yours**: it is shown, in a distinct colour, when you
hover the line, and hidden when you edit. **The LaTeX never shows a number the app
chose** -- a number you typed is part of what you said; one the app chose is not.
Chosen values live in the knobs and in the hovered code.

Each supported type therefore needs two things: how it reads as LaTeX, and how to
make an example of it.

### A value never recompiles

**Moving a value never rebuilds anything.** This comes first because it is what
the program is for: a knob is how you find out what a statement means by pushing
on it, and pushing only teaches you anything while the picture answers at display
rate. A compile is a fifth of a second on a big box.

So every number a variable is *given* is a knob -- injected (`float a;`), typed
into a declaration (`float a = 0.4;`), or typed into an assignment further down
(`a = 0.4;`). A drag, the play button, and typing a new number all take effect
without anything compiling. A drag writes the number into the text as it goes,
so what you read is what runs, and an undo step is one drag.

Two exceptions, neither a choice: a **`const`** must be a compile-time constant
in GLSL, so it stays a number in the shader with no knob; and a number *inside an
expression* -- the `3.0` in `sin(3.0 * x)` -- has no window to be moved from.

A compile, when one does happen, shows as the busy cursor.

### Performance matters

This is an infinite canvas people will pan and pinch with a finger. It has to
hold display rate while doing it, and a stutter is felt immediately. The frame
budget is a real constraint, not something to tidy up later.

- **Cost is proportional to what is on screen**, not to what is on the canvas.
- **Work whose inputs have not changed is not redone.**
- **Measure before optimising.** A guess about where the time goes has been
  wrong before.
- **A window's size in world units does not depend on the zoom.**
- **Nothing waits for the GPU** in the middle of a gesture.

### Deleting leaves nothing behind

Things are made and thrown away constantly here -- boxes, number lines, plots,
pictures, whole canvases -- so **a deleted thing stops costing anything**. Not
almost nothing: nothing. Anything that grows with use has a bound; a child
process stops when its window goes; nothing a box does writes a file.

### Every edge is smooth

Every line, card, marker, curve and silhouette on the canvas has a smooth edge,
at any zoom. A staircase edge is a bug.

### Every window is a window

Everything on the canvas -- a code box, a paragraph, a number line, a plot, a
picture, a space window, a photograph, a video -- is a **window**, and they all
behave alike: carried by the rim round their edge, landing on the grid, caught by
the lasso, resized by the grip in their corner, thrown away on the same drop target. A new kind
of window gets all of that, not a variant of it.

### Showing that two things are the same object

A variable is written in the equation, again in the code, and again on whatever
window (or windows!) stands for its value, and nothing about where those sit on screen says
they are the same thing. **Making that connection visible is a key part of the
program**, not decoration, and every new kind of window has to answer for it.
(Section 6.)

### The code is where things come from

A window is not asked for from a menu: it is **pulled off the name/type** of the thing
it is about, and lands where you let go. And the reverse: a value can be put
*into* the code from a window. Neither the text nor the windows is where things
are made -- they are two views of one program, and you can work in either.

### A viz is the user's

A **display window** (dw) -- a number line, a plot, a space window -- is the
app's: scales, cameras, panning, merging. How a value *looks* in one is a
**viz**, and a viz is the user's: it is GLSL written on a canvas. **The app's own
vizes are written the same way**, in sketches that ship with it. Drawing a type is
a viz; if a viz cannot yet say what a new window needs, what a viz can say is
extended -- it is never drawn in C++ instead. (Section 9 says why this is written
down so firmly.)

### One name for a thing

**A renamed spelling is renamed, not aliased.** There is one name for a thing at a
time, and the sketches are rewritten when it changes. Two spellings for one thing
is two things to read, and one of them is a lie about what the program calls it.
(Tried and undone.)

### Chrome comes and goes; what it chose stays

A control appears when you reach for it and goes when you leave; the state it
set does not. Chrome is a fixed number of screen pixels at any zoom, so it is
always as easy to hit. **A toggle says what pressing it would give you**, not
what it is now.

### Expect churn

Things will be changed, changed back and rewritten from scratch. Designs that are
cheap to throw away are better than designs that are general.

---

## 3. Reading code as mathematics

**A box is GLSL.** It used to be C++ first and GLSL if C++ refused it; that is
gone, and nothing in the app launches a compiler. **A box need not be a whole
program**: fragments compile, with the app supplying the scaffolding round them --
the uniforms, the interface, a `main` round the lines.

**Only some code has a mathematical reading.** A line that has one can read as LaTeX;
one that cannot stays code. A box can be part equation, part code.

**The flip is per line.** Each line is independently LaTeX, hovered code, or code
being edited.

**Editing a box means editing all of it.** Giving a box the caret turns the whole
of it into code and hides the injected values: while you write, you see your own
text.

**The toggle settles a box rather than flipping it.** A box starts in *auto* --
mathematics while read, code while written. Pressing the toggle answers for good:
the box reads that way with or without the caret, tomorrow too. (A toggle that
springs back the moment you click away has not done anything.) Every code box has
the toggle; only a `\text` box does not. What differs between kinds of box is
which side it starts on -- a viz box starts as code, since there is no equation
reading of a mesh.

**"Always show code" overrides every box.** A tickbox in the Escape menu makes
every code box on the canvas read as code, whatever its own toggle says, and the
toggle is not offered while it is ticked. What shows is **only your own text**:
the values the app chose are not shown even on hover, and live in the knobs, so a
box is only as wide as what you wrote. Prose is words either way. It is a choice
about how a canvas is read -- one meant to be shown as code, say -- so it is
**saved with the canvas**, and ticking it is an undo step like any other change.

**Indentation survives the flip.** A row's indent is measured off its own code,
so a line sits at the same place in either reading and does not jump sideways.
(Prose is exempt: there a leading space is a space between words.)

**A box is sized for the worst case of both readings**, in width and in height,
so flipping a line, focusing the box or dragging a knob never changes its shape.

The readings themselves:

- **A function reads as an arrow, and what it returns reads as an equality.** The
  head is `swirl : ℝ² → ℝ³`; the `return` is `swirl(p) := …`. These are the two
  lines that say what a function *is*.
- **The braces are kept, set as mathematics**: `\{` closes the signature and `\}`
  stands on its own line, so a definition has a visible extent. (A single tall
  brace down the side would be better; it wants layout across rows, which a box
  does not have.) The brace closing a loop inside a function stays code.
- **A function with two `return`s** reads as two definitions, each true under a
  condition nothing shows. Left as it is: the honest answer is a piecewise brace,
  which wants the same layout as above.
- **A part of a value is a subscript**: `p.x` is `p_x`. **A constructor is not a
  function**: `vec3(a, b, c)` is `(a, b, c)` and `Complex(a, b)` is `a + bi`. A
  dot followed by a digit is a decimal point; otherwise it reaches into a value.
- **An absolute value is bars**: `abs(x)` is `|x|`, the single bars of `length`'s
  double ones -- one idea, told apart by the doubling.
- **Numbers read as numbers**: `2.`, `2.0` and `2` are all `2`. `1.5` keeps its
  digits.
- **`:=` for a rule, `=` for a fixed value.** A right-hand side that mentions
  anything is a rule (`c := (a+b)/2`); a bare number is chosen (`a = 0.4`). The
  difference between a value somebody *chose* and one that *follows* is worth
  having on screen. **A call is a mention**, even with empty brackets --
  `r := randChanging()` -- but a constructor is not.
- **A name that is a word gets a thin space beside it**, so `speed * time` does
  not read as one name. Single letters stay tight.
- **The algebra is considered, not named**: `Cl(3,0,1);` reads "Consider
  Cl_{3,0,1}", and every type names the algebra outright (Section 8).

---

## 4. Writing in a box

- **Typing with nothing selected starts a box** wherever you are looking -- in the
  nearest clear space to the middle of the view (see *Placement*).
- **Tab is about lines, and so is Ctrl+/.** A step of indentation is two spaces.
  With a highlight, Tab and Shift+Tab move every line it touches in and out, and Ctrl+/ comments them all (or
  uncomments them if they all are). Both keep the highlight, since they are edits
  *about* it. Comments go in at the shallowest indentation, and a half-commented
  stretch becomes wholly commented.
- **Ctrl+A selects the box**; **Alt+Delete takes the whole line** (or every line a
  highlight touches) -- and taking the first line of a text box keeps its sigil.
- **Ctrl+Backspace and Ctrl+Delete take a word**, by the same reckoning of words as
  Ctrl+left/right.
- **Ctrl+D is VS Code's.** With nothing highlighted it highlights the name the
  caret is on or beside; with a highlight, it adds the next place that text is
  written, after the newest and round to the top, with a caret of its own, and
  then typing, deleting, moving, cutting and pasting happen at every caret.
  **A whole name finds only that name**; a highlight that is part of a name finds
  those letters inside other names too -- which is decided by the first
  highlight. **Ctrl+Shift+D skips**: the newest highlight is let go and the next
  one taken. Escape goes back to the first caret. Copying from several carets
  gives a line each, and pasting as many lines as there are carets gives one
  each. **Alt+D counts** (VS Code's *Increment Selection*): the first highlight,
  top to bottom, is a whole number or a letter, and each one after it is the
  next -- `4,4,4,4` becomes `4,5,6,7`, `aaa` becomes `abc`, z wraps to a; carets
  with nothing highlighted get `0, 1, 2...`. Whole-box gestures (Ctrl+A, Ctrl+/,
  Tab over a highlight, Alt+Delete) and a click go back to one caret.
- **Pasted text keeps its own indentation**, rather than having this line's
  indentation added to every line of it.
- **Double-clicking a name highlights the whole of it**, as Ctrl+D does -- and so
  takes hold of it, ready to be dragged out.
- **Dragging across text highlights it**, and so does Shift with any caret
  movement. Selecting part of a name is how you take hold of what it refers to.
- **The caret decides what keys mean.** With a caret in a box, Ctrl+C/X/V are the
  text and Ctrl+Z is the box's own undo; without one, they are the windows and the
  canvas.
- **The pointer and the keyboard are two different claims.** A box with the caret
  keeps the keyboard wherever the mouse is. Only a video's trim time being typed
  into takes the keys away, since that is a caret too.
- **A mistake shows on its line**: that line reveals its code, a red caret points
  at the spot, and a balloon gives the first *two* errors, since one mistake
  usually has another right behind it. **Clicking the balloon puts it away**, red caret and
  all, until the next compile or the next time the caret leaves the box --
  either shows it again, even if the error is the same. **Its corner has a copy button** (two pages; a tick once pressed),
  since an error is often wanted somewhere else -- a search, a question.

---

## 5. Values and knobs

**Free variables get knobs** -- a handle on a number line for a real. That is
"choosing values" made physical.

### A number line per box

**A box gets one number line, and all of its reals share it**: values in a box
are read together, and a stack of one-value axes says nothing about how they
compare. The axis frames itself once, when everything on it has a value. Free
and derived values look the same; a derived one cannot be dragged (move one of
its inputs instead).

A number line belongs to the canvas, not to a box: lines can be pulled off,
dropped onto each other to **merge** -- a way of *seeing* values together, not a
claim they are equal -- and an axis can carry values from different boxes. Each
zooms and pans on its own; the left button is spoken for along it, so **it
scrolls on the right button**, the one window on the canvas that does.

### Making and pulling out

- **Pull a variable out of the code**: select any part of any mention and drag it
  off the box; it comes away as an axis of its own and lands where you let go.
- **Pull a viz out of its window**: Ctrl-drag a marker off its number line, a
  curve off its plot, a point out of its space window (or hold the window with one
  finger and drag with another). Once it leaves, it is an ordinary carried window.
- **Put a value down on an axis**: press the bare axis and `float d;` is written
  into the box, at the line the caret was left on (the box blinks a caret there
  while you aim). The press carries straight on into dragging it -- making one and
  moving one are the same gesture -- which is why it is declared free. It joins
  the axis it was put on, at that scale.
- **Make a third from two** (touch): a finger on each of two values freezes them;
  bring the fingers together and a menu of `+` and `*` appears; slide onto one and
  lift, and `float c = a * b;` is written. Both must be in the same box. "Together"
  means the distance halved, since fingertips can never meet.

### What a window shows: *when*

A variable is mutable, so a window has to answer *when*. **It shows the value at
the line the box is being read at**: the caret's line while the box is selected,
otherwise the end. (Not the pointer's line: that made windows flicker as the
mouse crossed a card.) A hovered declaration shows the number it is *given*.

**With a caret in a box, only the variables being written about keep their
windows** -- those on the caret's line, or those a highlight touches. The rest
stand down but keep their place, so nothing shuffles. A number answering a
different question from the one you are asking is worse than no number. A
picture stays (it stands for the box), and a shared axis only stands down the
edited box's markers. A variable not yet declared at the reading line is not
shown at all.

**A variable is not a knob; the numbers given to it are.** A variable assigned
numbers on lines 2, 9 and 13 has three knobs, and dragging moves the one that
governs what you are looking at -- the last one at or before the reading line.

**A free real inside a function gets a knob too**: `float k;` is the same abstract
statement wherever it is written. **A derived one does not**, by decision: inside a
function it has a different value for every input, so there is no one number for
a marker to show. What a function does with its input is shown by feeding it.

### Closing a control bakes it

**A window that controls something bakes what it controlled; a window that only
shows something does not.** Throwing away a free variable's number line writes
its value into the code: `float a;` becomes `float a = 0.4;` -- and nothing
compiles, since a typed number is a knob too. Taking the number back out brings
the knob back. Closing a plot, a picture or a derived marker takes away the view
and nothing else. Closing is remembered with the canvas.

### Things that move by themselves

- **`time`** is the seconds since the box last compiled -- editing a box is
  starting it again.
- **`rand()`** is a new number in [0, 1) on every call, seeded by the pixel, so a
  picture made of it holds still. **`randChanging()`** is new every frame as well.
  (`r = rand()` while a box reports its values is the same number for every value,
  so reports agree.)
- A box that says `time` or `randChanging` is re-read every frame; any other box
  costs nothing while its inputs are still.
- **The play button** under a knob swings it as `centre + amplitude·sin(t)`,
  sweeping half its axis -- a swing rather than a ramp, since a ramp runs off the
  end. While playing, the button stands under the middle of the swing; pressing
  again stops it there; grabbing the marker takes over. The text is rewritten as
  it goes. Everything playing pauses while any box has the caret. Playing is not
  saved, and one undo after stopping goes back to before it began.

---

## 6. Showing that two things are the same object

**Colour is identity.** Every variable has its own colour from **viridis**, by its
place on the canvas -- the boxes in order, then each box's names in order -- and
everything standing for it wears that colour. Viridis because two colours equally
far apart on it look equally different; the walk along it is by the golden ratio,
so neighbours land far apart and a name added at the end recolours nothing. (A
name added earlier shifts the ones after it: the price of one list for the
canvas, which is also what gives a name one colour everywhere it is used.) The
dark end is skipped, since cards are dark.

**Nothing says whether a value is free or derived.** Shape used to (a ring you
could drag, a dot you could not); it was taken out as more than it was worth,
since every viz would have had to draw both. Editability may want showing some
other way, or may not need it.

**A blink says "this one, now"**: a viz in use flashes white, half a second on and
half off -- a hard switch, since a throb reads as decoration and a blink as a
signal.

**What lights up is the one thing under the pointer** -- the nearest marker, not
the whole row, and nothing over the bare axis, where a press makes a new value.
A derived marker lights up too: pointing says which thing it is even when a press
could not move it. What a press would take hold of is asked separately. Carrying a
window lights everything on it; a plot, having nothing finer, lights every curve.

**It works from the other end**: point at a mention of a variable, or highlight
one, and the same set lights up -- the name wherever else it is written, the
wires, the viz blinking. Hovering a window and hovering its name cannot disagree.
That holds for every kind of *value* -- a real, a colour, a point. A **function's**
name is exempt for now: hovering it need not reach its picture, surface or volume
(a curve on a plot happens to answer anyway).

**Using a window boxes the name**, in the variable's colour, everywhere it is
written, in whichever reading that line is showing.

**And a wire joins them**, a curve in the same colour drawn over every window. It
leaves the *viz*, not the pointer -- the marker is the variable, the pointer is
only where your hand is -- and lands on whichever side of the name the other end
is on.

- **A wire's side is chosen once, when the value is picked up**, and kept until it
  is let go. A choice re-taken every frame can change its mind mid-gesture.
- **A name written five times is still one variable, so the wires converge** at a
  junction just clear of the names and run on as one cable.
- **A picture's wire leaves the pointer**, because there the pointer *is* the
  value: **the pointer over a picture is the argument of the function it draws**
  (the `p` of `vec3 ripple(vec2 p)`). It lights up where that argument is written,
  in that function only, and takes the next colour after the box's variables.

**A derived value is the end of a little machine, and pointing at it draws the
machine.** `d = a + b * c` appears as wires from the windows of `b` and `c` into a
bullet marked `*`, from that and `a` into a bullet marked `+`, and from that, in
`d`'s colour, to everywhere `d` is written and shown.

- The wires leave the vizes, not the mentions -- it is a diagram of what the code
  computes, not of the text.
- A wire out of an operator is black: working belongs to no variable until it
  arrives at one.
- An operator is written as the code writes it (`*`, `+`, `|`); a call shows its
  own name (`sin`, `grade0`).
- A bullet has a front: flat where its inputs arrive, pointed where its answer
  leaves, towards what it feeds. One wire leaves the point before forking, since
  it made one thing.
- The deepest operation is lowest and the last is nearest the box, since the
  working runs towards what it makes. It hangs below the box and its windows,
  where there is room.
- A literal is not a node: a number has nowhere to come from.
- Whatever feeds a highlighted value is in scope, so its inputs keep their
  windows.

**A sample is half of a pair.** Pointing at a fed function's sample, or at its
answer, boxes both and wires them together (Section 7).

**Nothing is hovered while something is held.** A knob being dragged, a window
carried or turned, the canvas panned, a lasso drawn, a finger down, the Escape
menu up: the pointer is busy, and what it passes over is not being pointed at.
And **a drag in flight owns the pointer**, so letting go over another window still
lets go -- and a lasso can sweep across windows without stopping.

---

## 7. Functions

### A function is a function

**A function is recognised by its shape**: `Tri p(float t) { … }` is a function
exactly as `float f(float x)` is, and runs from its signature to the brace that
closes it. There is no type name for "a function"; the signature *is* the type.
(`RealFunction f;`, then `RToR f;`, were tried: they could only name one arrow,
and said nothing a signature does not.) A function takes one argument for now;
**a GLSL function of any other shape** -- two arguments, none, a `void` -- is
compiled, can be called, and is shared like any other, but is not a function in
this sense: nothing draws it.

**Functions compose by calling**: `h = f + 2g` is
`float h(float x) { return f(x) + 2.0 * g(x); }`, which is honest about what runs
and costs nothing. A function's constants are knobs, so dragging one reshapes it
live.

### Functions nobody wrote a body for

**A function you say things about without saying which function it is** -- this is
one of the things the app is *for*. Much of the mathematics it is aimed at, PDEs
above all, is about functions in general: `u` satisfies this, its derivative is
that. So `u` is declared and never defined, written as GLSL writes a function
with no body:

```
float k;                                     k ∈ ℝ
float u(vec3 q);                             u : ℝ³ → ℝ
float v(vec3 q) { return u(q) * k; }         v(q) := u(q) k
```

**The body is injected, exactly as a value is**: one of four recognisable shapes
-- a wave, a parabola, a bump, a line -- of whatever single number the argument
offers (itself, its distance out, its size), one per component of the answer.
Hover the prototype to see it. Everything downstream treats it as any function.

**Unless the box gives it a body**: a prototype followed by a definition of the
same function is a forward declaration, as GLSL calls it, and nothing is
injected. A box can begin `float u(vec3 q);`, say things about `u`, and later say
what `u` is.

A function of a multivector gets a constant body: a gap, not a choice.

### What goes in decides the window; what comes out decides the colour

**A box draws because it defines a function of two variables** -- it computes a
map from the plane, and a picture is what one looks like. (It used to draw
because its text mentioned `finalColor`: a fact about the file, not the
mathematics.)

The input decides the *kind* of window, and they are different kinds because **a
display window is largely its camera**:

| function | window | camera |
| --- | --- | --- |
| ℝ → ℝ | a curve on a plot, and a field on a number line | none |
| ℝ² or ℂ → a colour or ℂ | a picture | none: a colour has nowhere to go but its pixel |
| ℝ² → ℝ | a surface | orthographic, and it turns |
| ℝ³ → ℝ or ℂ | a volume | perspective, and it turns |

The projections are not a preference. **A surface is a graph**, and a graph must
draw the same quantity the same size wherever it stands. **A volume is a place you
look into**, and there nearer things being bigger is what says they are near. So
the wheel changes the patch shown on a surface, and moves the eye in a volume.

The output decides the colour. **A real is viridis, signed** -- zero nothing, +1
green, -1 yellow -- the same map in every window, so green means the same number
wherever it is read. A `vec3` or `Color` is a colour. **A complex number is domain
colouring**: hue for its argument, lightness for its size, black at a zero.

**`Complex` is its own type, not `vec2`**: the same two numbers, but one is a
place and the other a place with a multiplication, and a function of the second
is worth drawing differently. `z * w` is complex multiplication and `~z` the
conjugate.

- **A plot** has input running *up* and output across -- the transpose of the
  usual convention, because these axes are heading towards spacetime diagrams,
  where up is time. Both axes share one scale, so a slope of one is a diagonal.
  Plots merge like number lines, which is how a vector space becomes visible: f,
  g and f + 2g on one set of axes. A plot does not write its curves' names; colour
  and pointing say which is which.
- **A field** on a number line is a wash of colour, clamped rather than rescaled
  beyond ±1, so two fields can be compared. Each function starts on a row of its
  own; dropping one row on another puts them on one scale.
- **A surface opens flat** -- exactly the picture it would have been -- and the
  right button turns it into relief: what looked like a texture turns out to be a
  map seen from above. The eye never goes below the plane.
- **A volume starts turned**, since a volume seen square is a wash with no depth.
  Its cube is drawn, so you can tell how big it is and which way it faces.
- **Dragging right turns the right-hand side towards you**, in every window that
  turns, like a hand on a globe.

A curve has no sampling window: it is drawn by calling the function at every
pixel, so zooming a plot out never runs out of curve.

**A box's body is not a field.** What varies across a picture is the function's
argument, not the box's variables: the body runs the same everywhere, and every
one of its values is a number. **A drawing reads each variable as it is given
where it is declared** -- the body is not run per pixel, on purpose -- while
**feeding** a function runs after the body, so a fed function may read anything
the box computed.

A box gets one window of each kind, not one per function; each shows whichever
function of its kind the box is being read inside. A box with no variables and no
functions is never built, which keeps notes from being compiled.

### A function is drawn by being fed

A value has a place to be; a function does not. It is a machine, and the only
way to see a machine is to put something through it. **A spread of samples of
the input is drawn in whatever window the input type goes in, each is put
through the function, and the answers are drawn in whatever window the output
type goes in.** A function from a real to a point of Cl(3,0,1) is a row of beads
on a number line and a constellation of points in a space window, index for
index.

- **A sample is drawn exactly as a value of its type is.** (Drawing them smaller
  was tried and taken out.)
- Pointing at either end boxes both and wires them -- they are two halves of one
  statement. Nothing can be grabbed: a sample is a place the function was asked.
- **The function shown is the one the box is being read inside** -- the caret
  anywhere from its signature to its closing brace. Read elsewhere, the spread
  goes.
- The spread gets a row of its own, framed around it, since where a function is
  asked is a different question from the box's values.

**Where the spread comes from**: `Samples(float, 24);` on a line of its own makes
a box a **sampler**, saying how a type's space is laid out and how thickly. It
reads `S₂₄ ⊂ ℝ`. Its body lays samples down with `pick(…)`; `sbRand(i)` is a hash,
so the spread is the same every frame. A type with no sampler gets the spread the
app was born with -- 24 reals, evenly across [-4, 4] -- and only the reals have
one, since what a spread of points should be is the drawer's question.

Today the beads are the fallback for any function without a better drawing. A
function to a point will want to be a curve through space, a function to a
bivector lines rather than points -- and each of those will be a viz.

---

## 8. Types

### Where a type lives

`src/types/` is where a supported type lives, and it is meant to become the place
a **user** drops an extension. Today its contents are compiled in; eventually it
may be files loaded at runtime. Either way the directory is the unit. There is no
`Real.*`, because `float` is already GLSL: only some types are new vocabulary,
but all of them need showing. **Choosing a value and showing it are one job**, so
a type's example generator sits with how it is shown -- which is its viz, `example`
below. What is left in `src/types/` chooses only for a type no viz box chooses for
(`Even`, say), and as a box is first opened, before a viz has been built.

### Clifford algebras

The type the tool is most wanted for -- this app's answer to GAmphetamine.js, and
it works the same way: the algebra is **generated**, concrete structs and concrete
products, fully unrolled.

**A type is a set of grades.** That one restriction is the whole trick: the
product of two grade-sets is a grade-set, so every product has an *exact* result
type, and every result type has a name. Nothing is ever widened to `Multivector`
to make things line up.

**Names are the grade count and nothing else**, ascending: `Una`, `Bi`, `Tri`,
`Even`, `Odd`, `Multivector`, and compounds like `ScalarQuad` (where
`ScalarQuadvector` would be a sentence). A compound needs no declaring: write it
and it is a type.

**The signature is written the way it is spoken**: `Cl(3,0,1);` is p generators
squaring to +1, q to -1, r to 0. It is a statement about the canvas, not a line of
the program. A canvas that says nothing works in Cl(3,0,1), projective geometric
algebra. **A canvas works in one algebra** (Section 10).

**`e0` is the null generator, and only an algebra with one has an e0.**
Generators count from one, as axes do, so Cl(2,2) is e1..e4; a degenerate algebra
puts its null generator first as e0, as every PGA text does. `Cl(2,2);` means
`Cl(2,2,0);`.

**Degeneracy is not a special case**: the dual is a *complement*, defined by
`b ^ dual(b) = I`, which is what lets it survive a pseudoscalar squaring to zero.

The operators are GAmphetamine's, and each reads as mathematics:

| code | means | reads |
| --- | --- | --- |
| `a * b` | geometric product | `ab` |
| `a \| b` | inner product | `a · b` |
| `a ^ b` | outer product | `a ∧ b` |
| `a & b` | regressive product | `a ∨ b` |
| `~a` | reverse | `ã` |
| `a >> b` | sandwich | `a b ã` |
| `dual(a)` | dual | `a*` |

Every operator also has a function spelling -- `gp`, `ip`, `op`, `rp`, `rev`,
`add`, `sub`, `div`, `sw` -- which reads exactly the same.

- **The operators always mean the algebra**: `~ ^ & |` are never bitwise (deciding
  otherwise would need a type checker). Boolean work uses `&&` and `||`, and `!`
  is boolean not. (It was the dual once; that cost `!` its ordinary meaning for
  something rarely wanted.)
- **The precedence is the mathematician's**: every algebra operator binds tighter
  than `+` and `-`, so `a + b | c` is `a + (b | c)`. Within the algebra, `*` and
  `/`, then `^` and `&`, then `|`, then `>>`. Both the LaTeX and the shader use
  this order.
- **`a >> b` is a versor acting on something**, written out as `a b ã` because
  mathematics has no symbol for it. Its sign is superconjugation, and it gives
  back b's type: a versor action preserves grade, and the parts the type system
  cannot see cancelling are dropped.
- **A narrower value fits where a wider one is asked for** (`Even p = 1.0 + b;`),
  in declarations, assignments and returns. **Only upwards**: dropping grades
  silently is an error, and `grade2(…)` is how you say you meant it.
- **Grade selection is `grade2(M)`**, read as Hestenes writes it, `⟨M⟩₂` -- with
  the subscript always shown, even on the scalar part, so the easiest projection
  to miss does not look like no projection. Only grades a type has get a
  selector: asking an `Even` for grade 3 is a mistake, and the types can see it.
- **The reverse of a sum wears one long tilde**, drawn at the width of what it
  covers -- a small tilde in the middle of a sum would say it was the reverse of
  one term.

How the algebra reads: **it is considered, not named** -- `Cl(3,0,1);` reads
"Consider Cl_{3,0,1}", and every type names the algebra outright:

| type | reads |
| --- | --- |
| `Una`, `Bi`, `Tri` | `⋀ᵏ Cl_{3,0,1}` |
| `Even`, `Odd` | `Cl_{3,0,1}⁺`, `Cl_{3,0,1}⁻` |
| `Multivector` | `Cl_{3,0,1}` |
| compounds | the direct sum of their parts |

The signature goes *under* because the grading (+ and -) goes over. The exterior
power is written of the algebra rather than of the space underneath it -- a
standard abuse, justified because the two are isomorphic as graded vector spaces.

Conventions, stated so they can be argued with: the inner product is the
symmetric one, keeping grade |r - s|; the dual is the Poincaré complement; `a & b`
is `undual(dual(a) ^ dual(b))`, so `^` meets planes and `&` joins points, as in
ganja.

**A multivector has a knob** -- its components -- so choosing one and dragging its
point compiles nothing. A multivector *typed* into its declaration has no knob
yet.

### Colours

**`Color` is a vec3 that means a colour.** Everything GLSL does to a vec3 it does
to one -- `.rgb`, `mix`, `clamp` -- and `hsv(h, s, v)` makes one, with `hsvOf(c)`
going back. **A new struct when the algebra differs; an alias when only the
meaning does**: `Complex` needed a struct because its product is not
component-by-component, but a colour's arithmetic is a vec3's, and what differs is
only what it means -- which is what the app reads off the declared type. (This is
not the aliasing that "one name for a thing" forbids: a colour and a vector are two
things stored the same way.) The price is that the driver's messages say vec3.

**Its knob is its three numbers.** There is no reading as mathematics for a
colour yet, by decision.

**Its window has three views**, chosen from a dropdown beside it (where later
choices about the window will go too):

- **Picker** -- a hue ring, and inside it a triangle whose corners are black,
  white and the hue it points at, as drawing programs have long done. Every colour
  sits on the ring at its hue; the one the triangle points at -- the last taken
  hold of -- is also in the triangle. Drag round the ring for hue, inside the
  triangle for saturation and value.
- **RGB cube** and **HSV cylinder** -- turned on the right button, orthographic
  (a colour space is a graph of three numbers), with edges and walls coloured by
  what they stand for. A drag moves a colour in the plane facing you.

**Colour is identity, and here colour is also the value**: a marker is filled with
the colour it *is*, ringed in the colour of *which* variable it is. A colour
outside 0..1 is drawn where it is, outside the solid.

---

## 9. Display windows and vizes

**A dw** (display window) shows values on a scale of its own and knows about
scales, cameras, panning, merging and docking -- and nothing about what a value
looks like. **A viz** is how one type is drawn in one kind of dw, and a viz is the
user's.

### Vizes are bootstrapped

**The app's own vizes are written the way a user's would be: in GLSL, on a
canvas.** There are many kinds -- a ball, an arrow, a line through space, a plane,
a conic swept into a tube, a field -- and across all of them, a viz:

- **is drawn** in a chosen dw, depending on the value it stands for;
- **has a position** there, which is where its wires attach, also depending on the
  value.

Both come from GLSL functions in **one window** -- a viz is one box. **One window
per type and dw**: `Viz(Tri, "Space");`, since how a thing is drawn is a question
about where it is drawn. The window's name is in quotes: a type is something the
program has, and a window is only something the canvas calls one of its own.
Unquoted, it is an error that says so. A box with a second `Viz(...)` line is an
error on that line -- usually one left over from copying -- rather than a quiet
change of what the box draws.

**A dw has no opinion about which types go in it.** A value goes in a dw when its
type has a viz there, and for no other reason: `Viz(Bi, "Space")` puts a box's
bivectors in its space window beside its points, drawn by that viz. A dw that
chose its own types in C++ would be the drift below again, one step removed.

**Pointing at a dw shows its name** above it, exactly as a viz box spells it, so
the name to write is never a guess. A viz's parts have standard names -- `mesh` (its
geometry, made once, not per value), `vert` (where it goes, given the value),
`frag` (optional; without it a viz is lit), `position`, `grab` (how near the
pointer is) and `drag` (what the pointer is asking the value to be). A viz of
several pieces numbers them, up to four. A viz without `grab` cannot be pointed
at; without `drag`, it cannot be dragged.

**`example` chooses a new value**: `Tri example() { ... }` is what `Tri p;` is
given when nothing else says what it is. Inside it `rand()` starts from the
variable's name, so `p` gets the same example every time, and `sbLimitsMin` /
`sbLimitsMax` are the window's walls. A `Tri` in space is a point of weight 1
somewhere inside them, where it can be seen and grabbed; a real is a tidy number
in [-1, 1); a colour is a bright one of any hue. **A type has one example**,
written in any one of its viz boxes: two is an error in both. The canvas's own
wins over the built-in. Asking is a moment on the GPU, so it happens only when a
variable is new -- nothing kept from before the edit, nothing saved.

**A piece's mesh can be a model** made in a modelling program: `void mesh2() {
model("cupped_hands_pair.obj"); }`, the file beside the vizes sketch or beside the
canvas. It is scaled to fit the unit sphere, like a mesh a viz lays down itself, so
`vert` sizes it the same way. A piece a value does not want is folded away to
nothing by its `vert`, which is how one viz shows different things for different
values.

**A function's viz is its fragments**: `frag`, and optionally `grab`, with
`value(…)` meaning the function being drawn. It is named by what goes in and out
-- `Viz(RToR, "Plot")`, `Viz(R2ToR, "Surface")`. It runs inside the box whose
function it draws, on whatever canvas that is, so **what it takes from its sketch
travels with it**, renamed as its own helpers are so nothing can clash with the
canvas it lands on; and its knobs, and its sketch's, are read there every frame,
so dragging one restyles every curve drawn with it and compiles nothing. **Its
`example` chooses a body for a function nobody wrote**: `float example(float x)`
is what `float u(float x);` runs, with `rand()` started from `u`'s name -- the
built-ins pick one of four shapes a person would know (a wave, a parabola, a bump,
a line) of how far out the input is, for each part of the answer. The hovered
line shows `{ return example(x); }`. A box that takes `u` from another box takes
that viz along, since the body is its.

**The built-in vizes are sketches in the repo**, in `vizes/`: one per algebra
(`Cl(3,0,1) vizes.sketch`, `Cl(2,1,1) vizes.sketch`), since a `Tri` means
something different in each, and `vizes.sketch` for types of no algebra. Every
canvas has them without asking, and editing one takes effect everywhere. Each has
values of its own to look at while it is edited. **A canvas's own viz wins**; one
that does not compile gives way to the built-in.

**A viz sees what its sketch shares**, as every box does: the functions and free
values of the ordinary boxes beside it. A helper two vizes need -- where a `Tri`
is, a sphere's maths -- is written once, in an ordinary box, where it can also be
tried out with knobs and plots; and a knob there restyles every viz that reads it,
live, as any knob moves what reads it. A built-in viz takes these from its own
vizes sketch, compiled in: nothing of that sketch reaches a canvas using it, so
its names cannot clash with the canvas's, and its knobs are as the sketch saved
them. A viz box's own functions stay its own -- they can use what only a viz has
(`emit`, `sbPointer`), which no other box could call.

**A viz box is a box.** Its free values get knobs and windows, wherever they are
written -- `float cylinderRadius = 0.12;` inside `mesh` is a knob, as it would be
inside any function -- and its functions of one argument are drawn, as anywhere
else. Dragging one of its knobs restyles the viz live, compiling nothing. Its
standard functions (`mesh`, `vert`, `position`, `grab`, `drag`...) are not drawn:
they answer the window's questions about the pointer and the camera, and have no
meaning outside it. **A viz box can show itself**: `Tri self;` in the `Tri` viz
is a point in a space window, drawn by the viz it sits in -- which is part of the
point. (A built-in viz's own windows are never built: nobody is looking at them.)

Bulky machinery lives in a few app helpers that a viz *calls* (a ray march, say),
so the viz's own code reads as what it does: short, but there.

**A dw's furniture is the app's**: scales, ticks, walls, sky, camera, the light,
the picker's ring and triangle. That is what a dw is.

### How the vizes drifted -- and the rule that came of it

Viz boxes arrived with a fallback: *a type with no viz keeps the drawing the app
was born with*. The fallback became the main road. Every window added after it was
drawn in C++, because the viz box of the day could not say any of it (it was 2D
and flat), and nobody extended it instead -- and nobody said that was a choice.

The rule: **drawing a type is a viz, and a viz is GLSL in a vizes sketch.** A
drawing a viz cannot yet express is a reason to extend what a viz can say, or to
stop and ask; never a reason to draw it in C++. Today every value and every
function on the canvas is drawn by a viz.

### What a viz still cannot say

- **A path**: only triangles. A line of constant screen width will want its own
  primitive.
- **A helper written in one viz box, for another**: shared helpers go in an
  ordinary box.
- **Several dws from one window**: a window names one.

### Space and spacetime

**A space window is a place to look into**, with a camera of its own. What is in
it is whatever has a viz there -- built in, that is a point, which is what a
trivector of a 4D projective algebra is. **In Cl(3,0,1) a point is a pair of
cupped hands**, mirrored when its weight (`e123`) is negative, since a point and
its negative are the same place with opposite orientation; **a point at infinity
is a ball** out at the edge of everything. **Cl(3,0,1) is space and Cl(2,1,1) is
spacetime**, where the third axis is time; the two windows differ only in the
algebra and the sky. A box gets one, and all its values share it to begin with;
a value can be pulled out and dropped into another window of the same kind.

**A Tri's coefficients are homogeneous coordinates**, and which is which axis needs
no table: a grade-3 blade omits exactly one generator, and the one it omits is its
axis -- e023 is x, e013 is y, e012 is z, and e123 (omitting the null e0) is w.

**The cuboid is a window into the space, not a box containing it**: turned inside
out, so you always see into it, and what falls outside is cut away. A point at
infinity is exempt -- it is not *in* the space -- and each window can turn the cut
off. Right-drag (or two fingers) orbits; the wheel moves in and out. A point is
dragged in the plane facing you.

---

## 10. The canvas

### Placement

**There are two grid dots per line of type**, measured from the font, so the grid
is ruled paper and **a card's padding is one dot** -- the corner and the text are
both on the grid without having to choose between them. A row the mathematics
makes taller is rounded up to whole dots.

**The grid is only up while it is used**: while something is carried, resized or
the canvas zoomed. It fades in, lingers half a second, and fades out, so reading a
canvas means a bare one. There is no switch for it: a control for something that
governs itself can only be wrong.

**Every window snaps to the grid as drawn**, so placement gets finer as you zoom
in. **Boxes never overlap**: one put on another steps clear by the nearest edge,
and a box's docked windows count as part of it. (Pictures do overlap, since a
caption on a photograph is normal.)

**A new window goes in the nearest gap to the middle of the view.** A box started
by typing on the canvas, a conversation, a picture or video inserted from the menu:
each is centred on the middle of the screen if that is clear, and otherwise put
in the nearest place where it overlaps no other window and has a line's height of
room round it. Nothing already there moves. Then, if it is not in the middle, the
camera glides until it is -- so a new window always opens in front of you. A new
box claims room for its first few lines (about 24 characters by 3) rather than for
an empty card, since it grows as it is typed into. A file dropped on the window
still lands where it was dropped: there the pointer said where.

**Everything a box makes flanks its card** -- right, then left, then below each --
alternating so a box with two windows is balanced. Its number lines and plots,
which it is *read against*, sit under it, as wide as it is.

### The camera

- **The camera opens one notch in**, where a box is comfortable to read.
- **Home frames what is on screen**: every window whose middle is in view, centred
  with a little margin. Home because that is "show me all of it" in Blender, and
  because it is not a character (typing starts a box).
- **The right button pans, the wheel zooms**, one finger pans and two pinch. The
  left button belongs to the contents and never moves the canvas. While panning,
  the pointer is a closed hand.

### Carrying, selecting, closing

- **Every window is carried by its rim**, which appears on hover.
- **A cross appears at the foot of the window while something is carried.** Drop
  on it to throw the thing away; there is no close button anywhere.
- **A lasso selects** whatever's middle it goes round. It closes with a curve
  continuing the stroke, not a chord, so a hook or a spiral catches what it looks
  like it catches. Selection is shown by the rim -- there is no second visual
  language for "this one".
- **A left click anywhere puts the selection down**, except the press that picks
  it up and a gesture already under way.
- **A selection behaves as one window**: it moves together, by whole grid steps;
  it can be thrown away together; Delete throws it away.
- **Ctrl+C/X/V take boxes only** -- their text, size and knob values -- and paste
  at the pointer, selected. A number line without its box would be a window about
  nothing. Text goes through the system clipboard; windows stay our own.

### Resizing

**A window is resized by the grip in its bottom-right corner** -- three short
diagonals, shown while it is pointed at -- the conversation's grip, which every
window now has. **It keeps its shape**: dragging it scales the window uniformly,
the top-left corner held still, so a box's type gets bigger and a picture gets
bigger, and nothing is stretched. (A conversation is the exception, and the
model: its grip sets its width and height apart, its words reflowing.) **It snaps
to its neighbours' heights and widths** -- "as tall as that" and "as wide as that"
are one question, since the resize is uniform. Code boxes are offered as targets
but do not snap themselves: the same size as another program means nothing.

**The wheel zooms the canvas**, over boxes and pictures as over bare canvas.
Over a conversation it scrolls it, and inside a window with a camera or a scale
of its own -- a picture of a function, a surface, a volume, a space window -- it
is that window's.

**A display window resized comes loose from its box.** Docking is a row of slots
of one size beside or under the box, and a window of another size has no slot;
it stays exactly where it was on screen, and the windows docked after it close
up. (Docking wants rethinking; until then a loose window can be overlapped by
what closes up behind it.)

### Undo

**An undo step is the whole canvas**, as the text it would be saved as -- not a
list of operations, each needing its inverse kept right forever. The camera is
not part of it. A step is taken **once things have settled**, so a drag is one
step; **a change nobody made is not a step** (a build coming back, an axis framing
itself). **Restoring keeps every box whose text has not changed**, so undoing a
move does not rebuild every shader.

### Sharing between boxes

**Everything shareable is visible everywhere, automatically**: a function defined
in any box can be called from any other, viz boxes included, and a free value
read by any other --
the way a paper defines a thing once and then uses it. No imports, no names for
boxes, no special box of definitions.

**Functions and free values are shared; derived values are not.** A derived value
is made by running its box, and sharing it would make the canvas one program with
an order between boxes, which a canvas does not have. When a box mentions one, it
is told so.

- A knob still never recompiles: one knob moves every box reading it. **The cost
  is on edits**: every box calling an edited function is rebuilt.
- **A shared value is what it is given where it is declared.** An assignment
  further down its home box is part of that box's run of lines, which no other box
  runs.
- A name defined twice on the canvas is an error in both places, since with no
  order between boxes "the later one wins" means nothing.
- **A mistake in shared code is reported at home**; a box using it can only say the
  mistake is in something shared.
- Sameness is shown: a shared name wears its home colour everywhere, lighting it
  anywhere lights it at home, and pulling it out of a using box pulls out its home
  value.
- A box being written in is left alone until the caret leaves it, or a change
  elsewhere would compile half a thought.
- A box copied to another canvas can be left calling something that is not there.

**The algebra is the canvas's.** A canvas works in one Clifford algebra, so
nothing is ever written in two at once. The `Cl` line is written in a box but is
a statement about the whole canvas; two different ones is an error.

### Words

**`\text` at the start of a box makes it prose.** That sigil is the whole switch:
not a mode or a second kind of object, so deleting it makes the box code again.

- Prose is set as text, upright and spaced as words, and **wears no card**: a
  caption is ink on the canvas.
- **It is written in as words**: the caret goes between two letters of the typeset
  line. (The one place "editing shows you the code" is set aside, since here the
  code *is* the words.) A toggle shows the source, sigil and all; it stays where
  you leave it.
- **Bold, italic and justification belong to the box**, chosen from a strip above
  it -- no markup, nothing half-applied. Justification is one control cycling
  through three states, its icon being the state it is in.
- **A paragraph has a measure**, set by a bar on its right margin; it starts at
  twenty ems, about sixty characters, the width prose has been set at for five
  centuries. The box is as wide as its measure. A wrapped line is many rows, and
  the arrows move by row.

### Slides

**A slide is a region of the canvas with a permanent rim**, declared worth
looking at on its own. Clicking the rim glides the camera until the slide fills
the window: **presenting is navigating**. Nothing is exported, and a live plot on
a slide is the same live plot.

- **A slide's height is one screenful at zoom 1**, its width that times the deck's
  aspect (3:2 to begin with, the window's own shape, chosen from a folded-away
  part of the menu). Arriving covers the window, so the rim is just off screen.
- **A deck runs left to right**, in the order the slides sit. Left and right flick
  through it instantly once you are on one, and so does a two-finger swipe, as in
  a browser.
- **Slides come from the Escape menu**, added at the end of the deck, and the
  camera goes to look at the new one.
- **A slide owns nothing but its rim.** Its title and body are ordinary text
  boxes. A slide does not move: it is a place, not a container.
- **A slide's text goes where the slide says, and everything else moves** -- the
  one placement that does not step aside.
- **Delete slide** is a button in the slide's corner, spelled out because it is
  the one thing with no gesture. It takes the slide and everything whose middle is
  on it, and the deck closes the gap.

### Transitions

For presenting and for videos, **writing can turn into other writing**, the way
Manim does it: a line of code into its LaTeX, or one formula into another. Code
into code is not wanted.

- **It is a capability, not a state.** Nothing on the canvas is kept ready to
  animate; a transition is made when one is asked for, from the outlines of the
  two sides.
- **What is the same thing travels.** A name finds its mention on the other side,
  a symbol the same symbol (`*` the dot, `/` a fraction's rule), the k-th of each
  the k-th. On the way, its outline turns into the other's: the code's `a` becomes
  the italic one.
- **Everything else fades**: what goes is gone before the travelling settles, and
  what comes arrives once it is under way, so the two are never on top of each
  other.
- **Colour is not part of it.** A transition is outlines; colouring them is
  fading the colour of some polygons, which the caller does.
- For now it is reached only from the command line (`--animate_text_transition`,
  in `README.md`).

### Recording and editing video

*Decided, and built.* For videos with the app in them and
Hamish in a corner.
**A recording is what happened, not pixels**: the app keeps the session -- every
keystroke and click, beside the camera and microphone -- and the video is drawn
again from it afterwards. That is what lets an edit move what was never moved,
and render at a size nobody was looking at.

- **What is recorded is the canvas as it changes**, as the text it would be saved
  as, frame by frame, with the camera, the pointer and the caret beside it -- not
  the keys that changed it. Playing that back needs nothing to happen the same way
  twice; replaying keys would need every compile, reply and clock to.
- **Recording files and edit files are separate.** A recording is what happened,
  and is never changed; an edit says what to make of one. An edit is of one
  recording. Recordings run to tens of minutes, an hour and a half at most.
- **Recording is started and stopped from the Escape menu**, and goes into a
  folder of its own under `Videos\sketchbook`. The camera and the microphone are
  recorded with it: the first of each, or the ones named in the settings. **The
  camera starts first**: Windows takes a couple of seconds to open it, so the
  light is dim ("starting...") until it is delivering, and only then does the
  session begin -- nothing said straight after pressing Record is lost. **When
  they are not** -- none found, or the camera held by another program -- the red
  light says so, since a session found afterwards to have no face cannot be done
  again.
- **A transition is silent**: it is time the recording never had.
- **The editor is a mode of the app** -- the same drawing, so what is edited is
  what is rendered -- but feels like a program of its own, and is started only
  from the command line. **Its monitor is the video as it will be**: the frame,
  16:9, with the face in it, above the timeline.
- **The face is pinned** to the picture, not a window on the canvas, and is not
  shown while recording. It starts in the bottom-right corner, with rounded
  corners, and is keyframed like a window: moved, resized, taken off (F). Across
  a transition its picture holds still.
- **A timeline, with cuts and ripple deletes.** A ripple delete takes every track
  with it -- the canvas, the face, the sound. Where the recording jumps there is
  a transition by default, half a second of its own: a box whose text changed
  morphs (*Transitions*), boxes and the camera glide to where they are after it,
  numbers ease from one value to the other, and anything that came or went fades.
  Removing the transition is how a cut snaps. A cut that skips nothing has none.
- **Everything moves on a sigmoid**, speeding up and then slowing down.
- **Boxes, the camera and the face can be moved in the edit**, by keys. A key is
  at a moment *of the recording*, so a cut never moves one. **A key is a move made
  then**: what it sets is arrived at over the half second before it, from where
  things were, and held after -- not a drift all the way from the key before.
  The camera starts as the recording had it and can be given back to it (B),
  easing back over the half second after. Such a move is in the picture only:
  nothing on the canvas steps aside, because nothing on the canvas knows.
- **A box keeps its identity** for as long as the canvas has it -- a number in the
  file -- which is what lets an edit say which box it moved.
- **On-or-off settings are timelines of blocks.** The pointer is hidden by default.
  Ticking it in the edit's settings adds a timeline for it, and dragging on that
  makes a resizable block, inside which the pointer shows -- the arrow, where the
  recording had the mouse, with a ring while its button is down. Any other on-or-off setting works the same way. A
  block, like a key, is a stretch of the recording, so a cut never moves it.
- **Audio timelines can be added**, beside the microphone's: music, a voiceover
  recorded afterwards, from the settings or by dropping a file on the editor.
  **A sound is placed in the video's time**, not the recording's, unlike keys and
  blocks: it is laid against the finished video, so taking a piece out does not
  move it or cut it. Each is moved, trimmed and made louder or quieter on its
  own timeline, and mixed over the microphone.
- **Claude is not asked again**: a replay uses the replies the recording got.
- **The output is an MP4, 1080p60.**
- **Streaming is not a feature.** Twitch or Zoom capture the app's window like any
  other, and the microphone is chosen there.

### Pictures and video

Neither is mathematics; both are windows, which is nearly the whole design. They
arrive by being dropped on the app (or from the menu, for a tablet).

- **Three corners crop** -- the frame moves, the picture does not. The fourth,
  bottom-right, is the grip, and resizes.
- **A video has a transport**: play, a bar, and two trim markers whose exact times
  can be typed, since a clip is often chosen for one particular second. It loops
  between them. There is no decoder in the app, and there will not be: `ffmpeg` is
  asked for frames.
- **A picture beside a canvas is named relative to it**, so a folder of them can be
  moved together.

### Opening, saving, and the menu

- **The app opens the canvas last worked on.** **New canvas** gives an empty one --
  empty, not a starting card that would only have to be deleted.
- **A canvas saves itself** whenever something you did settles, every few seconds
  while you type, and before another canvas opens or the app closes. **Opening a
  canvas is not a change**, so looking at a file never rewrites it.
- **Nothing made here is only in memory**: a canvas with no file gets an untitled
  one. Ctrl+S gives it a name, or says "now".
- **The Escape menu has no title.** It lists recent canvases (a fact about you, not
  about a canvas, so kept with your settings; ones that are gone are left out).
  Its last button is fullscreen -- borderless, not exclusive and not always on top,
  and remembered between runs. A press off the menu closes it, and a press the
  menu acted on belongs to the menu.

### Asking Claude

A **conversation** is a window on the canvas with Claude in it. It is for the two
things a separate chat was being used for anyway, done where the work is: asking
about mathematics, where Claude answers partly by *making* things -- boxes, values
to push on, new vizes -- and asking about the code on the canvas, from an error to
noticing a named structure being used without being named.

**Claude sees and changes the canvas directly.** It reads every box, its errors
and its current values; it makes boxes, edits them, deletes them and sets knobs.
**Each change is an undo
step**, like anyone else's, so nothing it does needs accepting first. Its actions
are not listed in the conversation: the canvas is where you see them.

**It does only what it is asked.** A question -- "how do I...", "remind me..." --
is answered in words, with no tools if the canvas and what it knows will do; it
makes or changes things only when told to, and if it thinks something would help
it says so in a sentence and stops. The answer is as long as the question needs:
a reminder of how something is written is a few lines. (An API reminder that came
back as a rewritten viz, a test box and a long answer, at 30 cents, is why.)

**Its boxes go beside the conversation**, each under the last. **Your boxes are
yours**: it changes one only when asked to change that box, and a box you are
typing in is left alone. **Every message carries the canvas**, so a question about
"this error" needs no looking up first -- **all of it the first time, and after
that only what has changed** since Claude last saw it, with where your caret is
and what is selected. Every copy sent stays in the conversation and is paid for
again with every request, so an unchanged box is named, not repeated.

**It sees a picture only when you ask it to look**, and only of the display window
your pointer is on as you send the message. Pictures are the dearest thing it can
be sent, and checking its own work by eye was the habit that ran up bills. A single message may go
round its tools at most forty times before it stops and asks to be told to go on,
so nothing can run up a bill unseen.

**Code it writes is a box.** Code is the source of truth here, so a reply holds
prose and mathematics, typeset by the app's own TeX, and code only inline and
short -- a signature, a call, a line; anything longer is put on the canvas near
the conversation, when it was asked for, and spoken of by name. **A name
in a reply is a mention**: pointing at it lights the thing on the canvas, as a
variable's name in a box does.

**What Claude is told is a file in the repo**, `prompt/system.md`, read with every
message: text to be read and changed like the vizes sketches, taking effect on the
next message. It is one prompt for everything. It does only what is asked, at the
length asked; it answers every part of what is asked; it says in a sentence when
a named structure is being used without being named; a small thing asked for it
does at once, since every change is an undo step; and for anything substantial it
comes back first with a short plan and a question.

**It speaks only when asked.** Nothing runs while you are working, so nothing
costs anything while you are working.

**There can be several**, each its own window and its own conversation, so a
question about a surface can sit beside the surface. A conversation is a fixed-size
window that scrolls; Enter sends and Shift+Enter starts a new line; while Claude is
working there is an indicator and a stop button, and its thinking is not shown.
**It is saved in the `.sketch`** like any window, so it moves with the canvas,
and opening the canvas again picks the conversation up where it was. Pictures
Claude looked at are not kept -- they are large, and it can be asked to look again.

**Throwing a conversation away can be undone**, and it comes back with everything
it said. **But what is said is not an undo step**: undo is about the canvas, and a
message is not taken back by Ctrl+Z -- rewinding is how. And a conversation thrown
away is not saved: close the canvas and open it again, and it is gone.

**Rewinding** to one of your messages -- pointing at it offers the button -- asks
each time whether to take the canvas back too. Either way what you said goes back
into the input, to be said again or changed; taking the canvas back is itself an
undo step, so it can be undone. What was spent stays spent.

**The model menu is the Claude Code app's**: each model's name, a line on what it
is for, a tick on the one in use -- and what it costs per million tokens, since
that is what choosing decides. It opens from the model's name in the header, and
only between replies. A model's thinking is its own and is not handed to another,
so switching drops the earlier thinking from what is sent. **The effort slider is
always visible**, since effort is the main control on both how deep an answer goes
and what it costs. **The context bar** under the header is how full the context is,
turning amber and then red as it fills, and the header says what the conversation
has cost. Claude may search the web, and may read the built-in vizes sketches to
make new vizes like them.

**One is started from the Escape menu** ("Ask Claude"), in the middle of the view
with the caret in it. **It is drawn over every other window**, and covers what
is under it: while the pointer is on a conversation, nothing beneath it is being
pointed at. **Its wheel scrolls it** rather than resizing it, since a window that
scrolls inside is one whose wheel is its own, and **its bottom-right corner
resizes it**, the words reflowing. While it has the caret it holds the keyboard
as a box does: canvas undo, Delete and the slide arrows stand aside, and Escape
takes the caret out rather than opening the menu. Your words sit in a bubble at
the right; Claude's are set across the window. A formula still arriving is held
back until it is whole, rather than shown raw for a moment.

**It runs on your own API key**, entered from the Escape menu and kept with your
settings -- encrypted to your Windows account, never in a canvas. That is billed
per token, separately from a Claude subscription; the context bar's cost is there
so that is never a surprise.

---

## 11. What works today

- **Reals and their knobs**: `float a; float b; float c = (a + b) / 2;` reads
  `a ∈ ℝ`, `b ∈ ℝ`, `c := (a+b)/2`, with one number line carrying all three.
- **Functions**: `float f(float x);` reads `f : ℝ → ℝ`, with a plot and a field;
  knobs reshape curves live; a prototype of any signature draws in its window.
- **Pictures, surfaces and volumes** of functions of the plane and of space.
- **Clifford algebras**, with points of space and spacetime in their windows, and
  functions to points drawn by being fed.
- **Colours**, in a picker, a cube and a cylinder.
- **Prose, slides, pictures and video.**
- **Sharing between boxes.**
- **Vizes**, for every value and function, in `vizes/`.

The sketches in `C:\sketches` show each: `demo.sketch` (a deck of six slides with
one of everything -- the quickest check that nothing is broken), `glsl`,
`realfunction`, `function`, `spaces`, `pga`, `colour`, `sharing`, `mum grids`.

---

## 12. Open questions

Answered ones move up into the sections above.

- **Most multivectors have no viz.** A point is drawn, but nothing shows a
  bivector, a motor, anything else -- a bivector of Cl(3,0,1) is a line in space
  and a motor a screw motion, and both are things one wants to *see*. This is the
  next thing, and the reason for the rest.
- **A typed multivector has no knob.** `Tri p = Tri(…);` still compiles its numbers
  in.
- **A multivector shows no injected value**: sixteen components would make the box
  wider than the screen. The answer is probably a window that shows one.
- **A function takes one argument.** A product of two spaces is the next shape, and
  wants a spread of pairs.
- **What is a knob for a function?** A function nobody defined has its body
  compiled in, so there is nothing to drag. Make the generated body's coefficients
  knobs, or accept that choosing a function means choosing a body?
- **The injected body is long**, and a box is sized for the worst case, so a box
  with a prototype is wide even with it hidden. Live with it, let hovered code
  overflow, or give the body a row of its own?
- **Composition**: `f | g`, `f(g)`, or `compose(f, g)`? It has to read `f ∘ g`.
- **A sample cannot be grabbed.** Dragging the *sampler* -- the box that made the
  spread -- is what would make sense.
- **A spread of answers is not joined up.** A viz saying how would turn a row of
  beads into a curve through space.
- **A merged plot no longer reframes** around every curve on it. Restoring it means
  asking the GPU what range a function covers.
- **Nothing brings a closed window back.** The menu could offer it, or the name in
  the equation could be clickable.
- **Should the prologue be visible anywhere?** It is the app's half of the bargain
  -- what `Complex` and an algebra are, what operators exist -- and nothing shows it.
- **`vec2` as a value** would want a 2D knob.
- **A paragraph cannot flow into the next**; a body that outgrows its slide runs off
  it, and nothing warns.
- **Video has no sound.**
- **Pictures are outside the no-overlap rule** while boxes are not. That seems
  right, but it is two placement rules.
- **Editability is not shown** now that ring and dot are gone. Does it need to be?
- **How is a transition asked for on the canvas?** A step of a slide, a key on a
  box? Outside the editor's cuts, only the command line can now.
- **What a cut's transition does not move yet**: conversations jump to where
  they end up, and a morphing box's code is one colour while it morphs.
