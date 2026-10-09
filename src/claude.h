// Talking to Claude: one request to the Messages API, streamed, on a thread of
// its own.
//
// **A frame never waits on the network.** A `Call` starts its request the
// moment it is made and hands back what has arrived whenever it is asked, so
// the window drawing a conversation polls it once a frame and shows the text
// as it comes. Destroying one stops it -- the request is cancelled and the
// thread joined -- so a conversation thrown away leaves nothing running.
//
// Raylib-free, like everything else that talks to the outside world. The one
// platform-specific piece, the HTTPS itself, is `Post` below, in
// `claude_win32.cpp`.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "json.h"

namespace sb::claude {

// --- models -----------------------------------------------------------------

// What a model costs, in dollars per million tokens. Cached input is what was
// already sent in the request before; a cache write is the first time it is.
struct Price {
  double input = 0.0;
  double cacheWrite = 0.0;
  double cacheRead = 0.0;
  double output = 0.0;
};

struct Model {
  const char* id;
  const char* name;
  const char* blurb;
  Price       price;
  // Whether it takes an effort level at all; one that does not answers a
  // request carrying one with an error.
  bool        effort = true;
  // How many tokens fit in its context: what the context bar is a fraction of.
  long        context = 1000000;
};

// The models offered, most capable first. Kept here rather than asked of the
// API, because the price has to be known to show what a conversation costs.
const std::vector<Model>& Models();
const Model*              FindModel(std::string_view id);
inline constexpr const char* kDefaultModel = "claude-opus-5-5";

// The effort levels, lowest first: how much a reply may spend, thinking
// included. The main control on both depth and cost.
inline constexpr const char* kEfforts[] = {"low", "medium", "high", "xhigh", "max"};
inline constexpr int         kEffortCount = 5;

// --- usage ------------------------------------------------------------------

// What one reply used, as the API reports it.
struct Usage {
  long input = 0;       // read fresh
  long cacheWrite = 0;  // read fresh and stored
  long cacheRead = 0;   // read from the cache
  long output = 0;      // written, thinking included

  long   Context() const { return input + cacheWrite + cacheRead; }
  double Dollars(const Price& price) const {
    return (input * price.input + cacheWrite * price.cacheWrite + cacheRead * price.cacheRead +
            output * price.output) /
           1.0e6;
  }
};

// --- one request ------------------------------------------------------------

class Call {
 public:
  // Starts posting `request` (a Messages API body; `stream` is set here).
  Call(const std::string& key, json::Value request);
  ~Call();
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

  // Reply text that has arrived since the last time this was asked.
  std::string TakeText();
  // Whether the reply is in the middle of thinking rather than writing.
  bool Thinking() const;

  bool Done() const;
  // Once done: whether it failed, and why, in words fit to show.
  bool               Failed() const;
  const std::string& Error() const;
  // Once done: the reply as the API would have returned it whole -- its content
  // blocks, thinking and tool calls included, ready to be sent back -- why it
  // stopped, and what it used.
  const json::Value& Content() const;
  const std::string& StopReason() const;
  Usage              GetUsage() const;

  // Stops it now. Done afterwards, and failed if it had not finished.
  void Cancel();

  struct Shared;

 private:
  std::shared_ptr<Shared> shared_;
  std::thread             thread_;
};

// A picture sent to Claude goes as base64 text.
std::string Base64(const unsigned char* data, std::size_t size);

// --- the platform half ------------------------------------------------------

// Lets another thread stop a request in flight: closing the request's handle
// makes whatever it is blocked on return at once.
struct Abort {
  std::mutex mutex;
  void*      request = nullptr;
  bool       aborted = false;
};

// Posts `body` to the Messages API and hands each piece of the response to
// `piece` as it arrives. The HTTP status, or 0 with `error` set if there never
// was one.
int  Post(const std::string& key, const std::string& body,
          const std::function<void(std::string_view)>& piece, Abort* abort, std::string* error);
void Interrupt(Abort* abort);

}  // namespace sb::claude
