# sketchbook: how it is kept

How the decisions in `DESIGN.md` are kept in the code: mechanisms, file names, and
the things learnt the hard way. Read the part for an area before changing it. If
this file and `DESIGN.md` disagree, `DESIGN.md` wins -- fix this one, or ask.

---

## Building and testing

```sh
cmake --preset mingw          # or --preset default for MSVC
cmake --build --preset mingw
./build/sketchbook.exe
```

raylib is pulled in by CMake at configure time (cached in `build/_deps`). The
`mingw` preset points at `C:/msys64` and drives the build with MSYS `make`.
Assets are copied next to the executable after each build, and the binary falls
back to the source tree (`SKETCHBOOK_SOURCE_ASSET_DIR`); the built-in vizes are
found through `SKETCHBOOK_VIZES_DIR`, the repo's `vizes/`.

**Building while the app is open works.** Windows will not overwrite or delete a
running executable but will rename one, so a step before the link moves the old
`sketchbook.exe` aside (`cmake/MoveAsideRunning.cmake`) and writes the new one
under the usual name. A moved-aside copy is deleted by the first build after
nothing is running it. One build directory; nobody closes anything.

**Dev flags**: `--screenshot <file.png> --frames <n> --width <w> --height <h>`
capture a frame without anyone at the keyboard; `--open <file>` opens a canvas;
`--verbose` turns the log filter off; `--assemble <file>` prints the shader a
box's lines become (or, for a viz box, its three programs); `--rewrite <file>`
prints a file with its operators rewritten; `--overloads` prints the built-in
operator overloads.

**Testing on this machine.**

- Hamish usually has his own `build/sketchbook.exe` open. Never kill it -- only
  the test instances launched with test flags.
- A launch without `--open` opens his most recent canvas, so every interactive
  test opens a **scratch copy**, never a file in `C:\sketches` directly.
- Back up `%APPDATA%\sketchbook\recent.txt` before `--open` runs and restore it
  after, or the scratch copies leak into his recent list. Delete any untitled
  files a test creates.
- Tests pass `--width`/`--height`, so they never go fullscreen or touch
  `settings.txt`.
- Synthetic input (`SetForegroundWindow`, `mouse_event`, `keybd_event`) only
  works when the driver is run through the PowerShell tool, not from Bash; check
  that the window really got the foreground.
- A capture does not auto-save (see *Saving*), so screenshot runs cannot damage a
  canvas.
- A sweep of every sketch in `C:\sketches` with `--verbose`, grepping for errors,
  is the quickest whole-app check; `demo.sketch` has one of everything.

---

## A value never recompiles

**Every number a variable is given is a uniform, at all times**: the one injected
for `float a;`, one typed into a declaration, and one typed into an assignment
further down -- at the top of a box or inside a function. The shader does not
contain the number at all.

- **A variable is a GLSL local (or global); each literal assigned to it is a
  uniform.** `foo` declared on line 2 and assigned on lines 9 and 13 gets `foo_2`,
  `foo_9` and `foo_13` -- named for the box as well, `foo_9_12`, since a function
  from one box is compiled into another (see *Sharing*). The lines read
  `float foo = foo_2;  foo = foo_9;  foo = foo_13;`.
- **Dragging moves the governing literal**: `Variable::GoverningLiteral(line)`,
  the last at or before the reading line.
- **A rebuild that comes out the same is not compiled.** `Program::Rebuild`
  compares the shader it assembled with the one running; if they match -- which
  they do whenever only these numbers changed -- it hands the numbers over as
  uniforms and stops. Undo goes through this too: a step gives a box its old lines
  back (`RestoreLines`) rather than building a new one.
- **A drag writes the number into the text** (`WriteLiteral`, `WriteColorLiteral`)
  as it goes, for a literal that is typed there; an injected one has no text.
- A **colour** is three numbers behind one `vec3` uniform; a **multivector** is
  its members behind one `float[n]` uniform (`Literal::components`,
  `Literal::array`). They are saved as one `val` record per part: `val c.r`,
  `val p.e012`.
- **A compile shows as the busy cursor** (`busy.*`): it runs on the drawing
  thread, so no frame can say so; the system cursor is the one thing still drawn.

The checks: drag a knob and nothing compiles; type a new number into a
declaration and nothing compiles; undo either and nothing compiles. Count shader
loads in a `--verbose` log with and without the gesture.

---

## Performance

What already protects the frame budget, and must not be undone:

- **Font atlases are cached on a size ladder with an LRU** (`fontstore.*`), so
  zooming does not re-rasterise every glyph. **The LRU may not evict anything used
  since the frame began**: a glyph hands its caller a pointer straight into an atlas
  texture, so evicting mid-frame dangles that pointer (letters come out as smudges)
  and rasterises a whole face inside the frame. Over budget with nothing evictable,
  the cache runs large for a frame.
- **The atlas is packed by each glyph's own rectangle**, with a skyline packer in
  `BuildFont`, not by raylib's `LoadFontFromMemory`, which assumes no glyph is
  taller than its nominal size. KaTeX's Size1..Size4 glyphs are several times it;
  one was placed past the end of the bitmap and read past it again -- an access
  violation in `memcpy` on whichever rung the zoom landed on. A glyph that still
  will not fit is made empty.
- **Nothing is measured off an atlas.** Advances and outline boxes come from the
  font's own tables (`hmtx`, `glyf`), exactly proportional to size. raylib's
  advances are whole pixels at the rung rasterised, so a window changed size as
  the canvas zoomed. Check: render a box at several zooms and divide by the zoom.
