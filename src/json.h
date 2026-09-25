#pragma once

// Minimal JSON reader/writer. The config file is the only JSON this program
// touches, so a tiny self-contained implementation beats pulling in a library.
// Object keys keep insertion order, which makes the saved file readable.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cap {
namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(double d) : type_(Type::Number), num_(d) {}
  Value(int i) : type_(Type::Number), num_((double)i) {}
  Value(float f) : type_(Type::Number), num_((double)f) {}
  Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
  Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

  // Out of line on purpose. The config writes and reads a few hundred values
  // one line each, and a copy of these at every one of them was most of the
  // code config.cpp compiled to. Keys are views for the same reason: a
  // literal no longer becomes a std::string at the call.
  Value(const Value&);
  Value(Value&&) noexcept;
  Value& operator=(const Value&);
  Value& operator=(Value&&) noexcept;
  ~Value();

  // Assigning a plain value builds no temporary Value to move from. The
  // const char* one is not optional: without it a literal would pick bool.
  Value& operator=(bool b);
  Value& operator=(int i);
  Value& operator=(double d);
  Value& operator=(const char* s);
  Value& operator=(const std::string& s);

  static Value Array() {
    Value v;
    v.type_ = Type::Array;
    return v;
  }
  static Value Object() {
    Value v;
    v.type_ = Type::Object;
    return v;
  }

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  bool AsBool(bool def = false) const;
  double AsNumber(double def = 0.0) const;
  int AsInt(int def = 0) const;
  std::string AsString(std::string_view def = {}) const;

  // Object access. The const form returns a shared null value for missing keys,
  // so config reads can chain without existence checks.
  const Value& operator[](std::string_view key) const;
  Value& operator[](std::string_view key);
  bool Has(std::string_view key) const;

  // Array access.
  void Push(Value v);
  size_t Size() const;
  const Value& At(size_t i) const;

  const std::vector<std::pair<std::string, Value>>& items() const { return object_; }
  const std::vector<Value>& elements() const { return array_; }

 private:
  void Become(Type type);

  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

// Returns a Null value and fills `error` when the text is malformed.
Value Parse(const std::string& text, std::string* error = nullptr);

std::string Dump(const Value& v, int indent = 2);

}  // namespace json
}  // namespace cap
