// The few OpenGL entry points raylib does not hand over.
//
// raylib wraps enough of GL for drawing and, since 4.3, for compute shaders and
// shader storage buffers. What it does not wrap is the part that says *when*
// one stage may see what another wrote. A compute shader's writes are not
// visible to the vertex fetch that follows until a memory barrier says so, and
// there is no way to ask raylib for one -- so the pointer is fetched here.
//
// Without it the only way to wait was to read the buffer back to the CPU, which
// is a stall and defeats the point of the buffer.
//
// It also draws, because `rlDrawVertexArray` is triangles and a path is lines.
//
// Win32, so no raylib header in the implementation: <windows.h> and raylib both
// declare Rectangle, CloseWindow and ShowCursor.
#pragma once

namespace sb::glext {

// After the window exists, since fetching a GL pointer needs a current context.
// False if the driver has no glMemoryBarrier, in which case Barrier does
// nothing and whoever depends on it must fall back to reading the buffer.
bool Init();
bool Available();

// The bits worth naming, straight from the GL headers.
inline constexpr unsigned int kVertexAttribArray = 0x00000001;
inline constexpr unsigned int kBufferUpdate = 0x00000200;
inline constexpr unsigned int kShaderStorage = 0x00002000;

// What a compute shader wrote becomes visible to whatever reads it next.
void Barrier(unsigned int bits);

// Primitive modes, again from the GL headers.
inline constexpr unsigned int kLines = 0x0001;
inline constexpr unsigned int kTriangles = 0x0004;

// glDrawArrays, which is GL 1.1 and therefore always there.
void DrawArrays(unsigned int mode, int first, int count);
// The same drawn `instances` times over, each told which it is by
// `gl_InstanceID`: every value in a window drawn by one call rather than one
// call each.
void DrawArraysInstanced(unsigned int mode, int first, int count, int instances);

// --- fences ------------------------------------------------------------------
//
// What a compute shader worked out is read back to the CPU -- where a viz is,
// how far the pointer is from it -- and reading it the moment it is asked for
// waits for the GPU to finish everything queued before it. A fence says when it
// has, so the answer is read on a later frame, when it is already there, and
// nothing waits. Null where the driver has none.
void* Fence();
// Whether the GPU has passed it yet. Never waits.
bool  Passed(void* fence);
void  DropFence(void* fence);

// --- KHR_parallel_shader_compile --------------------------------------------
//
// Lets the driver compile on threads of its own instead of on the one thread
// that owns the context. Asking for as many as it likes is the whole of the
// setup; the rest of the extension is a way to *poll* whether a compile has
// finished, which only helps a caller that does not immediately ask for the
// result -- and raylib asks immediately.
bool ParallelCompile();
void UseAllCompilerThreads();

// --- multisampled render targets -------------------------------------------
//
// The window is multisampled, and anything drawn straight onto it is smooth. A
// render texture is not: raylib makes only single-sampled ones, so a window
// that draws into one and then puts the result on screen -- the space window
// does, so its 3D scene can be clipped to its own square -- comes out with every
// edge a staircase. raylib can bind and blit a framebuffer but cannot allocate
// a multisampled one, so these do.
struct MultisampleTarget {
  unsigned int framebuffer = 0;
  unsigned int color = 0;
  unsigned int depth = 0;
  int          width = 0;
  int          height = 0;
};

// False, with nothing left allocated, if the driver will not give one -- and
// then whoever asked should carry on single-sampled rather than not drawing.
bool MakeMultisample(int width, int height, int samples, MultisampleTarget* out);

// Averages what was drawn into `from` down into an ordinary framebuffer, the
// same size, which is what can then be drawn from as a texture.
void Resolve(const MultisampleTarget& from, unsigned int intoFramebuffer);

// By hand, like every other GPU resource here: a destructor has no context.
void Release(MultisampleTarget* target);

// glReadPixels into `rgba`, which must hold width * height * 4 bytes, from the
// framebuffer that is bound. Origin bottom-left, as GL has it. A stall, so only
// for a handful of pixels on the frame something is actually being asked.
void ReadPixels(int x, int y, int width, int height, unsigned char* rgba);

// Whether the driver advertises an extension, by name. Asked of the extension
// string rather than of an entry point, because a driver can have one without
// the other and the difference is worth being able to see.
bool HasExtension(const char* name);

}  // namespace sb::glext