- **Per-line LaTeX layout is cached** (`CodeBlock::EnsureCache`), rebuilt only when
  that line's text or the em size changes.
- **`CodeBlock::Measure` is memoised.** It is asked a dozen times a frame per box,
  and was half of all the app's CPU (1.0 ms of a 1.9 ms frame on twenty boxes).
  The answer is kept with a `Shape` saying what it answers; a pan only translates
  rows. **A signature is compared whole**, so nothing is forgotten by halves. Which
  algebra a box works in, and whether it is prose, are deliberately not in it:
  both are read off the lines, so the line cache moves when they do -- which also
  lets `EnsureCache` skip asking for the algebra on the common path. (The line
  cache does compare the *canvas's* algebra, since a box with no `Cl` line is
  built in it.)
- **Drawing is culled to the screen**: the grid iterates only dots in view, and
  windows off screen are skipped.
- **Compiles happen on the drawing thread** (it owns the GL context), so no gesture
  may cause one. A video's decoding is the one thing on a thread of its own.

**Measure before optimising.** The syntax highlighter was once suspected and costs
0.02 ms a frame; `tex::DrawLayout` never showed up. Where it stands on the
sketches in `C:\sketches`: 1.6 to 2.5 ms of CPU a frame, worst 4 to 7 ms, against
8.3 ms at 120 Hz. The one spike is a canvas *load* -- 100 to 200 ms, every shader
compiled at once.

---

## Deleting leaves nothing behind

- **A destructor cannot free what is on the GPU**: releasing needs the context, and
  destructors can run after the window has gone. Whoever destroys a box calls
  `CodeBlock::ReleaseGpu` first; `~CodeBlock` complains if they forgot. Loading a
  canvas was quietly leaking every shader on the old one until this was written
  down. The same holds for every panel (`ReleaseGpu`), `VizStore::ReleaseGpu`,
  and every `VizPane`.
- **Every pointer into the blocks stops pointing before the blocks stop
  existing.** The panels hold raw `CodeBlock*` (hence `Forget`), and so do
  `focus_`, `dragBlock_` and the selection drag. The keys that delete stand down
  while a drag is on, since Ctrl+X mid-carry would free the boxes being carried.
- **Anything that grows with use has a bound**: undo keeps 200 steps; the atlases
  are an LRU; a video decodes a handful of frames ahead and its thread blocks.
- **A child process stops when its window goes**: closing a video ends its
  `ffmpeg`. Check: open a canvas with a video, throw it away, and no `ffmpeg`
  runs.
- **The disk**: nothing a box does writes a file. The app writes the canvas, the
  recent list, `settings.txt`, and an untitled canvas's own file.

---

## Every edge is smooth

The window is 4x multisampled, so anything drawn as geometry is smooth for
nothing. Two ways of drawing do not get that:

- **Into a render target**: raylib's are single-sampled. The space window draws
  into a multisampled framebuffer resolved into its texture (`glext`'s
  `MakeMultisample` and `Resolve`; raylib can blit one but not make one).
- **In a fragment shader**: an edge a shader decides -- a band it discards outside,
  a ray that hits or misses -- is not geometry and stays a staircase. Such an edge
  is written as *coverage*: how much of the pixel is inside, from `fwidth` or by
  looking again, never yes or no.

Check a new window by magnifying a screenshot with no smoothing.

---

## raylib is not permanent

It is a convenience, not a foundation; keep it extractable.

**raylib-free**: `mathline.*`, `mathprogram.*`, `vizglsl.*`, `compiler.*`,
`process.*`, `selection.h`, `filedialog.*`, `video.*`, `recent.*`, `settings.*`,
and a type's own file (`types/RealFunction.*`, `types/Clifford.*`,
`types/Color.*`, `types/examples.h`). The Win32 files (`*_win32.cpp`, `glext`,
`busy`, `touch_win32`) *cannot* include it: `<windows.h>` and raylib both declare
`Rectangle`, `CloseWindow` and `ShowCursor`.

**Coupled**: rendering and input -- roughly a hundred raylib calls across
`blocks.cpp`, `app.cpp`, `ui.cpp`, `media.cpp`, the panels, `viz.cpp`,
`viewport.cpp`, `grid.cpp`, `text.cpp`, `tex/tex_layout.cpp` and `fontstore.cpp`,
plus `Vector2`, `Rectangle` and `Color` as vocabulary types in the render layer.
`fontstore.*` is the deepest hook (raylib's atlas packing, and its `fontSize`
being a stbtt pixel height rather than an em).

Rules:

- **New logic goes in a raylib-free file** if it is about mathematics, parsing, or
  talking to the outside world. Check by trying.
- **No raylib types in domain headers.**
- **Funnel drawing through the existing helpers** (`text::Draw`,
  `tex::DrawLayout`, `DrawCard`, `DrawRim`) so the surface to reimplement stays
  small and listed.

---

## Assembling a box into a shader

**A box builds one shader**, with `sbMode` choosing which end to use: 0 its
picture, 1 the report of its values, 2 the functions it draws, 3 the samples it
feeds, 5 a function viz's `grab`. It used to build three from the same lines;
folding them took `realfunction` from 43.8 ms to 4.5 and `glsl` from 20.4 to 8.7.

- **Every drawing of a function returns before the body**, so a field does not pay
  for the box. Before any of them, every variable is set to the value it was
  *given* where declared (the `seed`). That line used to sit below the picture,
  surface and volume branches, which then drew with every variable at nought and
  knobs that did nothing -- so it comes first.
