#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fstream>
#include <sstream>

#include <vector>

#include "app.h"
#include "recent.h"
#include "settings.h"
#include "logging.h"
#include "glext.h"
#include "glslops.h"
#include "raylib.h"
#include "rlgl.h"
#include "touch.h"

namespace {

struct Args {
  // The window opens at the shape a deck is made for, so a slide seen at its
  // own scale is exactly a screenful of it. The height is what kSlideHeight
  // means.
  int         width = 1200;
  int         height = 800;
  // A size asked for on the command line is the window wanted, so it wins over
  // coming back fullscreen.
  bool        sized = false;
  std::string screenshot;   // when set, save a PNG and exit
  // Dev only: print a file with its operators rewritten and exit, so the
  // rewrite can be checked without a window or a GPU.
  std::string rewrite;
  // Dev only: read a file as if it were a box's lines and print the shader it
  // assembles to. Same reason -- what a box turns into is worth being able to
  // read without a GPU in the room.
  std::string assemble;
  // raylib says a great deal about what it is doing and almost none of it is
  // worth reading. This turns the filter off when it is.
  bool verbose = false;
  int         frames = 0;   // 0 means run until the window is closed
  std::string open;         // a .sketch canvas to load at startup
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool hasValue = (i + 1 < argc);
    if (flag == "--width" && hasValue) { args.width = std::atoi(argv[++i]); args.sized = true; }
    else if (flag == "--height" && hasValue) { args.height = std::atoi(argv[++i]); args.sized = true; }
    else if (flag == "--screenshot" && hasValue) args.screenshot = argv[++i];
    else if (flag == "--frames" && hasValue) args.frames = std::atoi(argv[++i]);
    else if (flag == "--open" && hasValue) args.open = argv[++i];
    else if (flag == "--rewrite" && hasValue) args.rewrite = argv[++i];
    else if (flag == "--overloads") args.rewrite = "-";
    else if (flag == "--assemble" && hasValue) args.assemble = argv[++i];
    else if (flag == "--verbose") args.verbose = true;
    else if (flag[0] != '-') args.open = flag;   // a file dropped on the exe
  }
  if (!args.screenshot.empty() && args.frames <= 0) args.frames = 6;
  return args;
}

// Writes the frame currently being drawn to a PNG.
//
// Not raylib's TakeScreenshot: that reads the framebuffer while rlgl still has
// this frame's geometry sitting in an unflushed batch, so it would capture
// nothing but the background, and it also strips the directory from the path
// and oversizes the image by the monitor DPI factor.
void SaveFrame(const std::string& path) {
  rlDrawRenderBatchActive();  // submit everything queued this frame first

  const int width = GetRenderWidth();
  const int height = GetRenderHeight();
  unsigned char* pixels = rlReadScreenPixels(width, height);
  if (pixels == nullptr) return;

  Image image{};
  image.data = pixels;
  image.width = width;
  image.height = height;
  image.mipmaps = 1;
  image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  ExportImage(image, path.c_str());
  UnloadImage(image);
}

