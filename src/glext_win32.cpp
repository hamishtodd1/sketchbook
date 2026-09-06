#include "glext.h"

#if defined(_WIN32)

#include <windows.h>

// GL 1.1 is what opengl32.dll exports directly; anything newer has to be asked
// for by name at run time.
extern "C" __declspec(dllimport) void __stdcall glDrawArrays(unsigned int mode, int first,
                                                             int count);
extern "C" __declspec(dllimport) void __stdcall glGetIntegerv(unsigned int name, int* data);

namespace sb::glext {
namespace {

using BarrierFn = void(__stdcall*)(unsigned int);
BarrierFn g_barrier = nullptr;

using MaxThreadsFn = void(__stdcall*)(unsigned int);
MaxThreadsFn g_maxThreads = nullptr;

using GetStringiFn = const unsigned char*(__stdcall*)(unsigned int, unsigned int);
GetStringiFn g_getStringi = nullptr;

constexpr unsigned int kNumExtensions = 0x821D;   // GL_NUM_EXTENSIONS
constexpr unsigned int kExtensions = 0x1F03;      // GL_EXTENSIONS

}  // namespace

bool Init() {
  // wglGetProcAddress needs a current context, which is why this runs after the
  // window rather than beside it.
  g_barrier = reinterpret_cast<BarrierFn>(
      reinterpret_cast<void*>(wglGetProcAddress("glMemoryBarrier")));
  // The entry point resolving *is* the extension being present: it exists only
  // where parallel shader compile does. Two spellings, because the KHR name is
  // the later alias of an ARB extension and drivers do not all carry both --
  // this one answers to ARB and not to KHR.
  g_maxThreads = reinterpret_cast<MaxThreadsFn>(
      reinterpret_cast<void*>(wglGetProcAddress("glMaxShaderCompilerThreadsKHR")));
  if (g_maxThreads == nullptr) {
    g_maxThreads = reinterpret_cast<MaxThreadsFn>(
        reinterpret_cast<void*>(wglGetProcAddress("glMaxShaderCompilerThreadsARB")));
  }
  g_getStringi = reinterpret_cast<GetStringiFn>(
      reinterpret_cast<void*>(wglGetProcAddress("glGetStringi")));
  return g_barrier != nullptr;
}

bool HasExtension(const char* name) {
  if (g_getStringi == nullptr || name == nullptr) return false;
  int count = 0;
  glGetIntegerv(kNumExtensions, &count);
  for (int i = 0; i < count; ++i) {
    const unsigned char* text = g_getStringi(kExtensions, static_cast<unsigned int>(i));
    if (text == nullptr) continue;
    const char* have = reinterpret_cast<const char*>(text);
    const char* want = name;
    while (*have != 0 && *want != 0 && *have == *want) { ++have; ++want; }
    if (*have == 0 && *want == 0) return true;
  }
  return false;
}

bool Available() { return g_barrier != nullptr; }

void Barrier(unsigned int bits) {
  if (g_barrier != nullptr) g_barrier(bits);
}

void DrawArrays(unsigned int mode, int first, int count) {
  if (count <= 0) return;
  glDrawArrays(mode, first, count);
}

bool ParallelCompile() { return g_maxThreads != nullptr; }

void UseAllCompilerThreads() {
  // 0xFFFFFFFF is the spec's "as many as you like".
  if (g_maxThreads != nullptr) g_maxThreads(0xFFFFFFFFu);
}

}  // namespace sb::glext

#else

namespace sb::glext {

bool Init() { return false; }
bool Available() { return false; }
void Barrier(unsigned int) {}
void DrawArrays(unsigned int, int, int) {}
bool ParallelCompile() { return false; }
void UseAllCompilerThreads() {}
bool HasExtension(const char*) { return false; }

}  // namespace sb::glext

#endif