- **The trace is written only when the box is being asked**, guarded, or every
  pixel of every frame would race to write it.
- **The vertex half is ours** (`kVertexShader`), not raylib's default, because
  reporting a value asks about one point rather than the point being drawn, and a
  varying cannot be shadowed by the local that chooses between them.
- **The app supplies the ends; the box is the middle**: the fragment interface,
  one uniform per literal, the box's variables as globals, then `void main() {`
  round the lines. **Line for line** with the box: a line the app handles itself
  (a sigil, a `Cl` line) leaves a blank behind, so a diagnostic lands on its row.
- **A box's variables are globals**, declared in the prologue before the
  functions, because GLSL has nowhere to put a nested function and a hoisted
  function cannot see a local. (`realfunction.sketch` failed silently with
  `undefined variable "a"` until this.) A free variable *inside* a function stays
  a local of that function, untraced. `vec2 q = vec2(0.5);` -- a line with no
  mathematical reading -- is passed through and stays a local.
- **A box's lines appear twice**: hoisted above `main` (functions, padded to one
  line per line) and again inside it. Mapping a driver's line back takes two
  starts (`compiler::ShaderLines`), plus a range for shared code (`sharedAt`); the
  body is tried first.
- **`mathprogram` knows nothing about GL.** It assembles and stops in
  `State::Shading`; the host compiles on the GL thread and calls
  `ShaderCompiled`. Driver logs come in two shapes, both parsed in
  `compiler::ParseGlslLog`.
- **A box with no variables and no functions never reaches the compiler.**
- **Closing is remembered per kind** (`CodeBlock::kPicture`, `kSurface`,
  `kVolume`, `kColour`), since a box can define a picture and a surface at once.

**Compile cost is generated algebra**: `pga` about 60 ms and `mum grids` 205 ms,
cold, since a Cl(3,0,1) or Cl(2,2) box is well over 100 KB of GLSL. The driver's
cache saves a recompile of exactly the same source, and of nothing else -- the
whole reason a changed number must never change the source. Measured, a 120 KB
box does not stutter at runtime (`pga` 1.0 ms mean, 2.8 ms worst). The lever, if
it is ever needed: emit only the products a box actually calls.

**`KHR_parallel_shader_compile` is not available here**: the driver advertises
neither the KHR nor the ARB extension in a 4.3 core context (checked against the
extension string, sanity-checked against `GL_ARB_compute_shader`). `glext.*`
detects it and will use it wherever one turns up.

**One language.** The C++ toolchain is gone; nothing launches a compiler.
`process.*` stays for `ffprobe` (`Capture`).

---

## Reading values back

The shader writes **every real's value after every line** into one shader storage
buffer (`traceBuffer_`), in the single invocation that runs the box, and the
*when* is chosen afterwards by reading a different row -- so moving the caret is
free. A multivector or colour is one slot per component. That needs GL 4.3, which
is why the context is 4.3. A slot for a variable **not yet in scope is NaN**, not
zero, since zero is a value. A read is a stall, so it only happens when something
it depends on (a knob, a clock) has moved (`ProbeIfNeeded`).

**`rand`**: GLSL has no state between calls but a global, so `rand()` keeps a hash
state moved on by each call, seeded by the pixel; `randChanging()` has a second
state with the frame mixed in, so asking for one never moves the other. When
reporting, every pixel runs the body for a different variable, so all are seeded
alike.

---

## The LaTeX reading

- **A line read in the light of the lines around it**: `mathline::Parse` takes an
  `Inside` saying which function encloses the line, from spans worked out once by
  `mathline::FindFunctions` -- the same spans the hoisting uses, so "where does this
  function end" is one answer. `Inside::closes` is what tells a function's closing
  brace from a loop's.
- **Finding a mention in LaTeX**: `mathline` wraps every identifier in
  `\sbref{k}{…}`, which typesets as its argument; the tag rides through parsing
  and layout onto every item, so the renderer can ask where identifier k landed.
- **The wiring diagram** is drawn from `mathline::Wiring`, a small graph of leaves
  and operator nodes made for it -- not the parser's tree -- so nothing outside the
  parser needs to know about precedence or calls.
- **The precedence lives in both halves**: `glslops` (the shader) and `mathline`
  (the LaTeX). `mathline` was once left on C's precedence, so `a + b | c` ran one
  way and read the other. A new operator goes in both.
- **The long tilde** (`~(a + b)`): the fonts draw a tilde at several widths (text,
  Size1..Size4, AMS -- half an em to two and a third). The nearest is chosen, and
  its *stroke* is read off the outline (its middle and thickness at a hundred
  points) and redrawn at exactly the wanted width (`FontStore::Stroke`,
  `WideStroke` in `tex_layout.cpp`), rather than stretched -- stretching thins the
  slopes and blunts the ends. Past the widest it also grows taller, as the 0.35
  power of the extra length, up to 1.8x. It is a filled shape, so multisampling
  smooths it. `\widehat` could take the same path in one word, but nothing writes
  one yet.

### The TeX engine (`src/tex/`)

A real implementation of TeX's box-and-glue model for maths mode, not string
substitution:

- Atoms are classified Ord/Op/Bin/Rel/Open/Close/Punct/Inner, with inter-atom
  spacing from TeX's table (TeXbook ch. 18). A binary operator that cannot be
  binary -- a leading minus, one after another -- is reclassified, which is why
  `-x` is tighter than `a - x`.
- Fractions, radicals, scripts and limits use the Computer Modern σ/ξ parameters
  (`texparams` in `fontstore.h`). `\left(`…`\right)` grows a delimiter by choosing
  among Size1..Size4, centred on the maths axis.