// Assets ship next to the executable; the source tree is the fallback so the
// binary also runs straight out of a build directory.
std::string ResolveAssetDir() {
  const std::string beside = std::string(GetApplicationDirectory()) + "assets";
  if (DirectoryExists(beside.c_str())) return beside;
#ifdef SKETCHBOOK_SOURCE_ASSET_DIR
  if (DirectoryExists(SKETCHBOOK_SOURCE_ASSET_DIR)) return SKETCHBOOK_SOURCE_ASSET_DIR;
#endif
  return beside;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  // Nothing graphical either: read a file as a box's lines and print what it
  // assembles to -- the shader, the sampler, or the viz's two halves.
  if (!args.assemble.empty()) {
    std::ifstream file(args.assemble, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "sketchbook: cannot read %s\n", args.assemble.c_str());
      return 1;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
    sb::mathprogram::Program program;
    program.Rebuild(lines, sb::mathprogram::Canvas{});
    for (const sb::mathprogram::Function& fn : program.functions()) {
      std::printf("// function %s : %s -> %s   lines %d..%d  %s\n", fn.name.c_str(),
                  fn.inputType.c_str(), fn.outputType.c_str(), fn.firstLine, fn.lastLine,
                  fn.sampled ? "sampled" : "special-cased");
    }
    if (!program.glslSampleSource().empty()) {
      std::printf("%s", program.glslSampleSource().c_str());
    } else if (!program.glslMeshSource().empty()) {
      std::printf("%s", program.glslMeshSource().c_str());
    } else {
      std::printf("%s", program.glslSource().c_str());
    }
    return 0;
  }

  // Nothing graphical: read a file, rewrite its operators, print it.
  if (!args.rewrite.empty()) {
    if (args.rewrite == "-") {
      std::printf("%s", sb::glslops::BuiltinOverloads().c_str());
      return 0;
    }
    std::ifstream file(args.rewrite, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "sketchbook: cannot read %s\n", args.rewrite.c_str());
      return 1;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string out;
    std::string error;
    if (!sb::glslops::Rewrite(buffer.str(), &out, &error)) {
      std::fprintf(stderr, "sketchbook: %s\n", error.c_str());
      return 1;
    }
    std::printf("%s", out.c_str());
    return 0;
  }

  // Deliberately no FLAG_WINDOW_HIGHDPI. GLFW is already per-monitor DPI aware,
  // so the window is sized in real pixels either way; turning the flag on adds
  // GLFW_SCALE_TO_MONITOR, which leaves the draw space scaled by the monitor
  // factor while GetMousePosition() keeps reporting unscaled coordinates. Off,
  // screen size, render size, mouse position and the WM_POINTER touch
  // coordinates are all the same pixel space.
  // Before the window, so the window's own narration goes through it too.
  sb::log::Install(args.verbose);

  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(args.width, args.height, "Sketchbook");
  if (!IsWindowReady()) {
    std::fprintf(stderr, "sketchbook: could not create a window\n");
    return 1;
  }
  // The GL entry points raylib does not wrap. After the window, because asking
  // the driver for one needs a current context.
  if (!sb::glext::Init()) {
    TraceLog(LOG_WARNING, "GLEXT: no glMemoryBarrier; compute results will be read back instead");
  }
  // Shaders are compiled on the thread that owns the context, and a box builds
  // several of them at once. This lets the driver spread that over threads of
  // its own instead.
  if (sb::glext::ParallelCompile()) {
    sb::glext::UseAllCompilerThreads();
  } else {
    TraceLog(LOG_WARNING, "GLEXT: parallel shader compile unavailable (KHR advertised %d, ARB %d)",
             sb::glext::HasExtension("GL_KHR_parallel_shader_compile") ? 1 : 0,
             sb::glext::HasExtension("GL_ARB_parallel_shader_compile") ? 1 : 0);
  }

  // Escape opens the options menu, so it must not also close the window.
  SetExitKey(KEY_NULL);
  SetTargetFPS(120);

  sb::touch::Install();

  sb::App app;
  std::string error;
  if (!app.Init(ResolveAssetDir(), &error)) {
    std::fprintf(stderr, "sketchbook: %s\n", error.c_str());
    sb::touch::Shutdown();
    CloseWindow();
    return 1;
  }

  // A capture runs for a fixed number of frames and quits. Anything that happens
  // to its window meanwhile -- a keystroke meant for another one -- is not work
  // to be saved over the canvas it opened.
  if (args.frames > 0) app.SetAutosave(false);

  // **Fullscreen is remembered.** Left filling the screen, the app comes back
  // filling it. Before the canvas opens, so the canvas is laid out for the
  // window it will actually be seen in. Not for a capture, and not when a size
  // was asked for: both want exactly the window they named.
  if (args.frames == 0 && !args.sized && sb::settings::Fullscreen()) app.SetFullscreen(true);

  if (!args.open.empty()) {
    if (!app.OpenCanvas(args.open)) {
      std::fprintf(stderr, "sketchbook: could not open %s\n", args.open.c_str());
    }
  } else {
    // **The app opens on what you were last working on.** Going back to it is
    // the commonest thing anybody does with this program; a blank canvas is one
    // button away in the menu. The recent list already leaves out a file that
    // is no longer there, so a moved or deleted one just means an empty canvas.
    const std::vector<std::string> recent = sb::recent::List();
    if (!recent.empty()) app.OpenCanvas(recent.front());
  }

  int frame = 0;
  while (!WindowShouldClose()) {
    app.Update();

    BeginDrawing();
    app.Draw();
    ++frame;
    if (!args.screenshot.empty() && frame == args.frames) SaveFrame(args.screenshot);
    EndDrawing();

    if (args.frames > 0 && frame >= args.frames) break;
  }

  app.Shutdown();
  sb::touch::Shutdown();
  CloseWindow();
  return 0;
}
