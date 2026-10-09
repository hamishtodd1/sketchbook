#include "conversation.h"

#include <algorithm>
#include <utility>

namespace sb::claude {
namespace {

// Room for thinking as well as the reply: at the higher efforts a model may
// think for a long time, and `max_tokens` is a hard limit on both together.
int MaxTokensFor(const std::string& effort) {
  if (effort == "xhigh" || effort == "max") return 64000;
  return 32000;
}

json::Value Text(const std::string& text) {
  json::Value block = json::Value::Object();
  block.Set("type", "text");
  block.Set("text", text);
  return block;
}

}  // namespace

json::Value Conversation::Request() const {
  json::Value request = json::Value::Object();
  request.Set("model", model);
  request.Set("max_tokens", MaxTokensFor(effort));
  if (!system_.empty()) request.Set("system", system_);
  const Model* known = FindModel(model);
  if (known == nullptr || known->effort) {
    json::Value config = json::Value::Object();
    config.Set("effort", effort);
    request.Set("output_config", std::move(config));
  }
  if (tools_.IsArray() && tools_.Size() > 0) request.Set("tools", tools_);
  // Automatic prompt caching: everything sent before is read back at a fraction
  // of the price, which is most of every request after the first.
  json::Value cache = json::Value::Object();
  cache.Set("type", "ephemeral");
  request.Set("cache_control", std::move(cache));
  // Two user messages in a row -- a question after a reply that failed -- are
  // run together by the API into one turn, which is what they are.
  request.Set("messages", messages_);
  return request;
}

void Conversation::Start() {
  call_ = std::make_unique<Call>(key_, Request());
}

void Conversation::SawAll(const CanvasView& view) {
  seen_.clear();
  for (const auto& [id, description] : view.boxes) seen_[id] = description;
  sawCanvas_ = true;
}

std::string Conversation::Context(const CanvasView& view) {
  std::string out;
  if (!sawCanvas_) {
    out = "The canvas as it is now. " + view.header + "\n";
    for (const auto& [id, description] : view.boxes) out += "\n" + description;
    if (view.boxes.empty()) out += "\nThere are no boxes on it.\n";
  } else {
    // Only what is new or different, and what has gone.
    std::string changed;
    std::string same;
    for (const auto& [id, description] : view.boxes) {
      const auto found = seen_.find(id);
      if (found != seen_.end() && found->second == description) {
        same += (same.empty() ? "b" : ", b") + std::to_string(id);
      } else {
        changed += "\n" + description;
      }
    }
    std::string gone;
    for (const auto& [id, description] : seen_) {
      const bool still = std::any_of(view.boxes.begin(), view.boxes.end(),
                                     [id = id](const auto& box) { return box.first == id; });
      if (!still) gone += (gone.empty() ? "b" : ", b") + std::to_string(id);
    }
    if (changed.empty() && gone.empty()) {
      out = "The canvas has not changed since you last saw it.\n";
    } else {
      out = "What has changed on the canvas since you last saw it.\n";
      if (!changed.empty()) out += "New or changed:\n" + changed;
      if (!gone.empty()) out += "\nDeleted: " + gone + ".\n";
      if (!same.empty()) out += "\nUnchanged: " + same + ".\n";
    }
  }
  if (!view.status.empty()) out += "\n" + view.status;
  SawAll(view);
  return out;
}

bool Conversation::Send(const std::string& key, const std::string& text,
                        const CanvasView& view, const std::string& system,
                        const json::Value& tools, const std::string& canvas) {
  if (Busy() || text.empty() || key.empty()) return false;
  key_ = key;
  system_ = system;
  tools_ = tools;
  rounds_ = 0;
  joinNext_ = false;

  Turn said;
  said.role = Turn::Role::User;
  said.text = text;
  said.message = static_cast<int>(messages_.Size());
  said.canvas = canvas;
  turns_.push_back(std::move(said));
  Turn reply;
  reply.role = Turn::Role::Assistant;
  turns_.push_back(std::move(reply));

  json::Value content = json::Value::Array();
  content.Push(Text(Context(view)));
  content.Push(Text(text));
  json::Value message = json::Value::Object();
  message.Set("role", "user");
  message.Set("content", std::move(content));
  messages_.Push(std::move(message));
  Start();
  return true;
}

void Conversation::Finish(const std::string& note) {
  if (!turns_.empty() && !note.empty()) turns_.back().note = note;
}

bool Conversation::Poll() {
  if (call_ == nullptr) return false;
  bool        changed = false;
  Turn&       reply = turns_.back();
  std::string more = call_->TakeText();
  if (!more.empty()) {
    // Words from a later round of the same reply start a new paragraph.
    if (joinNext_ && !reply.text.empty()) reply.text += "\n\n";
    joinNext_ = false;
    reply.text += more;
    changed = true;
  }
  if (!call_->Done()) return changed;

  more = call_->TakeText();
  if (!more.empty()) {
    if (joinNext_ && !reply.text.empty()) reply.text += "\n\n";
    joinNext_ = false;
    reply.text += more;
  }
  const Usage used = call_->GetUsage();
  context_ = used.Context() + used.output;
  if (const Model* known = FindModel(model)) dollars_ += used.Dollars(known->price);

  if (call_->Failed()) {
    // Half an answer is not sent back: the conversation carries on from the
    // last thing that finished.
    reply.failed = reply.text.empty();
    Finish(call_->Error());
    call_.reset();
    return true;
  }

  json::Value message = json::Value::Object();
  message.Set("role", "assistant");
  message.Set("content", call_->Content());
  messages_.Push(std::move(message));
  const std::string stop = call_->StopReason();
  if (stop == "pause_turn") {
    // A search on the server side that wants another turn to finish: carry on.
    joinNext_ = true;
    Start();
    return true;
  }
  if (stop == "tool_use") {
    for (const json::Value& block : call_->Content().Items()) {
      if (block["type"].String() != "tool_use") continue;
      handOut_.push_back({block["id"].String(), block["name"].String(), block["input"]});
      waiting_.push_back(block["id"].String());
    }
  } else if (stop == "max_tokens") {
    Finish("The reply ran out of room and stops here.");
  }
  call_.reset();
  return true;
}

std::vector<ToolCall> Conversation::TakeToolCalls() {
  std::vector<ToolCall> out = std::move(handOut_);
  handOut_.clear();
  return out;
}

void Conversation::Answer(const std::string& id, json::Value content, bool isError) {
  if (std::find(waiting_.begin(), waiting_.end(), id) == waiting_.end()) return;
  for (const auto& [done, result] : answered_) {
    if (done == id) return;
  }
  json::Value result = json::Value::Object();
  result.Set("type", "tool_result");
  result.Set("tool_use_id", id);
  result.Set("content", std::move(content));
  if (isError) result.Set("is_error", true);
  answered_.emplace_back(id, std::move(result));
  if (answered_.size() < waiting_.size()) return;

  // Every call of the round answered, in the order they were asked.
  json::Value results = json::Value::Array();
  for (const std::string& asked : waiting_) {
    for (auto& [done, block] : answered_) {
      if (done == asked) results.Push(std::move(block));
    }
  }
  waiting_.clear();
  answered_.clear();
  json::Value message = json::Value::Object();
  message.Set("role", "user");
  message.Set("content", std::move(results));
  messages_.Push(std::move(message));

  if (++rounds_ >= kMaxRounds) {
    Finish("Stopped after " + std::to_string(kMaxRounds) +
           " steps. Say \"go on\" to let it carry on.");
    return;
  }
  joinNext_ = true;
  Start();
}

void Conversation::SetModel(const std::string& id) {
  if (Busy() || id == model) return;
  model = id;
  for (json::Value& message : messages_.Items()) {
    if (message["role"].String() != "assistant") continue;
    json::Value* content = message.Find("content");
    if (content == nullptr || !content->IsArray()) continue;
    std::vector<json::Value>& blocks = content->Items();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [](const json::Value& block) {
                                  const std::string& type = block["type"].String();
                                  return type == "thinking" || type == "redacted_thinking";
                                }),
                 blocks.end());
    // A reply that was nothing but thinking would now be empty, which the API
    // will not take.
    if (blocks.empty()) {
      json::Value said = json::Value::Object();
      said.Set("type", "text");
      said.Set("text", "(thought, and said nothing)");
      blocks.push_back(std::move(said));
    }
  }
}