- Supported: the symbols in `tex_symbols.cpp`, scripts with limits on big
  operators, `\frac` `\dfrac` `\tfrac` `\binom`, `\sqrt[n]`, `\left`/`\right`,
  `\big`..`\Bigg`, accents, `\overline`, `\underline`, `\text`, `\operatorname`,
  `\math*` alphabets, spacing, style switches, and the matrix and `cases`
  environments.
- Every codepoint/face pairing was checked against the bundled fonts' cmaps.
  `\neq` and `\notin` have no glyph anywhere and are built as `\not` on `=` and
  `\in`, as KaTeX does.
- Parsing is forgiving: an unknown command is drawn as literal text and reported
  through `Layout::error`.
- Known gaps: no blackboard-bold face (`\mathbb` falls back to bold); no italic
  correction before a superscript; the KaTeX Computer Modern has no bold-italic,
  so asking for both gives bold.

### Fonts at any zoom

Nothing is drawn through `BeginMode2D`: the canvas transform is applied on the CPU
(`Viewport::ToScreen`) and the zoom is folded into the *font size*. `FontStore`
keeps atlases on a size ladder (16/24/32/48/64/96/128 px em) and uses the rung
nearest the size being drawn. raylib's `fontSize` is a stbtt pixel *height*
(ascent to descent), not an em, and faces disagree on the ratio, so the store reads
`head`/`hhea` from each TTF and converts the em it wants into the per-face size.

---

## Operators

GLSL has no operator overloading, but it has function overloading, so **the
rewrite needs no types** (`glslops.*`):

    a * b -> gp(a, b)     a ^ b -> op(a, b)     ~a -> rev(a)
    a | b -> ip(a, b)     a & b -> rp(a, b)     a >> b -> sw(a, b)
    a + b -> add(a, b)    a - b -> sub(a, b)    -a -> neg(a)
    a / b -> div(a, b)    x += y -> x = add(x, y), and so on

The same names exist for GLSL's own types -- `float gp(float, float)`,
`vec3 add(vec3, float)`, including mixed int/float pairs so nothing is ambiguous --
so a blanket rewrite is safe on ordinary code (`glslops::BuiltinOverloads`).

- **Line breaks survive the rewrite**, so diagnostics land on the written line.
- **`return` is a keyword, not a name**: `return (a) * b;` looked like a call of
  `return`, so it is lifted out before the climb and the operator wraps only what
  is returned.
- `dual` stays a function, since the regressive product is defined through it.

---

## Clifford algebras (`types/Clifford.*`)

- **Generated on demand** by a `Generator` that wants each type a box mentions and
  closes over **products only** (geometric, inner, outer), plus each type's dual
  and single grades. Closing over addition too would want the union of every
  pair -- the whole lattice, 30 types and 3.5 MB of code. A sum of two unlike
  things is written when it is wanted, and widens to the nearest existing type
  otherwise. The default types every ordinary box gets are `Una`, `Bi` and `Even`
  (`DefaultTypes`); a viz window goes without them.
- **What GLSL lacks forces three things**: no default member initialisers, so
  every function fills every component; no forward declaration of a struct, so
  types are emitted in dependency order; no constructor overloading, so a value is
  made by listing every component (which is what an example value is), and
  widening is a function per type (`toEven`, `toBi`) that the declaration calls --
  at the top of a box, inside functions, in assignments, and on every `return` of
  a function returning a multivector (every `return` on a line).
- **`GlslStructs`** emits only the structs and basis of exactly the named types --
  what a lean viz build uses.
- **The sandwich is checked against the blade algebra**: a quarter turn takes e1
  to e2, and twenty random `Even >> Bi` pairs agree component for component with
  the triple product from `BladeProduct`.
- **A free multivector's knob**: `Tri p;` gets `uniform float p_3_7[4];` and the
  line `p = Tri(p_3_7[0], …)`. Example values are `ExampleComponents`, deterministic
  in the name.

---

## Sharing between boxes (`App::ShareAcrossCanvas`)

- Every box's build works out what it **offers**: its functions as the GLSL it
  compiles them to (with the uniforms inside them), its free values as the uniform
  behind the number given at the declaration, and every name it defines.
- Once a frame the app lists which version of each box's offer it has
  (`sharedVersion`); when that changes -- or the function vizes do -- it gathers
  them into one `mathprogram::Canvas` (`Gather`), and each box asks whether what
  it *took* changed (`ImportsStale`, comparing an `importKey`). Only those are
  rebuilt, and only a different shader is compiled.
- A box **takes** what it mentions and does not define, and whatever that mentions
  (`Take`). Values arrive as a uniform and a global set where drawing starts;
  functions arrive as declarations first, then text, so functions in different
  boxes can call each other in any order.
- Knob values are copied across each frame (`PullImports`).
- A box being written in (focused and dirty) is skipped until the caret leaves.
- A mistake inside imported code is worded "in something shared from another
  box"; an undeclared name that is a value another box derives gets a message
  saying only free values and functions are shared (`DerivedElsewhere`).
- The colour order is canvas-wide (`Canvas::colourOrder`, `VariableColor`).
- `C:\sketches\sharing.sketch` shows it.

---

## Vizes

### A viz window (`vizglsl.*`)

`Viz(Type, "Dw");` then GLSL functions with standard names; anything else is a
helper visible to that window's own functions.

