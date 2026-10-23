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
drop it on the window. `sketches/demo.sketch` is a deck of six slides with one
of everything on it -- reals and their knobs, functions and their plots, a
shader, an algebra, prose, a picture and a video. Click a slide's rim to go to
it, then use the arrow keys.

## Using it

| Input | Action |
| --- | --- |
| Right-drag (or middle-drag) | Pan |
| Drag one finger | Pan |
| Wheel, trackpad pinch, two-finger pinch | Zoom -- over a conversation, scroll it; inside a picture, surface, volume or space window, that window's own zoom |
| Drag a window's bottom-right grip | Resize it, keeping its shape (a conversation: width and height apart) |
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
| Double-click a name | Highlight the whole name |
| Drag highlighted text out of the box | That variable gets a number line of its own |
| `Ctrl`+drag a marker, curve or point | Pull it out into a window of its own |
| Hover any window's edge, drag the rim | Move it; drop on another of its kind to merge |
| Drag a window onto the cross | Close it |
| Drag the background | Lasso a selection |
| `Delete` | Throw the selection away |
| `Ctrl`+`C` / `X` / `V` | With a caret, the text; without one, the selected boxes |
| `Ctrl`+`Z` | Undo -- the box's own with a caret, the canvas's without |
| `Ctrl`+`Y` or `Ctrl`+`Shift`+`Z` | Redo -- the box's own with a caret, the canvas's without |
| `Ctrl`+`A` | Select all of the box |
| `Tab` / `Shift`+`Tab`, `Ctrl`+`/` | Indent, outdent, comment the highlighted lines |
| `Ctrl`+`D` | Highlight the name at the caret; again, add the next place it is written, with a caret of its own |
| `Ctrl`+`Shift`+`D` | Skip: let go of the newest highlight and take the next one instead |
| `Alt`+`D` | With several carets: count on from the first highlight (4 4 4 to 4 5 6, a a a to a b c) |
| `Escape` | With several carets, back to the first one |
| `Home` with no caret | Frame everything on screen |
| Click a slide's rim | Go to that slide |
| `left` / `right`, or a two-finger swipe, on a slide | Previous / next slide |
| Drag a picture's other corners | Crop it |
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

`--animate_text_transition "float a = 1.;"` opens a window that shows a line of
code turning into its LaTeX and back, three times, then closes. Given two lines,
`--animate_text_transition "float c = a / b;" "$a b^{-1}$"`, it shows one formula
turning into the other; a side in `$...$` is LaTeX as it stands, and anything
else is code, read.

**Record**, in the Escape menu, records the session into a new folder under
`Videos\sketchbook` (a red light top right says so, once the camera has started;
the same button stops it),
with your camera and microphone beside it. They are the first ffmpeg finds;
`sketchbook.exe --list-devices` says which, and a line `camera <name>` or
`microphone <name>` in `%APPDATA%\sketchbook\settings.txt` picks another (`none`
for neither).
`sketchbook.exe --edit <folder>` opens one in the editor:

| Input | Action |
| --- | --- |
| `Space` | Play / pause |
| Drag on the timeline | Scrub; a press on a piece also selects it |
| `C` | Cut at the playhead |
| `Delete` or `Backspace` | Take the selected piece out and close the gap (ripple delete); on a selected transition, make the cut snap |
| `T` | Take the selected transition away, or put it back |
| Drag a transition's right edge | Make it longer or shorter |
| Drag the face, or a box, on the monitor | Move it there from this moment on (its corner resizes it) |
| `F` | Take the face off, or put it back |
| Right-drag, wheel on the monitor | Move, zoom the camera from this moment on |
| `B` | Give the camera back to the recording |
| Click a diamond under the timeline | Select that key; `Delete` removes it |
| **Settings**, then a tick | Give that switch (the mouse pointer) a timeline |
| Drag on a switch's timeline | Make a block, where it is on; drag a block's edges or middle to change it, `Delete` removes it |
| **Settings** > Add sound..., or drop a sound file on the window | Lay it under the video at the playhead, on a timeline of its own |
| Drag a sound; its edges | Move it; trim it |
| `Up`, `Down` with a sound selected | A decibel louder, quieter |
| `Ctrl`+`Z`, `Ctrl`+`Y` | Undo, redo |
| Wheel / `Shift`+wheel over the timeline | Zoom it about the pointer / slide it |
| `Left`, `Right` (`Shift`: a frame) | A second back or on |
| `Home`, `End` | Either end |
| `Ctrl`+`R` | Render the edit to `video.mp4` in the recording's folder (1080p, 60 fps); `Escape` stops it |

The editor shows the video as it will be: the canvas framed 16:9, with your face
in the corner. Where a piece was taken out, half a second of transition (purple)
moves what was there to what comes next; clicking it selects it. A move of the
face, a box or the camera is a key at the playhead (diamonds under the timeline:
camera, face, boxes), reached over the half second before it and held after. The microphone plays along, cut
as the pieces are, and is silent through a transition; the rendered video has
the same sound. Every change is saved at once,
to `edit.txt` in the recording's folder; the recording itself is never changed.

## Licence

The bundled fonts are OFL; see `assets/fonts/LICENSES.md`. raylib is zlib/libpng.
