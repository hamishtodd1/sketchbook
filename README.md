# sketchbook

An infinite pan/zoom canvas for exploring mathematics through programming. A box
holds GLSL and reads as the mathematics it says; a value with no number gets a
knob, a function gets a window to be seen in, and dragging a knob changes
everything that depends on it at display rate. C++20 on
[raylib](https://www.raylib.com/).

![the initial screen](docs/screenshot.png)

`DESIGN.md` says what the program is and why it behaves as it does.
`TECHNICAL.md` says how that is kept in the code.

## Building

raylib is pulled in by CMake at configure time, so the only prerequisites are a
C++20 compiler and CMake 3.20+. The first configure clones and builds raylib and
takes a few minutes; after that it is cached in `build/_deps`.

```sh
cmake --preset mingw        # MSYS2 GCC; use --preset default for MSVC
cmake --build --preset mingw
./build/sketchbook.exe
```

The `mingw` preset points at `C:/msys64` and drives the build with MSYS `make`.
Building while the app is open works: the running copy is moved aside, not
overwritten.

The app opens the canvas you last worked on. To open a particular one, pass it
on the command line (`sketchbook.exe path.sketch`, or `--open path.sketch`) or
drop it on the window. `C:/sketches/demo.sketch` is a deck of six slides with one
of everything on it -- reals and their knobs, functions and their plots, a
shader, an algebra, prose, a picture and a video. Click a slide's rim to go to
it, then use the arrow keys.

## Using it

| Input | Action |
| --- | --- |
| Right-drag (or middle-drag) | Pan |
| Drag one finger | Pan |
| Wheel, trackpad pinch, two-finger pinch | Zoom -- or, over a window, resize it |
| Type with nothing selected | Start a new box where you are looking |
| Click a box | Put the caret there; the box turns into code |
| Click away | Put it back; rebuild if the text changed |
| `Ctrl`+`Enter` | Rebuild the box being edited |
| Hover a line | Show it as code, with the value the app chose in blue |
| Drag a knob on a number line | Choose that variable's value |
| Hover a knob, press play | Swing the value back and forth |
| Press a number line off its markers | Make a new variable there |
| Right-drag a number line | Scroll that axis |
| Right-drag, or two-finger drag, a 3D window | Turn it |
| Drag across a box's text | Highlight it |
| Drag highlighted text out of the box | That variable gets a number line of its own |
| `Ctrl`+drag a marker, curve or point | Pull it out into a window of its own |
| Hover any window's edge, drag the rim | Move it; drop on another of its kind to merge |
| Drag a window onto the cross | Close it |
| Drag the background | Lasso a selection |
| `Delete` | Throw the selection away |
| `Ctrl`+`C` / `X` / `V` | With a caret, the text; without one, the selected boxes |
| `Ctrl`+`Z` | Undo -- the box's own with a caret, the canvas's without |
| `Ctrl`+`Y` | Redo, for the canvas |
| `Ctrl`+`A` | Select all of the box |
| `Tab` / `Shift`+`Tab`, `Ctrl`+`/` | Indent, outdent, comment the highlighted lines |
| `Home` with no caret | Frame everything on screen |
| Click a slide's rim | Go to that slide |
| `left` / `right`, or a two-finger swipe, on a slide | Previous / next slide |
| Drag a picture's corner | Crop it |
| Drop a file on the window | A picture, a video, or a canvas to open |
| `Ctrl`+`S` / `Ctrl`+`O` | Save / open a canvas |
| `Esc` | The menu: new canvas, recent canvases, slides, files, fullscreen |

Canvases save themselves as you work. A canvas that never came from a file is
given one under `%APPDATA%/sketchbook/untitled`; `Ctrl`+`S` gives it a name. The
format is plain text, one record per line, so a canvas diffs and merges like
source.

A box whose first line starts with `\text` is prose: it is written in as words,
wraps to a measure set by the bar on its right edge, and has bold, italic and
justification controls above it.

Video needs `ffmpeg` and `ffprobe` on the path.

## Licence

The bundled fonts are OFL; see `assets/fonts/LICENSES.md`. raylib is zlib/libpng.
