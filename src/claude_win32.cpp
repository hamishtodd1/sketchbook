// The HTTPS half of talking to Claude, through WinHTTP: part of Windows, so
// nothing is installed or run alongside the app. A child process (curl, say)
// would also have put the key on a command line, where any program can read it.
#include "claude.h"

#if defined(_WIN32)

#ifndef WINVER
#define WINVER 0x0603
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winhttp.h>

#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace sb::claude {
namespace {

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

std::string Words(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
      return "Could not find api.anthropic.com -- is the internet connected?";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not connect to api.anthropic.com.";
    case ERROR_WINHTTP_TIMEOUT: return "The API took too long to answer.";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "The connection to the API was lost.";
    case ERROR_WINHTTP_SECURE_FAILURE: return "The secure connection to the API failed.";
    default: return "Network error " + std::to_string(code) + ".";
  }
}

// Every handle opened, closed once on the way out -- except a request handle
// another thread has already closed to stop it.
struct Handles {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  Abort*    abort = nullptr;
  ~Handles() {
    {
      const std::lock_guard<std::mutex> lock(abort->mutex);
      if (request != nullptr && abort->request == request) WinHttpCloseHandle(request);
      abort->request = nullptr;
    }
    if (connect != nullptr) WinHttpCloseHandle(connect);
    if (session != nullptr) WinHttpCloseHandle(session);
  }
};

}  // namespace

int Post(const std::string& key, const std::string& body,
         const std::function<void(std::string_view)>& piece, Abort* abort, std::string* error) {
  Handles h;
  h.abort = abort;
  const auto fail = [&]() {
    const DWORD code = GetLastError();
    bool        stopped = false;
    {
      const std::lock_guard<std::mutex> lock(abort->mutex);
      stopped = abort->aborted;
    }
    if (error != nullptr) *error = stopped ? std::string("Stopped.") : Words(code);
    return 0;
  };

  h.session = WinHttpOpen(L"sketchbook", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (h.session == nullptr) {
    // Older Windows has no automatic proxy; the system default will do.
    h.session = WinHttpOpen(L"sketchbook", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (h.session == nullptr) return fail();
  // Resolve, connect and send are quick or not at all. Receiving is the long
  // one: a reply that is thinking hard says nothing for a while, though the
  // stream sends a ping now and then to show it is still there.
  WinHttpSetTimeouts(h.session, 0, 30000, 60000, 300000);

  h.connect = WinHttpConnect(h.session, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (h.connect == nullptr) return fail();
  h.request = WinHttpOpenRequest(h.connect, L"POST", L"/v1/messages", nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
  if (h.request == nullptr) return fail();
  {
    const std::lock_guard<std::mutex> lock(abort->mutex);
    if (abort->aborted) {
      if (error != nullptr) *error = "Stopped.";
      return 0;
    }
    abort->request = h.request;
  }

  const std::wstring headers = Widen("x-api-key: " + key +
                                     "\r\nanthropic-version: 2023-06-01"
                                     "\r\ncontent-type: application/json\r\n");
  if (!WinHttpSendRequest(h.request, headers.c_str(), static_cast<DWORD>(-1),
                          const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                          static_cast<DWORD>(body.size()), 0)) {
    return fail();
  }
  if (!WinHttpReceiveResponse(h.request, nullptr)) return fail();

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return fail();
  }

  std::vector<char> buffer(16 * 1024);
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(h.request, &available)) {
      fail();
      return static_cast<int>(status);
    }
    if (available == 0) break;
    if (available > buffer.size()) buffer.resize(available);
    DWORD read = 0;
    if (!WinHttpReadData(h.request, buffer.data(), available, &read)) {
      fail();
      return static_cast<int>(status);
    }
    if (read == 0) break;
    piece(std::string_view(buffer.data(), read));
  }
  return static_cast<int>(status);
}

void Interrupt(Abort* abort) {
  const std::lock_guard<std::mutex> lock(abort->mutex);
  abort->aborted = true;
  if (abort->request != nullptr) {
    WinHttpCloseHandle(static_cast<HINTERNET>(abort->request));
    abort->request = nullptr;
  }
}

}  // namespace sb::claude

#else

namespace sb::claude {

int Post(const std::string&, const std::string&, const std::function<void(std::string_view)>&,
         Abort*, std::string* error) {
  if (error != nullptr) *error = "Talking to Claude is only built for Windows so far.";
  return 0;
}

void Interrupt(Abort* abort) {
  const std::lock_guard<std::mutex> lock(abort->mutex);
  abort->aborted = true;
}

}  // namespace sb::claude

#endif
