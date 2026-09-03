#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"
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
  std::string screenshot;   // when set, save a PNG and exit
  int         frames = 0;   // 0 means run until the window is closed
  std::string open;         // a .sketch canvas to load at startup
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool hasValue = (i + 1 < argc);
    if (flag == "--width" && hasValue) args.width = std::atoi(argv[++i]);
    else if (flag == "--height" && hasValue) args.height = std::atoi(argv[++i]);
    else if (flag == "--screenshot" && hasValue) args.screenshot = argv[++i];
    else if (flag == "--frames" && hasValue) args.frames = std::atoi(argv[++i]);
    else if (flag == "--open" && hasValue) args.open = argv[++i];
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

  // Deliberately no FLAG_WINDOW_HIGHDPI. GLFW is already per-monitor DPI aware,
  // so the window is sized in real pixels either way; turning the flag on adds
  // GLFW_SCALE_TO_MONITOR, which leaves the draw space scaled by the monitor
  // factor while GetMousePosition() keeps reporting unscaled coordinates. Off,
  // screen size, render size, mouse position and the WM_POINTER touch
  // coordinates are all the same pixel space.
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(args.width, args.height, "Sketchbook");
  if (!IsWindowReady()) {
    std::fprintf(stderr, "sketchbook: could not create a window\n");
    return 1;
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

  if (!args.open.empty() && !app.OpenCanvas(args.open)) {
    std::fprintf(stderr, "sketchbook: could not open %s\n", args.open.c_str());
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
