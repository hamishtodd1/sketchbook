#include "process.h"

#if defined(_WIN32)

#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <vector>

namespace sb::proc {
namespace {

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

}  // namespace

std::string Quote(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

Result Run(const std::string& commandLine, int timeoutMs) {
  Result result;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  // One pipe carries both streams, so the diagnostics stay in the order the
  // compiler emitted them.
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return result;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::wstring wide = Widen(commandLine);
  wide.push_back(L'\0');  // CreateProcessW may write into this buffer

  const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writeEnd);  // our copy; the child holds the only remaining one
  if (!ok) {
    CloseHandle(readEnd);
    return result;
  }
  result.started = true;

  // Drain the pipe while waiting. Draining matters: a compiler that emits more
  // diagnostics than the pipe buffer holds would otherwise block forever.
  const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
  std::vector<char> buffer(4096);
  bool running = true;
  while (running) {
    DWORD available = 0;
    while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      DWORD read = 0;
      if (!ReadFile(readEnd, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
          read == 0) {
        break;
      }
      result.output.append(buffer.data(), read);
    }

    if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
      running = false;
    } else if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
      result.timedOut = true;
      running = false;
    }
  }

  // Whatever was still in flight when the process exited.
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(readEnd, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
        read == 0) {
      break;
    }
    result.output.append(buffer.data(), read);
  }

  DWORD code = 0;
  if (GetExitCodeProcess(pi.hProcess, &code)) result.exitCode = static_cast<int>(code);

  CloseHandle(readEnd);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return result;
}

}  // namespace sb::proc

#else  // ----------------------------------------------------------- fallback

#include <array>
#include <cstdio>

namespace sb::proc {

std::string Quote(const std::string& value) { return "'" + value + "'"; }

Result Run(const std::string& commandLine, int /*timeoutMs*/) {
  Result result;
  FILE* pipe = popen((commandLine + " 2>&1").c_str(), "r");
  if (pipe == nullptr) return result;
  result.started = true;
  std::array<char, 4096> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }
  const int status = pclose(pipe);
  result.exitCode = (status == -1) ? -1 : (status / 256);
  return result;
}

}  // namespace sb::proc

#endif
