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

#include <algorithm>
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

// ------------------------------------------------------------------- Pipe

Pipe::~Pipe() { Stop(); }

bool Pipe::Start(const std::string& commandLine) {
  Stop();

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) return false;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  // Diagnostics are not wanted here: they would land in the middle of the
  // frames. A decoder that fails simply stops writing, and a short read says so.
  si.hStdError = nullptr;
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::wstring wide = Widen(commandLine);
  wide.push_back(L'\0');

  const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writeEnd);
  if (!ok) {
    CloseHandle(readEnd);
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  readEnd_ = readEnd;
  return true;
}

std::size_t Pipe::Read(unsigned char* into, std::size_t bytes,
                       const std::function<bool()>& abort) {
  if (readEnd_ == nullptr) return 0;
  std::size_t got = 0;
  while (got < bytes) {
    if (abort && abort()) return got;

    DWORD available = 0;
    if (!PeekNamedPipe(readEnd_, nullptr, 0, nullptr, &available, nullptr)) {
      // The child closed its end: the stream is over.
      return got;
    }
    if (available == 0) {
      Sleep(2);
      continue;
    }
    const DWORD want =
        static_cast<DWORD>(std::min<std::size_t>(bytes - got, static_cast<std::size_t>(available)));
    DWORD read = 0;
    if (!ReadFile(readEnd_, into + got, want, &read, nullptr) || read == 0) return got;
    got += read;
  }
  return got;
}

void Pipe::Stop() {
  if (process_ != nullptr) {
    // The child is a decoder with nothing to save; it is told to stop rather
    // than waited for, or seeking would cost as long as decoding.
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 2000);
    CloseHandle(process_);
    process_ = nullptr;
  }
  if (readEnd_ != nullptr) {
    CloseHandle(readEnd_);
    readEnd_ = nullptr;
  }
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

Pipe::~Pipe() { Stop(); }

bool Pipe::Start(const std::string& commandLine) {
  Stop();
  process_ = popen(commandLine.c_str(), "r");
  return process_ != nullptr;
}

std::size_t Pipe::Read(unsigned char* into, std::size_t bytes,
                       const std::function<bool()>& abort) {
  if (process_ == nullptr) return 0;
  std::size_t got = 0;
  while (got < bytes) {
    if (abort && abort()) return got;
    const std::size_t read =
        std::fread(into + got, 1, bytes - got, static_cast<FILE*>(process_));
    if (read == 0) return got;
    got += read;
  }
  return got;
}

void Pipe::Stop() {
  if (process_ == nullptr) return;
  pclose(static_cast<FILE*>(process_));
  process_ = nullptr;
}

}  // namespace sb::proc

#endif