| | |
| --- | --- |
| `void mesh()` | lays geometry down with `emit(p, n, uv)` (or `emit(p, n)`), three to a triangle |
| `void vert(T v, inout vec3 p, inout vec3 n)` | where a vertex goes, in the dw's space |
| `vec4 frag(T v, vec3 p, vec3 n, vec2 uv)` | optional; default is phong in a 3D dw, `sbTint` in a flat one |
| `vec3 position(T v)` | where wires attach |
| `float grab(T v)` | pixels from the pointer |
| `T drag(T v)` | the value the pointer asks for |

Pieces: `mesh2`/`vert2`/`frag2`/`position2` … up to 4; a fifth is an error on its
line. `grab` and `drag` are one per viz.

Handed to every stage: `sbPointer` (pixels from the window's corner),
`sbScreen(p)`, `sbRayOrigin`/`sbRayDir` (the pointer as a ray in dw space),
`sbEye`, `sbSize`, `sbViewProj`, `sbAcross()`/`sbUpward()` (screen directions in
dw space), `sbPixel` (a chrome pixel in dw units, in a flat dw), `sbTint` (the
fragment stage), and `sbUncut`, which `vert` sets for something not in the space
the walls cut.

**Three programs per window**: a compute program running `mesh` and the questions
(`sbMode` 0 mesh, 1 position and grab for every value, 2 drag one value), and a
vertex/fragment pair for the drawing. Each stage gets the helpers and *its own*
standard functions; the others' lines are blanked, not removed, so a line number
is the same in all three -- and so a `frag` using `fwidth` or `discard` does not
break the other stages. Prologues are padded to one length so one map
(`ShaderLines`) reads diagnostics from any of them.

**Lean first.** Each program is built first with only the structs of the algebra's
types (`GlslStructs`), and with the whole algebra only if that fails, whose log is
then the one reported. Three programs carrying the whole algebra were 224 ms cold
(130 KB of products a ball never calls); lean, the ball is about 95 ms cold. The
rest is the driver's price for any compute program here, 50 to 80 ms, paid on
editing a viz. After the first run the driver's cache makes a built-in about 2 ms.

### On the GPU (`viz.*`)

- `VizGpu`: one window compiled. `mesh` runs once per build into a buffer per
  piece (16384 vertices × 8 floats, a count in front); only the count is read
  back, once.
- `VizValues`: one dw window's values in a buffer -- each value's tint (4 floats)
  then its numbers -- uploaded only when the run changes. **One instanced draw per
  piece** (`DrawArraysInstanced`), the vertex shader reading the mesh by
  `gl_VertexID` and the value by `gl_InstanceID`, with an empty VAO bound (a core
  draw needs one). Back-face culling is off while a viz draws: a mesh wound the
  way anyone would write it must not vanish (this cost an hour once).
- **Nothing waits for the GPU**: `position`, `grab` and `drag` answers are
  dispatched, fenced (`glext::Fence`), and read on a later frame once
  `glext::Passed`. They are asked only when something they depend on moved (the
  values, the camera, the window), when the pointer is over the window, or while
  dragging. A drag is therefore a frame behind the pointer.
- **Asking uses the real pointer** (`carry::Position()`), not `hover::Pointer()`,
  which is "nowhere" during a held gesture -- that bug sent a dragged point off to
  (−552, 808, 648).
- `VizPane`: what one dw window hands its viz and hears back (items, the run, the
  answers), shared by the space window, the number line and the colour window.
  Panes are kept by a runtime window id, and released when their window goes.

### Which vizes there are (`VizStore`)

- The canvas's own viz boxes (`local_`), else the built-ins: `vizes/vizes.sketch`
  always, and `vizes/Cl(p,q,r) vizes.sketch` for the canvas's algebra. Each
  library is its boxes read from the file (`block` … `> line` … `end`), built
  against a canvas gathered from them alone, never drawn. Re-read when the
  algebra changes, or when a file's timestamp changes (checked once a second).
- **A built-in is compiled the first time a dw asks** (`Find` polls the box). A
  library viz that fails is logged once, with a `VIZ:` prefix.
- A local viz that does not compile gives way to the built-in.

### The dws' spaces

- **Space / Spacetime**: world space; `FrameFor` builds the same view-projection
  raylib's `BeginMode3D` uses (`GetCameraMatrix`, `MatrixPerspective` with
  `RL_CULL_DISTANCE_NEAR/FAR`), so what the viz is told and what it is drawn with
  agree. Cut: `sbCut`, `sbLimitsMin/Max`. **Which values go in is asked of the
  vizes**: `SpacePanel::Sync` takes every variable whose type `VizStore::Has` a viz
  for the window (no compile needed), and keeps a pane per window and type
  (`panes_`, keyed by window id and type), since one viz draws one type. A hit
  test asks every pane and takes the nearest by each viz's own `grab`. A real goes
  in as its one number, anything else as its parts (`PartsOf`); a drag still needs
  an array knob, so only a multivector can be dragged here yet. The space window
  used to accept `Tri` alone, fixed in C++ (`kPointType`) -- the drift again.
- **Axis**: x is the value, y the same units upward from the line; the matrix
  maps that to the *row's* clip space, and the viz draws with the viewport set to
  the row (flush the batch before changing it, restore after), which clips a
  marker off the end. The line keeps its chrome: ticks, labels, play button,
  off-the-end arrows. A hit test at a point that is not the pointer (a second
  finger) goes by `position` rather than `grab`.
- **Picker / Cube / Cylinder**: the picker's plane, the ring's outer edge at radius
  1; the cube is RGB − 0.5, the cylinder HSV (radius s/2, height v − 0.5).
  Orthographic, from the window's own `Camera2`. Items are sorted far to near by
  the dw's own placement of the colour, since a flat draw has no depth test. The
  triangle's marker and the drag inside the triangle are the picker's own C++.

