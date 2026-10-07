#include "glext.h"

#if defined(_WIN32)

#include <windows.h>

// GL 1.1 is what opengl32.dll exports directly; anything newer has to be asked
// for by name at run time.
extern "C" __declspec(dllimport) void __stdcall glDrawArrays(unsigned int mode, int first,
                                                             int count);
extern "C" __declspec(dllimport) void __stdcall glGetIntegerv(unsigned int name, int* data);
extern "C" __declspec(dllimport) void __stdcall glReadPixels(int x, int y, int width, int height,
                                                             unsigned int format, unsigned int type,
                                                             void* data);

namespace sb::glext {
namespace {

using BarrierFn = void(__stdcall*)(unsigned int);
BarrierFn g_barrier = nullptr;

using MaxThreadsFn = void(__stdcall*)(unsigned int);
MaxThreadsFn g_maxThreads = nullptr;

using GetStringiFn = const unsigned char*(__stdcall*)(unsigned int, unsigned int);
GetStringiFn g_getStringi = nullptr;

using InstancedFn = void(__stdcall*)(unsigned int, int, int, int);
InstancedFn g_drawInstanced = nullptr;
using FenceSyncFn = void*(__stdcall*)(unsigned int, unsigned int);
using ClientWaitFn = unsigned int(__stdcall*)(void*, unsigned int, unsigned long long);
using DeleteSyncFn = void(__stdcall*)(void*);
FenceSyncFn  g_fenceSync = nullptr;
ClientWaitFn g_clientWait = nullptr;
DeleteSyncFn g_deleteSync = nullptr;
constexpr unsigned int kSyncGpuCommandsComplete = 0x9117;   // GL_SYNC_GPU_COMMANDS_COMPLETE
constexpr unsigned int kSyncFlushCommands = 0x00000001;     // GL_SYNC_FLUSH_COMMANDS_BIT
constexpr unsigned int kAlreadySignaled = 0x911A;           // GL_ALREADY_SIGNALED
constexpr unsigned int kConditionSatisfied = 0x911C;        // GL_CONDITION_SATISFIED

constexpr unsigned int kNumExtensions = 0x821D;   // GL_NUM_EXTENSIONS
constexpr unsigned int kExtensions = 0x1F03;      // GL_EXTENSIONS

// Framebuffers and renderbuffers are GL 3.0, so they are asked for by name too.
using GenFn = void(__stdcall*)(int, unsigned int*);
using DeleteFn = void(__stdcall*)(int, const unsigned int*);
using BindFn = void(__stdcall*)(unsigned int, unsigned int);
using StorageFn = void(__stdcall*)(unsigned int, int, unsigned int, int, int);
using AttachFn = void(__stdcall*)(unsigned int, unsigned int, unsigned int, unsigned int);
using StatusFn = unsigned int(__stdcall*)(unsigned int);
using BlitFn = void(__stdcall*)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
GenFn     g_genFramebuffers = nullptr;
GenFn     g_genRenderbuffers = nullptr;
DeleteFn  g_deleteFramebuffers = nullptr;
DeleteFn  g_deleteRenderbuffers = nullptr;
BindFn    g_bindFramebuffer = nullptr;
BindFn    g_bindRenderbuffer = nullptr;
StorageFn g_storageMultisample = nullptr;
AttachFn  g_attachRenderbuffer = nullptr;
StatusFn  g_status = nullptr;
BlitFn    g_blit = nullptr;

constexpr unsigned int kFramebuffer = 0x8D40;       // GL_FRAMEBUFFER
constexpr unsigned int kReadFramebuffer = 0x8CA8;   // GL_READ_FRAMEBUFFER
constexpr unsigned int kDrawFramebuffer = 0x8CA9;   // GL_DRAW_FRAMEBUFFER
constexpr unsigned int kRenderbuffer = 0x8D41;      // GL_RENDERBUFFER
constexpr unsigned int kRgba8 = 0x8058;             // GL_RGBA8
constexpr unsigned int kDepth24 = 0x81A6;           // GL_DEPTH_COMPONENT24
constexpr unsigned int kColorAttachment0 = 0x8CE0;  // GL_COLOR_ATTACHMENT0
constexpr unsigned int kDepthAttachment = 0x8D00;   // GL_DEPTH_ATTACHMENT
constexpr unsigned int kComplete = 0x8CD5;          // GL_FRAMEBUFFER_COMPLETE
constexpr unsigned int kColorBit = 0x4000;          // GL_COLOR_BUFFER_BIT
constexpr unsigned int kNearest = 0x2600;           // GL_NEAREST

template <typename Fn>
Fn Fetch(const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(wglGetProcAddress(name)));
}

bool HaveMultisample() {
  return g_genFramebuffers && g_genRenderbuffers && g_deleteFramebuffers &&
         g_deleteRenderbuffers && g_bindFramebuffer && g_bindRenderbuffer &&
         g_storageMultisample && g_attachRenderbuffer && g_status && g_blit;
}

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
  g_drawInstanced = Fetch<InstancedFn>("glDrawArraysInstanced");
  g_fenceSync = Fetch<FenceSyncFn>("glFenceSync");
  g_clientWait = Fetch<ClientWaitFn>("glClientWaitSync");
  g_deleteSync = Fetch<DeleteSyncFn>("glDeleteSync");