bool Conversation::Rewind(std::size_t turn, std::string* words, std::string* canvas) {
  if (turn >= turns_.size() || turns_[turn].role != Turn::Role::User) return false;
  Stop();
  call_.reset();
  waiting_.clear();
  answered_.clear();
  handOut_.clear();
  const Turn& said = turns_[turn];
  if (words != nullptr) *words = said.text;
  if (canvas != nullptr) *canvas = said.canvas;
  if (said.message >= 0 && said.message <= static_cast<int>(messages_.Size())) {
    messages_.Items().resize(static_cast<std::size_t>(said.message));
  }
  turns_.resize(turn);
  // How full the context is was measured with what has just gone; the next
  // reply measures it again. What it cost stays: that was spent.
  context_ = 0;
  // What Claude saw of the canvas may have gone with it, so the next message
  // carries all of it again.
  seen_.clear();
  sawCanvas_ = false;
  return true;
}

namespace {

// Pictures are left out of what is kept: a look is large, and can be taken
// again. What stands in its place says so.
void DropPictures(json::Value* value) {
  if (value->IsArray()) {
    for (json::Value& item : value->Items()) {
      if (item["type"].String() == "image") {
        json::Value said = json::Value::Object();
        said.Set("type", "text");
        said.Set("text", "(A picture was here; look again to see it.)");
        item = std::move(said);
      } else {
        DropPictures(&item);
      }
    }
  } else if (value->IsObject()) {
    if (json::Value* content = value->Find("content")) DropPictures(content);
  }
}

}  // namespace

