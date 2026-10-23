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

// Starts a child with these as its standard handles, and **no other handle of
// this process**. Without the list, a child inherits every inheritable handle
// there is at that moment -- including the end of another child's pipe being
// set up on another thread, which then never closes: a face decoder started
// while a sound was being read once held that read open forever.
bool Launch(const std::string& commandLine, HANDLE in, HANDLE out, HANDLE err,
            PROCESS_INFORMATION* pi) {
  std::vector<HANDLE> handles;
  for (HANDLE h : {in, out, err}) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE &&
        std::find(handles.begin(), handles.end(), h) == handles.end()) {
      handles.push_back(h);
    }
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<unsigned char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
  const bool listed =
      handles.empty() ||
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                handles.size() * sizeof(HANDLE), nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in;
  si.StartupInfo.hStdOutput = out;
  si.StartupInfo.hStdError = err;
  si.lpAttributeList = attributes;

  std::wstring wide = Widen(commandLine);
  wide.push_back(L'\0');
  const BOOL ok = listed && CreateProcessW(nullptr, wide.data(), nullptr, nullptr,
                                           handles.empty() ? FALSE : TRUE,
                                           CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                           nullptr, nullptr, &si.StartupInfo, pi);
  DeleteProcThreadAttributeList(attributes);
  return ok != FALSE;
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

std::string Capture(const std::string& commandLine) {
  Pipe pipe;
  if (!pipe.Start(commandLine)) return {};
  std::string   out;
  unsigned char chunk[4096];
  for (;;) {
    const std::size_t got = pipe.Read(chunk, sizeof(chunk));
    out.append(reinterpret_cast<const char*>(chunk), got);
    if (got < sizeof(chunk)) break;   // a short read means the stream is over
  }
  return out;
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

  // Diagnostics are not wanted here: they would land in the middle of the
  // frames. A decoder that fails simply stops writing, and a short read says so.
  PROCESS_INFORMATION pi{};
  const bool ok = Launch(commandLine, nullptr, writeEnd, nullptr, &pi);
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

Feed::~Feed() { Stop(); }

bool Feed::Alive() const {
  return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool Feed::Start(const std::string& commandLine, const std::string& logPath) {
  Stop();
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 22)) return false;
  SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
  HANDLE log = CreateFileW(Widen(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  const HANDLE        said = log == INVALID_HANDLE_VALUE ? nullptr : log;
  PROCESS_INFORMATION pi{};
  const bool          ok = Launch(commandLine, readEnd, said, said, &pi);
  CloseHandle(readEnd);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (!ok) {
    CloseHandle(writeEnd);
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  writeEnd_ = writeEnd;
  return true;
}

bool Feed::Write(const unsigned char* data, std::size_t bytes) {
  if (writeEnd_ == nullptr) return false;
  while (bytes > 0) {
    DWORD wrote = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 20));
    if (!WriteFile(writeEnd_, data, chunk, &wrote, nullptr) || wrote == 0) return false;
    data += wrote;
    bytes -= wrote;
  }
  return true;
}

int Feed::Finish() {
  if (writeEnd_ != nullptr) {
    CloseHandle(writeEnd_);
    writeEnd_ = nullptr;
  }
  if (process_ == nullptr) return -1;
  int code = -1;
  if (WaitForSingleObject(process_, 120000) == WAIT_OBJECT_0) {
    DWORD exit = 0;
    if (GetExitCodeProcess(process_, &exit)) code = static_cast<int>(exit);
  }
  CloseHandle(process_);
  process_ = nullptr;
  return code;
}

void Feed::Stop() {
  if (writeEnd_ != nullptr) {
    CloseHandle(writeEnd_);
    writeEnd_ = nullptr;
  }
  if (process_ != nullptr) {
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 2000);
    CloseHandle(process_);
    process_ = nullptr;
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