  g_genFramebuffers = Fetch<GenFn>("glGenFramebuffers");
  g_genRenderbuffers = Fetch<GenFn>("glGenRenderbuffers");
  g_deleteFramebuffers = Fetch<DeleteFn>("glDeleteFramebuffers");
  g_deleteRenderbuffers = Fetch<DeleteFn>("glDeleteRenderbuffers");
  g_bindFramebuffer = Fetch<BindFn>("glBindFramebuffer");
  g_bindRenderbuffer = Fetch<BindFn>("glBindRenderbuffer");
  g_storageMultisample = Fetch<StorageFn>("glRenderbufferStorageMultisample");
  g_attachRenderbuffer = Fetch<AttachFn>("glFramebufferRenderbuffer");
  g_status = Fetch<StatusFn>("glCheckFramebufferStatus");
  g_blit = Fetch<BlitFn>("glBlitFramebuffer");
  return g_barrier != nullptr;
}

bool MakeMultisample(int width, int height, int samples, MultisampleTarget* out) {
  if (out == nullptr || !HaveMultisample() || width <= 0 || height <= 0) return false;
  MultisampleTarget made;
  made.width = width;
  made.height = height;
  g_genFramebuffers(1, &made.framebuffer);
  g_genRenderbuffers(1, &made.color);
  g_genRenderbuffers(1, &made.depth);

  g_bindRenderbuffer(kRenderbuffer, made.color);
  g_storageMultisample(kRenderbuffer, samples, kRgba8, width, height);
  g_bindRenderbuffer(kRenderbuffer, made.depth);
  g_storageMultisample(kRenderbuffer, samples, kDepth24, width, height);
  g_bindRenderbuffer(kRenderbuffer, 0);

  g_bindFramebuffer(kFramebuffer, made.framebuffer);
  g_attachRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, made.color);
  g_attachRenderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, made.depth);
  const bool complete = g_status(kFramebuffer) == kComplete;
  g_bindFramebuffer(kFramebuffer, 0);

  if (!complete) {
    Release(&made);
    return false;
  }
  *out = made;
  return true;
}

void Resolve(const MultisampleTarget& from, unsigned int intoFramebuffer) {
  if (!HaveMultisample() || from.framebuffer == 0) return;
  g_bindFramebuffer(kReadFramebuffer, from.framebuffer);
  g_bindFramebuffer(kDrawFramebuffer, intoFramebuffer);
  // Nearest, because a resolve between two targets of the same size is the one
  // blit where the filter means "average the samples" whatever it is set to --
  // and a linear filter on a multisampled source is an error.
  g_blit(0, 0, from.width, from.height, 0, 0, from.width, from.height, kColorBit, kNearest);
  g_bindFramebuffer(kFramebuffer, 0);
}

void Release(MultisampleTarget* target) {
  if (target == nullptr) return;
  if (g_deleteRenderbuffers != nullptr) {
    if (target->color != 0) g_deleteRenderbuffers(1, &target->color);
    if (target->depth != 0) g_deleteRenderbuffers(1, &target->depth);
  }
  if (g_deleteFramebuffers != nullptr && target->framebuffer != 0) {
    g_deleteFramebuffers(1, &target->framebuffer);
  }
  *target = MultisampleTarget{};
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

void DrawArraysInstanced(unsigned int mode, int first, int count, int instances) {
  if (count <= 0 || instances <= 0 || g_drawInstanced == nullptr) return;
  g_drawInstanced(mode, first, count, instances);
}

void* Fence() {
  return g_fenceSync != nullptr ? g_fenceSync(kSyncGpuCommandsComplete, 0) : nullptr;
}

bool Passed(void* fence) {
  if (fence == nullptr || g_clientWait == nullptr) return true;
  // Flushed, so a fence nobody else is waiting behind still gets to the GPU.
  const unsigned int state = g_clientWait(fence, kSyncFlushCommands, 0);
  return state == kAlreadySignaled || state == kConditionSatisfied;
}

void DropFence(void* fence) {
  if (fence != nullptr && g_deleteSync != nullptr) g_deleteSync(fence);
}

void ReadPixels(int x, int y, int width, int height, unsigned char* rgba) {
  if (rgba == nullptr || width <= 0 || height <= 0) return;
  glReadPixels(x, y, width, height, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);
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
void DrawArraysInstanced(unsigned int, int, int, int) {}
void* Fence() { return nullptr; }
bool Passed(void*) { return true; }
void DropFence(void*) {}
void ReadPixels(int, int, int, int, unsigned char*) {}
bool ParallelCompile() { return false; }
void UseAllCompilerThreads() {}
bool HasExtension(const char*) { return false; }
bool MakeMultisample(int, int, int, MultisampleTarget*) { return false; }
void Resolve(const MultisampleTarget&, unsigned int) {}
void Release(MultisampleTarget*) {}

}  // namespace sb::glext

#endif
