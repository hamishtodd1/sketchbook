#include "claude.h"

#include <utility>

namespace sb::claude {

// ---------------------------------------------------------------- models

const std::vector<Model>& Models() {
  // Dollars per million tokens: input, 5-minute cache write, cache read, output.
  static const std::vector<Model> models = {
      {"claude-fable-5-1", "Fable 5.1", "Most capable, for the hardest problems",
       {10.0, 12.5, 0.25, 50.0}},
      {"claude-opus-5-5", "Opus 5.5", "Deep reasoning and code, at a fraction of Fable",
       {4.0, 5.0, 0.20, 20.0}},
      {"claude-sonnet-5-5", "Sonnet 5.5", "Fast and capable, for everyday work",
       {2.0, 2.5, 0.20, 10.0}},
      {"claude-sonnet-5", "Sonnet 5", "The previous Sonnet", {2.0, 2.5, 0.20, 10.0}},
      {"claude-haiku-4-5-20251001", "Haiku 4.5", "Fastest and cheapest, for quick answers",
       {1.0, 1.25, 0.10, 5.0}, false, 200000},
  };
  return models;
}

const Model* FindModel(std::string_view id) {
  for (const Model& model : Models()) {
    if (id == model.id) return &model;
  }
  return nullptr;
}

std::string Base64(const unsigned char* data, std::size_t size) {
  static const char kDigits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    const unsigned int a = data[i];
    const unsigned int b = i + 1 < size ? data[i + 1] : 0u;
    const unsigned int c = i + 2 < size ? data[i + 2] : 0u;
    const unsigned int n = (a << 16) | (b << 8) | c;
    out.push_back(kDigits[(n >> 18) & 63]);
    out.push_back(kDigits[(n >> 12) & 63]);
    out.push_back(i + 1 < size ? kDigits[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? kDigits[n & 63] : '=');
  }
  return out;
}

// ---------------------------------------------------------------- one call

struct Call::Shared {
  std::mutex  mutex;
  std::string fresh;      // reply text not yet taken
  bool        thinking = false;
  bool        done = false;
  bool        failed = false;
  std::string error;
  json::Value content = json::Value::Array();
  std::string stopReason;
  Usage       usage;
  Abort       abort;
};

namespace {

// Reads the event stream a reply arrives as -- `event:` and `data:` lines, a
// blank line after each event -- and builds the reply out of it, block by
// block, exactly as the API would have returned it whole.
class Assembler {
 public:
  explicit Assembler(Call::Shared* shared) : shared_(shared) {}

  void Feed(std::string_view piece) {
    if (raw_.size() < kRawKept) raw_.append(piece.substr(0, kRawKept - raw_.size()));
    buffer_.append(piece);
    // An event ends at a blank line, whichever line ending the server uses.
    while (true) {
      std::size_t end = buffer_.find("\n\n");
      std::size_t skip = 2;
      const std::size_t crlf = buffer_.find("\r\n\r\n");
      if (crlf != std::string::npos && (end == std::string::npos || crlf < end)) {
        end = crlf;
        skip = 4;
      }
      if (end == std::string::npos) break;
      Event(std::string_view(buffer_).substr(0, end));
      buffer_.erase(0, end + skip);
    }
  }

  bool               finished = false;
  const std::string& Raw() const { return raw_; }

 private:
  // Enough of the start of a reply to say what went wrong when it is an error
  // rather than a stream.
  static constexpr std::size_t kRawKept = 64 * 1024;

  void Event(std::string_view event) {
    std::string data;
    std::size_t at = 0;
    while (at <= event.size()) {
      std::size_t eol = event.find('\n', at);
      if (eol == std::string_view::npos) eol = event.size();
      std::string_view line = event.substr(at, eol - at);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.substr(0, 5) == "data:") {
        line.remove_prefix(5);
        if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (!data.empty()) data.push_back('\n');
        data.append(line);
      }
      at = eol + 1;
    }
    if (data.empty()) return;
    json::Value value;
    if (!json::Parse(data, &value)) return;
    Handle(value);
  }

  void ReadUsage(const json::Value& usage) {
    Usage& u = shared_->usage;
    if (usage["input_tokens"].IsNumber()) u.input = usage["input_tokens"].Integer();
    if (usage["cache_creation_input_tokens"].IsNumber()) {
      u.cacheWrite = usage["cache_creation_input_tokens"].Integer();
    }
    if (usage["cache_read_input_tokens"].IsNumber()) {
      u.cacheRead = usage["cache_read_input_tokens"].Integer();
    }
    if (usage["output_tokens"].IsNumber()) u.output = usage["output_tokens"].Integer();
  }

  json::Value* Block(long index) {
    std::vector<json::Value>& blocks = shared_->content.Items();
    if (index < 0 || index >= static_cast<long>(blocks.size())) return nullptr;
    return &blocks[static_cast<std::size_t>(index)];
  }