json::Value Conversation::Save() const {
  json::Value saved = json::Value::Object();
  saved.Set("model", model);
  saved.Set("effort", effort);
  saved.Set("context", context_);
  saved.Set("dollars", dollars_);
  json::Value turns = json::Value::Array();
  for (const Turn& turn : turns_) {
    json::Value t = json::Value::Object();
    t.Set("role", turn.role == Turn::Role::User ? "user" : "assistant");
    t.Set("text", turn.text);
    if (turn.failed) t.Set("failed", true);
    if (!turn.note.empty()) t.Set("note", turn.note);
    if (turn.message >= 0) t.Set("message", turn.message);
    if (!turn.canvas.empty()) t.Set("canvas", turn.canvas);
    turns.Push(std::move(t));
  }
  saved.Set("turns", std::move(turns));
  json::Value messages = messages_;
  DropPictures(&messages);
  saved.Set("messages", std::move(messages));
  return saved;
}

void Conversation::Load(const json::Value& saved) {
  if (!saved["model"].String().empty()) model = saved["model"].String();
  if (!saved["effort"].String().empty()) effort = saved["effort"].String();
  context_ = saved["context"].Integer();
  dollars_ = saved["dollars"].Number();
  turns_.clear();
  for (const json::Value& t : saved["turns"].Items()) {
    Turn turn;
    turn.role = t["role"].String() == "user" ? Turn::Role::User : Turn::Role::Assistant;
    turn.text = t["text"].String();
    turn.failed = t["failed"].Bool();
    turn.note = t["note"].String();
    turn.message = static_cast<int>(t["message"].Integer(-1));
    turn.canvas = t["canvas"].String();
    turns_.push_back(std::move(turn));
  }
  messages_ = saved["messages"].IsArray() ? saved["messages"] : json::Value::Array();
  // Saved while its tools were being carried out: those calls are answered as
  // interrupted, or the API would not take the conversation back.
  if (messages_.Size() > 0) {
    const json::Value& last = messages_.Items().back();
    if (last["role"].String() == "assistant") {
      json::Value results = json::Value::Array();
      for (const json::Value& block : last["content"].Items()) {
        if (block["type"].String() != "tool_use") continue;
        json::Value result = json::Value::Object();
        result.Set("type", "tool_result");
        result.Set("tool_use_id", block["id"].String());
        result.Set("content", "Interrupted: the canvas was closed before this ran.");
        result.Set("is_error", true);
        results.Push(std::move(result));
      }
      if (results.Size() > 0) {
        json::Value message = json::Value::Object();
        message.Set("role", "user");
        message.Set("content", std::move(results));
        messages_.Push(std::move(message));
      }
    }
  }
}

void Conversation::Stop() {
  if (call_ != nullptr) {
    call_->Cancel();
    return;
  }
  if (waiting_.empty()) return;
  // Every call still owed an answer gets one, so the conversation stays one the
  // API will take -- but nothing carries on.
  json::Value results = json::Value::Array();
  for (const std::string& asked : waiting_) {
    json::Value block;
    for (auto& [done, answered] : answered_) {
      if (done == asked) block = std::move(answered);
    }
    if (block.IsNull()) {
      block = json::Value::Object();
      block.Set("type", "tool_result");
      block.Set("tool_use_id", asked);
      block.Set("content", "Stopped by Hamish before this ran.");
      block.Set("is_error", true);
    }
    results.Push(std::move(block));
  }
  waiting_.clear();
  answered_.clear();
  handOut_.clear();
  json::Value message = json::Value::Object();
  message.Set("role", "user");
  message.Set("content", std::move(results));
  messages_.Push(std::move(message));
  Finish("Stopped.");
}

}  // namespace sb::claude
