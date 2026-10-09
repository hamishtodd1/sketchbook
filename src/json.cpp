#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sb::json {
namespace {

const Value&       Null() {
  static const Value null;
  return null;
}
const std::string& Empty() {
  static const std::string empty;
  return empty;
}

void PutUtf8(unsigned int cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  bool Document(Value* out) {
    Space();
    if (!Any(out, 0)) return false;
    Space();
    if (at_ != text_.size()) return Fail("text after the value");
    return true;
  }
  std::string error;

 private:
  // Deep enough for anything an API sends, shallow enough that nothing hostile
  // can blow the stack.
  static constexpr int kDepth = 200;

  bool Fail(const char* why) {
    error = std::string(why) + " at byte " + std::to_string(at_);
    return false;
  }
  void Space() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
      ++at_;
    }
  }
  bool Word(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }

  bool Any(Value* out, int depth) {
    if (depth > kDepth) return Fail("nested too deeply");
    if (at_ >= text_.size()) return Fail("unexpected end");
    const char c = text_[at_];
    if (c == '{') return ObjectOf(out, depth);
    if (c == '[') return ArrayOf(out, depth);
    if (c == '"') {
      std::string s;
      if (!StringOf(&s)) return false;
      *out = Value(std::move(s));
      return true;
    }
    if (Word("true")) { *out = Value(true); return true; }
    if (Word("false")) { *out = Value(false); return true; }
    if (Word("null")) { *out = Value(); return true; }
    return NumberOf(out);
  }

  bool NumberOf(Value* out) {
    const std::size_t from = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) ++at_;
    while (at_ < text_.size()) {
      const char c = text_[at_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        ++at_;
      } else {
        break;
      }
    }
    if (at_ == from) return Fail("expected a value");
    const std::string digits(text_.substr(from, at_ - from));
    char*             end = nullptr;
    const double      n = std::strtod(digits.c_str(), &end);
    if (end == nullptr || *end != '\0') return Fail("bad number");
    *out = Value(n);
    return true;
  }

  bool Hex4(unsigned int* out) {
    if (at_ + 4 > text_.size()) return Fail("short \\u escape");
    unsigned int v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned int>(c - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    *out = v;
    return true;
  }

  bool StringOf(std::string* out) {
    ++at_;  // the opening quote
    while (true) {
      if (at_ >= text_.size()) return Fail("unterminated string");
      const char c = text_[at_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (at_ >= text_.size()) return Fail("unterminated escape");
      const char e = text_[at_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned int cp = 0;
          if (!Hex4(&cp)) return false;
          // A character outside the first plane arrives as two halves.
          if (cp >= 0xD800 && cp <= 0xDBFF && text_.substr(at_, 2) == "\\u") {
            at_ += 2;
            unsigned int low = 0;
            if (!Hex4(&low)) return false;
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          PutUtf8(cp, out);
          break;
        }
        default: return Fail("bad escape");
      }
    }
  }

  bool ArrayOf(Value* out, int depth) {
    ++at_;
    *out = Value::Array();
    Space();
    if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
    while (true) {
      Value item;
      Space();
      if (!Any(&item, depth + 1)) return false;
      out->Push(std::move(item));
      Space();
      if (at_ >= text_.size()) return Fail("unterminated array");
      if (text_[at_] == ',') { ++at_; continue; }
      if (text_[at_] == ']') { ++at_; return true; }
      return Fail("expected , or ]");
    }
  }

  bool ObjectOf(Value* out, int depth) {
    ++at_;
    *out = Value::Object();
    Space();
    if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
    while (true) {
      Space();
      if (at_ >= text_.size() || text_[at_] != '"') return Fail("expected a member name");
      std::string key;
      if (!StringOf(&key)) return false;
      Space();
      if (at_ >= text_.size() || text_[at_] != ':') return Fail("expected :");
      ++at_;
      Space();
      Value value;
      if (!Any(&value, depth + 1)) return false;
      out->Set(std::move(key), std::move(value));
      Space();
      if (at_ >= text_.size()) return Fail("unterminated object");
      if (text_[at_] == ',') { ++at_; continue; }
      if (text_[at_] == '}') { ++at_; return true; }
      return Fail("expected , or }");
    }
  }

  std::string_view text_;
  std::size_t      at_ = 0;
};

void Quote(const std::string& s, std::string* out) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(c));
          *out += hex;
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void WriteTo(const Value& v, std::string* out) {
  switch (v.type()) {
    case Value::Type::Null: *out += "null"; break;
    case Value::Type::Bool: *out += v.Bool() ? "true" : "false"; break;
    case Value::Type::Number: {
      const double n = v.Number();
      // Whole numbers as whole numbers: a token count or an index is read by
      // something that may not accept "4096.000000".
      if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15) {
        *out += std::to_string(static_cast<long long>(n));
      } else if (std::isfinite(n)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.17g", n);
        *out += buffer;
      } else {
        *out += "null";
      }
      break;
    }
    case Value::Type::String: Quote(v.String(), out); break;
    case Value::Type::Array: {
      out->push_back('[');
      bool first = true;
      for (const Value& item : v.Items()) {
        if (!first) out->push_back(',');
        first = false;
        WriteTo(item, out);
      }
      out->push_back(']');
      break;
    }
    case Value::Type::Object: {
      out->push_back('{');
      bool first = true;
      for (const auto& [key, member] : v.Members()) {
        if (!first) out->push_back(',');
        first = false;
        Quote(key, out);
        out->push_back(':');
        WriteTo(member, out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace

const std::string& Value::String() const { return IsString() ? string_ : Empty(); }

const Value& Value::operator[](std::string_view key) const {
  if (!IsObject()) return Null();
  for (const auto& [name, value] : members_) {
    if (name == key) return value;
  }
  return Null();
}

Value& Value::Set(std::string key, Value value) {
  if (!IsObject()) *this = Object();
  for (auto& [name, existing] : members_) {
    if (name == key) {
      existing = std::move(value);
      return existing;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
  return members_.back().second;
}

Value& Value::Push(Value value) {
  if (!IsArray()) *this = Array();
  items_.push_back(std::move(value));
  return items_.back();
}

Value* Value::Find(std::string_view key) {
  if (!IsObject()) return nullptr;
  for (auto& [name, value] : members_) {
    if (name == key) return &value;
  }
  return nullptr;
}

void Value::Erase(std::string_view key) {
  for (auto it = members_.begin(); it != members_.end(); ++it) {
    if (it->first == key) {
      members_.erase(it);
      return;
    }
  }
}

bool Parse(std::string_view text, Value* out, std::string* error) {
  Reader reader(text);
  Value  value;
  if (!reader.Document(&value)) {
    if (error != nullptr) *error = reader.error;
    return false;
  }
  *out = std::move(value);
  return true;
}

std::string Write(const Value& value) {
  std::string out;
  WriteTo(value, &out);
  return out;
}

}  // namespace sb::json
