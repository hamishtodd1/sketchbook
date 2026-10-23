#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fstream>
#include <sstream>

#include <thread>
#include <vector>

#include "app.h"
#include "viz.h"
#include "claude.h"
#include "crash.h"
#include "recent.h"
#include "settings.h"
#include "logging.h"
#include "glext.h"
#include "glslops.h"
#include "raylib.h"
#include "rlgl.h"
#include "touch.h"
#include "transition.h"
#include "capture.h"
#include "editor.h"
#include "recording.h"

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
  // Dev only: ask Claude one question and print the answer as it streams, with
  // what it used and cost -- the whole connection, with no window in the way.
  std::string ask;
  std::string model = sb::claude::kDefaultModel;
  std::string effort;       // empty is the model's own default
  // Dev only: open a conversation on the canvas and say this in it, so the
  // whole loop -- Claude, its tools, the canvas -- runs with nobody at the keys.
  std::string converse;
  // Dev only: do something to the first conversation on the canvas --
  // `menu`, `rewind:N`, `rewind-canvas:N`, `say:words` -- and, at the end of a
  // capture, write the canvas out, which a capture otherwise never does.
  std::string devChat;
  std::string devBox;
  std::string devKeys;
  std::string saveTo;
  std::string vizes;
  // Dev only: at the end of a capture, print the canvas as Claude is shown it --
  // which is also the quickest way to see what the compiler made of a box.
  bool        describe = false;
  // Dev only: open a window showing a transition between writings and nothing
  // else (transition.h) -- one line of code, or two formulas.
  std::vector<std::string> transition;
  // The editor, opened on a recording (editor.h): a program of its own, reached
  // only from here. `playhead` and `play` are where it starts, for a capture.
  std::string edit;
  double      playhead = 0.0;
  bool        play = false;
  // Prints the camera and microphone a recording would use, and exits.
  bool        listDevices = false;
  std::string editKeys;
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
    else if (flag == "--ask" && hasValue) args.ask = argv[++i];
    else if (flag == "--model" && hasValue) args.model = argv[++i];
    else if (flag == "--effort" && hasValue) args.effort = argv[++i];
    else if (flag == "--converse" && hasValue) args.converse = argv[++i];
    else if (flag == "--dev-chat" && hasValue) args.devChat = argv[++i];
    else if (flag == "--dev-box" && hasValue) args.devBox = argv[++i];
    else if (flag == "--dev-keys" && hasValue) args.devKeys = argv[++i];
    else if (flag == "--save-to" && hasValue) args.saveTo = argv[++i];
    else if (flag == "--vizes" && hasValue) args.vizes = argv[++i];
    else if (flag == "--describe") args.describe = true;
    else if (flag == "--edit" && hasValue) args.edit = argv[++i];
    else if (flag == "--playhead" && hasValue) args.playhead = std::atof(argv[++i]);
    else if (flag == "--play") args.play = true;
    else if (flag == "--list-devices") args.listDevices = true;
    else if (flag == "--edit-keys" && hasValue) args.editKeys = argv[++i];
    else if (flag == "--animate_text_transition" && hasValue) {
      args.transition.push_back(argv[++i]);
      // A second side, if one follows that is not a flag.
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        args.transition.push_back(argv[++i]);
      }
    }
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
  sb::crash::Install();
  const Args args = ParseArgs(argc, argv);

  // Nothing graphical: ask Claude something, and print the answer as it comes.
  if (!args.ask.empty()) {
    std::string key = sb::settings::ApiKey();
    if (key.empty()) {
      if (const char* env = std::getenv("ANTHROPIC_API_KEY")) key = env;
    }
    if (key.empty()) {
      std::fprintf(stderr, "sketchbook: no API key -- paste one from the Escape menu, or set "
                           "ANTHROPIC_API_KEY\n");
      return 1;
    }
    const sb::claude::Model* model = sb::claude::FindModel(args.model);
    sb::json::Value request = sb::json::Value::Object();
    request.Set("model", args.model);
    request.Set("max_tokens", 32000);
    if (!args.effort.empty()) {
      sb::json::Value config = sb::json::Value::Object();
      config.Set("effort", args.effort);
      request.Set("output_config", std::move(config));
    }
    // Automatic prompt caching: whatever was sent before is read back cheaply.
    sb::json::Value cache = sb::json::Value::Object();
    cache.Set("type", "ephemeral");
    request.Set("cache_control", std::move(cache));
    sb::json::Value message = sb::json::Value::Object();
    message.Set("role", "user");
    message.Set("content", args.ask);
    sb::json::Value messages = sb::json::Value::Array();
    messages.Push(std::move(message));
    request.Set("messages", std::move(messages));

    sb::claude::Call call(key, std::move(request));
    bool thinking = false;
    while (!call.Done()) {
      const bool now = call.Thinking();
      if (now && !thinking) std::fprintf(stderr, "[thinking]\n");
      thinking = now;
      const std::string text = call.TakeText();
      if (!text.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const std::string rest = call.TakeText();
    std::fwrite(rest.data(), 1, rest.size(), stdout);
    std::printf("\n");
    if (call.Failed()) {
      std::fprintf(stderr, "sketchbook: %s\n", call.Error().c_str());
      return 1;
    }
    const sb::claude::Usage usage = call.GetUsage();
    std::fprintf(stderr, "[%s, stopped: %s] in %ld (+%ld cache write, %ld cache read), out %ld",
                 args.model.c_str(), call.StopReason().c_str(), usage.input, usage.cacheWrite,
                 usage.cacheRead, usage.output);
    if (model != nullptr) std::fprintf(stderr, " -- $%.4f", usage.Dollars(model->price));
    std::fprintf(stderr, "\n");
    return 0;
  }

  // Nothing graphical either: read a file as a box's lines and print what it
  // assembles to -- the shader, the sampler, or a viz's three programs.
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
    } else if (!program.vizType().empty()) {
      if (program.error().present) {
        std::printf("// line %d: %s\n", program.error().line, program.error().message.c_str());
      }
      std::printf("// --- compute\n%s// --- vertex\n%s// --- fragment\n%s",
                  program.viz().compute.c_str(), program.viz().vertex.c_str(),
                  program.viz().fragment.c_str());
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

  if (args.listDevices) {
    const sb::capture::Devices devices = sb::capture::Find();
    std::printf("camera: %s\nmicrophone: %s\n",
                devices.camera.empty() ? "(none)" : devices.camera.c_str(),
                devices.microphone.empty() ? "(none)" : devices.microphone.c_str());
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

  if (!args.transition.empty()) {
    SetWindowTitle("Sketchbook transition");
    SetTargetFPS(120);
    const int code = sb::transition::Run(ResolveAssetDir(), args.transition, args.screenshot,
                                         args.frames, SaveFrame);
    CloseWindow();
    return code;
  }

  // Escape opens the options menu, so it must not also close the window.
  SetExitKey(KEY_NULL);
  SetTargetFPS(120);

  sb::touch::Install();

  if (!args.vizes.empty()) sb::SetVizesFolder(args.vizes);
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

  sb::Editor editor;
  if (!args.edit.empty()) {
    SetWindowTitle("Sketchbook editor");
    // The editor plays the recording's sound; nothing else here makes any.
    InitAudioDevice();
    // Dev: a test run makes no sound on whoever's speakers these are.
    if (std::getenv("SKETCHBOOK_TEST_CAPTURE") != nullptr) SetMasterVolume(0.0f);
    if (!editor.Open(args.edit, &app, &error)) {
      std::fprintf(stderr, "sketchbook: %s\n", error.c_str());
      app.Shutdown();
      sb::touch::Shutdown();
      CloseWindow();
      return 1;
    }
    editor.SetPlayhead(args.playhead);
    editor.SetPlaying(args.play);
    editor.DevKeys(args.editKeys);
  } else if (!args.open.empty()) {
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
    // The dev conversations wait half a second, until the canvas has built and
    // grown its windows, so they meet it as somebody pressing a button would.
    if (frame == 60) {
      if (!args.converse.empty()) app.Converse(args.converse, args.effort);
      if (!args.devChat.empty()) app.DevChat(args.devChat);
      if (!args.devBox.empty()) app.DevKeys(args.devBox, args.devKeys);
    }
    if (!args.edit.empty()) editor.Update();
    app.Update();
    // The editor draws the canvas into a frame of its own -- the monitor's, or a
    // render's -- and the window shows that frame rather than the canvas.
    if (!args.edit.empty()) editor.DrawFrame(app);

    BeginDrawing();
    if (args.edit.empty()) app.Draw();
    if (!args.edit.empty()) editor.Draw(app.Fonts());
    ++frame;
    if (!args.screenshot.empty() && frame == args.frames) SaveFrame(args.screenshot);
    EndDrawing();

    if (args.frames > 0 && frame >= args.frames) break;
  }

  if (args.describe) std::printf("%s", app.Describe().c_str());
  if (!args.saveTo.empty() && !app.SaveTo(args.saveTo)) {
    std::fprintf(stderr, "sketchbook: could not save %s\n", args.saveTo.c_str());
  }
  editor.Close();
  if (IsAudioDeviceReady()) CloseAudioDevice();
  app.Shutdown();
  sb::touch::Shutdown();
  CloseWindow();
  return 0;
}
