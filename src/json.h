// JSON, as much as talking to the Claude API needs: read a reply, write a
// request. Raylib-free, like the rest of the layer that talks to the outside
// world.
//
// An object keeps its members in the order they were written, since a request
// is easier to read in the order it was built and nothing here is large enough
// for lookup by name to want a map.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sb::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(int n) : type_(Type::Number), number_(n) {}
  Value(long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
  Value(long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
  Value(double n) : type_(Type::Number), number_(n) {}
  Value(const char* s) : type_(Type::String), string_(s) {}
  Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
  Value(std::string_view s) : type_(Type::String), string_(s) {}

  static Value Array() { Value v; v.type_ = Type::Array; return v; }
  static Value Object() { Value v; v.type_ = Type::Object; return v; }

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsString() const { return type_ == Type::String; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  // Read as a kind, or the given fallback if it is not that kind.
  const std::string& String() const;
  double Number(double fallback = 0.0) const { return IsNumber() ? number_ : fallback; }
  long   Integer(long fallback = 0) const {
    return IsNumber() ? static_cast<long>(number_) : fallback;
  }
  bool   Bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }

  // A member, or null if there is none -- so a missing field reads as nothing
  // rather than as a crash, which is what a reply from a server needs.
  const Value& operator[](std::string_view key) const;
  // The array's elements, or the object's members.
  const std::vector<Value>& Items() const { return items_; }
  std::vector<Value>&       Items() { return items_; }
  const std::vector<std::pair<std::string, Value>>& Members() const { return members_; }
  std::size_t Size() const { return IsArray() ? items_.size() : members_.size(); }

  // Building. Set replaces a member of the same name rather than adding a
  // second one.
  Value& Set(std::string key, Value value);
  Value& Push(Value value);
  // A member to write into, made if it is not there.
  Value* Find(std::string_view key);
  void   Erase(std::string_view key);

  // Strings are appended to rather than copied, since a streamed reply grows
  // one small piece at a time.
  void Append(std::string_view more) { string_ += more; }

 private:
  Type                                       type_ = Type::Null;
  bool                                       bool_ = false;
  double                                     number_ = 0.0;
  std::string                                string_;
  std::vector<Value>                         items_;
  std::vector<std::pair<std::string, Value>> members_;
};

// False, with where and why, if the text is not one JSON value.
bool Parse(std::string_view text, Value* out, std::string* error = nullptr);
// Compact: a request is read by a server, not a person.
std::string Write(const Value& value);

}  // namespace sb::json