### A viz of a function

- Named by in and out: `FunctionType` / `FunctionTypeOf` (R, R2, R3, C); windows
  per type in `FunctionWindows`; `sbShape` per window in `ShapeOf` (Axis 0, Plot 1,
  Picture 2, Surface 3, Volume 4).
- **Spliced into every box that defines a function of its type**
  (`VizzesSource`, `VizzesMain` in `mathprogram.cpp`). Every function and global
  the window defines is renamed with a `_Type_Dw` suffix (`Rename`; not in
  comments, not after a dot), `frag` → `sbFrag_T_D`, `grab` → `sbGrab_T_D`, and
  `value` → `sbValue_T_D`, a dispatch over the box's functions of that type by
  `sbIndex` -- a function's place in `program.functions()`, which every drawing
  path uses (`BeginField`, `DrawFunctionInto`, `GrabAt`).
- The canvas carries the vizes (`Canvas::functionVizes`), filled each frame by
  `VizStore::FunctionVizes` for the types some box needs; a change re-gathers, and
  `VizzesKey` in the import key rebuilds exactly the boxes affected.
- **Standalone check**: the window is compiled alone against a `value` that
  returns nought (`Sources::standalone`, `FunctionVizReady`). Only a passing one is
  offered.
- **Helpers** (`FunctionHelpers`): `sbPaint`, `sbColourOf`, `sbThickness` (by
  output type); `sbEye`, `sbFrame`, `sbFilm`, `sbMid`, `sbHalfSpan` for the two
  with a camera; `sbSurfaceCamera` and `sbMarch` (the surface march, calling
  `sbValue_R2ToR_Surface` by name) for a surface; `sbVolumeCamera`, `sbHub`,
  `sbClip` for a volume. Uniforms: `sbRange`, `sbVertical`, `sbThick`, `sbOrbit`,
  `sbLift`, `sbPixel`, `sbAt`.
- **A curve's `grab`** is `sbMode` 5: one fragment into the feed target, writing
  `sbGrab_T_D(sbAt)` to `sbFnOut[0]` and read back (`CodeBlock::GrabAt`).
  `PlotPanel::AskCurveAt` asks each shown curve's box at the press and takes the
  nearest within reach. (It used to compare the pixels round the pointer before
  and after each curve was drawn.)

### How the vizes drifted

Viz boxes arrived on 2026-09-04 with the fallback "a type with no viz keeps the
drawing the app was born with". Every window added after that was drawn in C++ in
its dw's file, because the viz box of the day was 2D, in screen pixels, with no
normals, lighting or fragment shader -- and nobody extended it or said that was a
choice. The plot's viz stopped working when functions became GLSL, since it
compiled alone and could not call the function. CLAUDE.md held both "a viz is the
user's" and the fallback, so nothing flagged the contradiction. Rule: see
`DESIGN.md` §9.

---

## Functions

- **The curve** (`RToR@Plot`) is coverage: the gap to the curve, measured square
  to it by dividing by the slope (taken from one step either side), faded over a
  pixel with `fwidth`. KNOWN: it breaks up slightly where very steep *and* sharply
  curved, since the slope uses a fixed step.
- **The surface** marches 160 steps, halving eight times onto the crossing -- the
  colour is the height at the hit, so taking the detected step banded the surface
  along the ray. **Only edges are supersampled**: a ray that passed within a pixel
  and a half before landing, or clipped a crest and came out before it was that
  deep, is marched four more times across its pixel and averaged by coverage. A
  pixel and a half because the march sees its approach a dozen pixels apart; both
  sides are looked for, or half the edge stays stepped.
- **The volume** is clipped to its cube by the slab test, then 96 steps
  accumulated front to back. Its cube is drawn in C++ as twelve lines, so the
  camera is built twice and the two must agree exactly.
- **Both are marched in the box's own fragment shader**, not meshed: no vertex
  buffer, second program or draw path, and the same loop shape either way.
