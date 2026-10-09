// One conversation with Claude: what has been said, the reply in flight, and
// the tools that reply is waiting on.
//
// Raylib-free, so what a conversation *is* -- its messages, the requests they
// make, the loop of asking for tools and answering, what it has cost -- is apart
// from how a window draws one (`chat.*`) and from what a tool does to the
// canvas (`app_tools.cpp`).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "claude.h"
#include "json.h"

namespace sb::claude {

// What is shown: your words, and Claude's -- one turn for everything said in
// reply to one message, however many rounds of tools it took. What Claude *did*
// is not shown here; the canvas is where that is seen.
struct Turn {
  enum class Role { User, Assistant };
  Role        role = Role::User;
  std::string text;
  // A reply that was stopped or went wrong, said in `note`.
  bool        failed = false;
  std::string note;
  // Your turns only: where it starts in the messages sent, and the canvas as it
  // was when it was said -- which is what rewinding to it can put back.
  int         message = -1;
  std::string canvas;
};

// The canvas as a message carries it: one description a box, and a line or two
// on where Hamish is -- his caret, what is selected, the window under his
// pointer. Made by the app, which knows the canvas; what of it is *sent* is
// decided here, by what Claude has already seen.
struct CanvasView {
  std::string                              header;   // the algebra
  std::vector<std::pair<int, std::string>> boxes;    // id, and the box described
  std::string                              status;
};

// A tool a reply is waiting on.
struct ToolCall {
  std::string id;
  std::string name;
  json::Value input;
};

class Conversation {
 public:
  std::string model = kDefaultModel;
  std::string effort = "medium";

  const std::vector<Turn>& Turns() const { return turns_; }
  // A reply is coming, or waiting on its tools.
  bool Busy() const { return call_ != nullptr || !waiting_.empty(); }
  bool Thinking() const { return call_ != nullptr && call_->Thinking(); }
  // Its tools are being carried out.
  bool Working() const { return call_ == nullptr && !waiting_.empty(); }

  // Says something and asks for the reply. The canvas goes with the words
  // without being shown -- **the whole of it the first time, and after that only
  // what has changed since Claude last saw it**, since every copy sent stays in
  // the conversation and is paid for again with every request. False, with
  // nothing sent, while a reply is still coming.
  bool Send(const std::string& key, const std::string& text, const CanvasView& view,
            const std::string& system, const json::Value& tools, const std::string& canvas);
  // Takes whatever has arrived. Once a frame; true if anything changed.
  bool Poll();
  // The tool calls the reply is waiting on, each handed out once.
  std::vector<ToolCall> TakeToolCalls();
  // What one of them came to. Once every call of a round is answered, the
  // conversation carries on by itself.
  void Answer(const std::string& id, json::Value content, bool isError);
  // Stops the reply, whether it is arriving or waiting on tools.
  void Stop();

  // How full the context was at the last reply, and what the whole
  // conversation has cost.
  long   ContextTokens() const { return context_; }
  double Dollars() const { return dollars_; }

  // Another model, from the next message on. Thinking is the model's own and is
  // not handed to a different one, so earlier thinking is dropped from what is
  // sent. Only between replies.
  void SetModel(const std::string& id);

  // What Claude has seen of the canvas other than with a message: a box a tool
  // reported on, or all of it, read whole.
  void Saw(int box, const std::string& description) { seen_[box] = description; }
  void SawAll(const CanvasView& view);

  // Back to just before your turn `turn`: it and everything after it go. Hands
  // back its words, to be said again or changed, and the canvas as it was then.
  bool Rewind(std::size_t turn, std::string* words, std::string* canvas);

  // As it is kept in a canvas file, and back. Pictures are left out of what is
  // kept -- they are large, and can be looked at again -- and a conversation
  // saved in the middle of using its tools is closed off so it can carry on.
  json::Value Save() const;
  void        Load(const json::Value& saved);

  // A reply may go round this many times before it is stopped and asked to
  // pause, so a runaway loop cannot run up a bill.
  static constexpr int kMaxRounds = 40;

 private:
  json::Value Request() const;
  void        Start();
  void        Finish(const std::string& note);

  std::vector<Turn>     turns_;
  // What is sent: the API's own messages, tool calls and results included.
  json::Value           messages_ = json::Value::Array();
  std::string           key_;
  std::string           system_;
  json::Value           tools_;
  std::unique_ptr<Call> call_;
  std::vector<ToolCall> handOut_;
  std::vector<std::string>                          waiting_;
  std::vector<std::pair<std::string, json::Value>>  answered_;
  int    rounds_ = 0;
  bool   joinNext_ = false;
  // Each box as Claude last saw it, and whether it has seen the canvas at all
  // in what is still in the conversation.
  std::map<int, std::string> seen_;
  bool                       sawCanvas_ = false;
  std::string                Context(const CanvasView& view);
  long   context_ = 0;
  double dollars_ = 0.0;
};

}  // namespace sb::claude
