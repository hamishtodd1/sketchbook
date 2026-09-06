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

// --- KHR_parallel_shader_compile --------------------------------------------
//
// Lets the driver compile on threads of its own instead of on the one thread
// that owns the context. Asking for as many as it likes is the whole of the
// setup; the rest of the extension is a way to *poll* whether a compile has
// finished, which only helps a caller that does not immediately ask for the
// result -- and raylib asks immediately.
bool ParallelCompile();
void UseAllCompilerThreads();

// Whether the driver advertises an extension, by name. Asked of the extension
// string rather than of an entry point, because a driver can have one without
// the other and the difference is worth being able to see.
bool HasExtension(const char* name);

}  // namespace sb::glext