  void Handle(const json::Value& event) {
    const std::string&           type = event["type"].String();
    const std::lock_guard<std::mutex> lock(shared_->mutex);

    if (type == "message_start") {
      ReadUsage(event["message"]["usage"]);
    } else if (type == "content_block_start") {
      const long index = event["index"].Integer(-1);
      if (index < 0) return;
      std::vector<json::Value>& blocks = shared_->content.Items();
      if (static_cast<long>(blocks.size()) <= index) blocks.resize(static_cast<std::size_t>(index) + 1);
      blocks[static_cast<std::size_t>(index)] = event["content_block"];
      if (static_cast<long>(partial_.size()) <= index) partial_.resize(static_cast<std::size_t>(index) + 1);
      partial_[static_cast<std::size_t>(index)].clear();
      const std::string& kind = event["content_block"]["type"].String();
      shared_->thinking = (kind == "thinking" || kind == "redacted_thinking");
    } else if (type == "content_block_delta") {
      const long   index = event["index"].Integer(-1);
      json::Value* block = Block(index);
      if (block == nullptr) return;
      const json::Value& delta = event["delta"];
      const std::string& kind = delta["type"].String();
      if (kind == "text_delta") {
        const std::string& text = delta["text"].String();
        if (json::Value* into = block->Find("text")) into->Append(text);
        else block->Set("text", text);
        shared_->fresh += text;
      } else if (kind == "thinking_delta") {
        if (json::Value* into = block->Find("thinking")) into->Append(delta["thinking"].String());
        else block->Set("thinking", delta["thinking"].String());
      } else if (kind == "signature_delta") {
        block->Set("signature", delta["signature"].String());
      } else if (kind == "input_json_delta") {
        partial_[static_cast<std::size_t>(index)] += delta["partial_json"].String();
      } else if (kind == "citations_delta") {
        json::Value* citations = block->Find("citations");
        if (citations == nullptr || !citations->IsArray()) {
          citations = &block->Set("citations", json::Value::Array());
        }
        citations->Push(delta["citation"]);
      }
    } else if (type == "content_block_stop") {
      const long   index = event["index"].Integer(-1);
      json::Value* block = Block(index);
      if (block == nullptr) return;
      // A tool call's arguments arrive as pieces of JSON text, and are only
      // JSON once they have all arrived.
      const std::string& pieces = partial_[static_cast<std::size_t>(index)];
      if (!pieces.empty()) {
        json::Value input;
        if (json::Parse(pieces, &input)) block->Set("input", std::move(input));
      }
      shared_->thinking = false;
    } else if (type == "message_delta") {
      if (event["delta"]["stop_reason"].IsString()) {
        shared_->stopReason = event["delta"]["stop_reason"].String();
      }
      ReadUsage(event["usage"]);
    } else if (type == "message_stop") {
      finished = true;
    } else if (type == "error") {
      shared_->failed = true;
      shared_->error = ErrorWords(event["error"]);
    }
  }

  Call::Shared*            shared_;
  std::string              buffer_;
  std::string              raw_;
  std::vector<std::string> partial_;

 public:
  static std::string ErrorWords(const json::Value& error) {
    const std::string& kind = error["type"].String();
    const std::string& message = error["message"].String();
    if (kind == "authentication_error") return "The API key was not accepted.";
    if (kind == "overloaded_error") return "Claude is overloaded just now -- try again shortly.";
    if (kind == "rate_limit_error") return "Rate limited: " + message;
    if (!message.empty()) return message;
    return kind.empty() ? std::string("Something went wrong.") : kind;
  }
};

void Run(std::shared_ptr<Call::Shared> shared, std::string key, std::string body) {
  Assembler   assembler(shared.get());
  std::string error;
  const int   status = Post(key, body, [&](std::string_view piece) { assembler.Feed(piece); },
                            &shared->abort, &error);

  const std::lock_guard<std::mutex> lock(shared->mutex);
  bool aborted = false;
  {
    const std::lock_guard<std::mutex> abortLock(shared->abort.mutex);
    aborted = shared->abort.aborted;
  }
  if (!shared->failed) {
    if (aborted) {
      shared->failed = true;
      shared->error = "Stopped.";
    } else if (status == 0) {
      shared->failed = true;
      shared->error = error.empty() ? "Could not reach the API." : error;
    } else if (status != 200) {
      // Not a stream at all: the error comes back as one JSON object.
      json::Value reply;
      shared->failed = true;
      shared->error = json::Parse(assembler.Raw(), &reply)
                          ? Assembler::ErrorWords(reply["error"])
                          : "The API answered " + std::to_string(status) + ".";
    } else if (!assembler.finished) {
      shared->failed = true;
      shared->error = "The reply stopped before it was finished.";
    }
  }
  shared->thinking = false;
  shared->done = true;
}

}  // namespace

Call::Call(const std::string& key, json::Value request) : shared_(std::make_shared<Shared>()) {
  request.Set("stream", true);
  thread_ = std::thread(Run, shared_, key, json::Write(request));
}

Call::~Call() {
  Cancel();
  if (thread_.joinable()) thread_.join();
}

void Call::Cancel() {
  {
    const std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->done) return;
  }
  Interrupt(&shared_->abort);
}

std::string Call::TakeText() {
  const std::lock_guard<std::mutex> lock(shared_->mutex);
  std::string out = std::move(shared_->fresh);
  shared_->fresh.clear();
  return out;
}

bool Call::Thinking() const {
  const std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->thinking;
}

bool Call::Done() const {
  const std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->done;
}

// Read only once Done, after which the thread writes nothing more.
bool               Call::Failed() const { return shared_->failed; }
const std::string& Call::Error() const { return shared_->error; }
const json::Value& Call::Content() const { return shared_->content; }
const std::string& Call::StopReason() const { return shared_->stopReason; }

Usage Call::GetUsage() const {
  const std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->usage;
}

}  // namespace sb::claude
