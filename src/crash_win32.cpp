// When the app crashes, it says where.
//
// A crash with nobody at a debugger is otherwise a window that vanishes. So an
// unhandled fault prints its code and the faulting thread's stack -- each frame
// as an offset into the executable, which `nm -C` on the build turns into the
// function it was in -- and then lets Windows carry on as it would have.
// Win32 and raylib-free, like the rest of that layer.
#include "crash.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace sb::crash {
namespace {

LONG WINAPI Report(EXCEPTION_POINTERS* info) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  std::fprintf(stderr, "CRASH: exception 0x%08lx, thread %lu, executable at 0x%llx\n",
               static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
               static_cast<unsigned long>(GetCurrentThreadId()),
               static_cast<unsigned long long>(base));
  // Walked from the fault itself rather than from here, using the unwind
  // tables every x64 function carries, so the frames are the ones that failed.
  CONTEXT context = *info->ContextRecord;
  for (int i = 0; i < 40 && context.Rip != 0; ++i) {
    const std::uintptr_t at = static_cast<std::uintptr_t>(context.Rip);
    std::fprintf(stderr, "CRASH:   0x%llx  (exe+0x%llx)\n", static_cast<unsigned long long>(at),
                 static_cast<unsigned long long>(at - base));
    DWORD64           imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
    if (function == nullptr) {
      // A leaf function: its caller's address is the top of the stack.
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void*   handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                     &establisher, nullptr);
  }
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install() { SetUnhandledExceptionFilter(Report); }

}  // namespace sb::crash

#else

namespace sb::crash {
void Install() {}
}  // namespace sb::crash

#endif