- **Cameras**: the surface's eye is `(-sin yaw · cos pitch, sin pitch,
  cos yaw · cos pitch)`; the space window rotates a point about +y. Those orbit
  opposite ways for the same sign of drag, so what has to match between windows
  is where the camera ends up, not the sign. (The space window was once left
  turning the other way from the surface and volume.)
- **Prototypes**: `ExampleFunctionBody` in `types/RealFunction.*`; a prototype
  with a later body in the same box loses its span and is hoisted as written.
- **Feeding** is `sbMode` 3, one fragment per sample, after the body. `pick` is
  the sampler's word because GLSL reserves `sample`. Bounds: `kMaxSamples` 256,
  `kMaxStride` 16.

---

## Space and spacetime windows (`spacedw.*`)

Four GPU lessons not to undo:

- **It draws multisampled**, into a 4x framebuffer resolved into the shared
  texture; a plain render texture gave every edge a staircase.
- **One render target between all windows**, drawn into and blitted at once;
  released when the last one goes, by hand. It is 16:9, the windows' shape, so
  the projection needs no crop.
- **raylib frees a material's shader with its model.** The models share shaders
  (the sky's is swapped between two), so they are handed back the default shader
  before unloading, and the real ones are released once each.
- **An indexed mesh keeps its winding in its index buffer.** raylib's cube is
  indexed and its sphere is not, so turning the cube inside out and uploading only
  vertices and normals flipped its lighting while leaving it facing the same way --
  a solid block hiding everything inside.

The windows `Sync` keeps an arrangement: a point already in a window stays there;
a new one goes to its box's home window, which is also where a fed spread is
shown. The canvas file records each window's points.

---

## Interaction plumbing

- **Nothing hovered while held** (`hover.h`): the app sets one flag a frame from
  the list of gestures; every *hover* reads `hover::Pointer()` (nowhere while
  held), and a gesture's own code reads the real pointer. So what a held thing
  says about itself -- its wires, its rim, the drop target -- is untouched.
- **A drag in flight owns the pointer**: a panel whose `Dragging()` is true gets
  input first, or a release over another window's rim would be lost. Reaching for
  a *selected* window's rim carries the selection. Only the menu opening abandons a
  gesture.
- **A carry can follow a finger** (`carry.h`): with two fingers down the emulated
  mouse follows the first (still) finger, so a window pulled out by the second
  would never move. The panels ask `carry::` for position and button.
- **The closed-hand cursor is drawn**, since GLFW's "hand" is the pointing one: the
  system cursor is hidden and ours drawn in two passes, to read on light and dark.
  Losing the window counts as letting go, so it is never left hidden.
- **What is under the pointer** is asked the same way for mouse and finger
  (`App::FindPullable`). A plot with one curve needs no question. A field is under
  the pointer anywhere along its row; off every marker, the last painted wins.
- **A dw's name on hover** (`windowname.h`): a dw offers its name while pointed at
  or carried, as it draws, and the app writes the last one offered after every
  window, so nothing covers it. The name is the one a viz box uses.
- **Pointing at a name asks every window where it is.** A window reports what
  is under the pointer (`ActiveVariables`), and the text's side of the same
  question is `lightUp` in `App::Update`, which asks each panel's `MarkerFor`.
  A window standing for a value needs both, or the light goes one way only: the
  colour and space windows had only the first, so hovering a colour's name lit
  nothing. A new dw that shows values adds its `MarkerFor` to that list.
- **A wire's side** is one bool per held value, frozen while held (`WireAnchor`).
  Continuous geometry was tried and undone as a second mechanism for the same
  problem. Wiring is meant to stay simple: the smallest fix, and revert growth.
- **The lasso**: the eight things every panel does with a selection (catch, clear,
  ask, find a rim, remember positions, move, draw rims, erase) are written once in
  `selection.h`; a panel only says where its n-th window is and what closing one
  means. A third copy in `spacedw.cpp` had drifted, testing the raw stroke rather
  than the closed loop. The loop closes with a cubic Bezier from the stroke's
  first two and last two points.
- **Where a window goes first** (`dock.h`): flanking slots alternate right/left
  down two columns, one height for all; the order is set once where the panels are
  chained (`App::Init`, `SetBefore`), so a new kind is one line.
- **Placement**: `kDotsPerLine` from the font at startup; `kGridMinPx/MaxPx`
  (halved when the dots doubled); `SnapStep` uses an in-between dot only once it
  has faded at least halfway in; `SnapToGrid` in `grid.*`; every box placement
  goes through `App::PlaceBlock`. Drawing the dots at 1920x1080 costs half a
  millisecond, only while they are up.
- **Undo** (`TrackUndo`): a step is the canvas text without the camera (4 KB, a
  tenth of a millisecond for the demo deck), taken once nothing is held and a
  moment has passed since the last click, key or wheel. Restoring keeps a box whose
  lines are unchanged (shader, caret history and all), moving it to its recorded
  position exactly, not re-snapped. Pictures and videos are reloaded from disk on
  every undo -- the next thing to keep, if a canvas full of them makes undo slow.
- **Touch**: real multi-touch comes from `WM_POINTER` (`touch_win32.cpp`, which
  subclasses the window procedure; `GetPointerType`/`GetPointerInfo` resolved at
  run time). raylib's touch API is mouse-synthesised on Windows desktop. Legacy
  mouse events from touch pass through, so a tap still works the menu; the
  viewport ignores mouse panning while a finger is down and briefly after.
  `FLAG_WINDOW_HIGHDPI` is deliberately off: with it, the draw space is scaled
  while `GetMousePosition()` is not. Touch work is deferred: keep touch paths
  minimal until a touch pass. Known edge: holding a number line with a finger
  presses on it, which makes a stray value before a pull; untested on a
  touchscreen.
- **Two values made into a third**: "together" is the distance halved, floored at
  90 px and capped at 200 (it was 46 px, and no hand could reach it). A finger is
  followed by its id where that exists and by its last position otherwise, and
  "have they met" is asked before "are both still there", since a digitizer drops
  one of two touching fingertips exactly as the gesture succeeds.
- **Slides by swipe**: on a touchpad, Windows reports a two-finger horizontal swipe
  as horizontal wheel, piled up until decisive, one page per sweep (GLFW inverts
  the axis, so "right" arrives negative). On a touchscreen the canvas holds still
  until the fingers have travelled; a change of separation makes it a pinch from
  that moment. A window under the fingers takes the pan but never the swipe.
- **Delete slide** sits just inside the rim's grab band; a slide too small on
  screen for it is not offered one; going to the button keeps the offer open.
  A slide's body goes under its title by the height the title turned out to be.
- **Prose**: rows are not lines -- the geometry is a list of rows, each a slice of
  a line, the slices partitioning it so every caret position is in one row. The
  caret is a byte offset in the source, so it survives the source toggle. It is
  clamped above the sigil, and so are the deletes. The strip is drawn after every
  box, since a neighbour can land over it; its toggle sits in the strip, not at
  the box's edge, which an unwrapped paragraph would push off screen. The measure
  bar sits clear of the rim's grab band.
- **Clipboard**: the system one, for text -- the caret is a byte offset into UTF-8,
  so text goes in as bytes and a newline splits the line. Windows stay on our own.
- **Pasted text** is inserted without the carried indentation (`InsertCodepoint`
  carries it for typed Enter only).

---

## Media (`media.*`, `video.*`)

`ffmpeg` is asked for raw frames, read off a pipe on a worker thread into a
**bounded** queue -- nothing decoded to disk, nothing held but the next few frames.
Seeking restarts the child, so scrubbing asks for a seek every so often rather
than every frame. `video.*` is raylib-free: it hands back bytes, and `media.cpp`
makes the texture. If `ffmpeg` is not on the path, the window says so. Crop
handles sit inside the rim's grab band and clear of the transport.

---

## Saving, settings and the log

- **The format** is plain text, one record per line, written by `App::WriteCanvas`
  and read by `LoadCanvas`. A save is **written beside the file and moved over
  it**, so a crash leaves the old file whole.
- **Auto-save** uses the undo machinery's idea of a change, so opening a canvas, a
  build coming back, or an axis framing itself never rewrites a file. Every few
  seconds while a box is being written in, and before another canvas opens or the
  app closes.
- **A capture does not auto-save**: a window popping up during a capture once took
  a stray keystroke, made it a box, and saved it over the canvas.
- **Untitled canvases** live in `%APPDATA%/sketchbook/untitled`, named for when
  they started; the file goes once Ctrl+S names it.
- **Facts about the user, not a canvas**, live in `%APPDATA%/sketchbook`:
  `recent.txt` (written the moment a canvas opens or saves; missing files left
  out) and `settings.txt` (fullscreen, written the moment it changes).
- **Always-code** is a canvas record, `alwayscode 1`, written only when ticked and
  applied in `ReadCanvas` (so a new canvas, a load and an undo step all set it).
  It is a static flag on `CodeBlock` (`SetAlwaysCode`), read by `ReadsAsCode`,
  folded into the layout signature's flags at bit 6, and it makes `Injection`
  return nothing, so a box is neither drawn with nor sized for one. A launch
  with `--width`/`--height`, or a capture, ignores the fullscreen setting.
- **Fullscreen** is done by hand -- title bar off, window over the monitor -- since
  raylib's `ToggleBorderlessWindowed` also makes the window topmost, so alt-tab put
  other apps underneath it and clicks went to a window you could not see.
- **Picture paths** under the canvas's folder are written relative to it; others
  stay absolute; absolute paths in older canvases still load.
- **The log is filtered, not turned down**: raylib tests the level before the
  callback, and the callback is the only way shader errors are caught at all. A
  whitelist of this app's prefixes survives (`logging.h`: `CANVAS:`, `MEDIA:`,
  `BLOCKS:`, `GLEXT:`, `SPACE:`, `SHADER BOX:`, `VIZ:`); a new prefix must be
  added there to be heard. `log::Catch` captures raylib's compile errors into a
  string for the length of a compile. `--verbose` turns the filter off.
- **A press the menu acted on belongs to the menu**, including the press that
  closed it, or choosing a recent canvas would also click into the new canvas.

---

## Key files

| file | what it holds |
| --- | --- |
| `main.cpp` | entry point, window setup, frame capture, the dev flags |
| `app.*` | the scene, focus and input routing, saving and loading, undo, sharing |
| `blocks.*` | the card: per-line rendering and editing, code and prose, its GPU side |
| `mathline.*` | one line of code → LaTeX, and what shape a function is |
| `mathprogram.*` | assembling a box into its shader; sharing; function vizes spliced in |
| `vizglsl.*` | a viz window turned into its programs (raylib-free) |
| `viz.*` | a viz on the GPU, a window's values on their way to it, which vizes there are |
| `vizes/` | the built-in vizes, as sketches |
| `types/` | a supported type: `Clifford.*`, `Color.*`, `Complex.*`, `RealFunction.*`, `RealViz.*` (number line), `RealFunctionViz.*` (plot), `examples.h` |
| `spacedw.*` | the space and spacetime windows |
| `colordw.*` | the colour window: picker, cube, cylinder |
| `shaderwindow.*`, `surfacedw.*`, `volumedw.*` | pictures, surfaces, volumes |
| `media.*`, `video.*` | pictures and video as windows; the ffmpeg pipe |
| `glslops.*` | the operator rewrite and the built-in overloads |
| `compiler.*` | reading a driver's log |
| `glext.*` | GL entry points raylib does not wrap: barriers, instanced draws, fences, multisampled targets |
| `selection.h` | the lasso, and the eight things every panel does with a selection |
| `dock.h` | where a window a box makes goes before anybody moves it |
| `hover.h`, `carry.h`, `windowname.h` | what the pointer is over; what a carried window follows; the name above a dw |
| `grid.*`, `resize.*` | the dots and snapping; the wheel-resize rule |
| `fontstore.*`, `text.*`, `tex/` | fonts, plain text, the TeX engine |
| `highlight.*` | Monokai colouring for a line of GLSL |
| `ui.*`, `recent.*`, `settings.*` | the Escape menu; recent canvases; fullscreen |
| `busy.*`, `logging.h` | the busy cursor; the log filter and `log::Catch` |
| `touch.*`, `touch_win32.*`, `viewport.*` | multi-touch; the pan/zoom camera |
| `filedialog_win32.*`, `process.*` | open/save dialogs; running a child process |
