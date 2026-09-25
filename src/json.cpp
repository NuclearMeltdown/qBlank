#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cap {
namespace json {
namespace {

const Value& NullValue() {
  static const Value kNull;
  return kNull;
}

// ------------------------------------------------------------------- serialise

// The config is dumped four times a second on the frame thread to see whether
// anything changed, so this is written to be cheap: plain runs are appended in
// one go, and only the characters that need escaping are handled one by one.
void EncodeString(const std::string& s, std::string& out) {
  out += '"';
  const char* run = s.data();
  const char* const end = run + s.size();
  for (const char* p = run; p != end; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c >= 0x20 && c != '"' && c != '\\') continue;  // UTF-8 passes through untouched
    out.append(run, p);
    run = p + 1;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default: {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      }
    }
  }
  out.append(run, end);
  out += '"';
}

void EncodeNumber(double d, std::string& out) {
  if (!std::isfinite(d)) {
    out += '0';
    return;
  }
  // Integers are written without a decimal point so the file stays readable.
  // They are most of the numbers, and to_chars does them without the format
  // parsing and locale lookup snprintf goes through.
  if (d == (double)(long long)d && std::fabs(d) < 1e15) {
    char buf[24];
    out.append(buf, std::to_chars(buf, buf + sizeof(buf), (long long)d).ptr);
  } else {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", d);
    out += buf;
  }
}

// A new line indented to `depth`. Appended in place: building the padding as a
// string for every value cost more than all the rest of the dump.
void NewLine(int indent, int depth, std::string& out) {
  out += '\n';
  out.append((size_t)(indent * depth), ' ');
}

void DumpValue(const Value& v, int indent, int depth, std::string& out) {
  const bool pretty = indent > 0;

  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.AsBool() ? "true" : "false"; break;
    case Value::Type::Number: EncodeNumber(v.AsNumber(), out); break;
    case Value::Type::String: EncodeString(v.str(), out); break;
    case Value::Type::Array: {
      if (v.elements().empty()) {
        out += "[]";
        break;
      }
      out += '[';
      bool first = true;
      for (const Value& e : v.elements()) {
        if (!first) out += ',';
        first = false;
        if (pretty) NewLine(indent, depth + 1, out);
        DumpValue(e, indent, depth + 1, out);
      }
      if (pretty) NewLine(indent, depth, out);
      out += ']';
      break;
    }
    case Value::Type::Object: {
      if (v.items().empty()) {
        out += "{}";
        break;
      }
      out += '{';
      bool first = true;
      for (const auto& kv : v.items()) {
        if (!first) out += ',';
        first = false;
        if (pretty) NewLine(indent, depth + 1, out);
        EncodeString(kv.first, out);
        out += pretty ? ": " : ":";
        DumpValue(kv.second, indent, depth + 1, out);
      }
      if (pretty) NewLine(indent, depth, out);
      out += '}';
      break;
    }
  }
}

// ----------------------------------------------------------------------- parse

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool ParseValue(Value& out) {
    SkipWs();
    if (pos_ >= s_.size()) return Fail("unexpected end of file");
    char c = s_[pos_];
    switch (c) {
      case '{': return ParseObject(out);
      case '[': return ParseArray(out);
      case '"': {
        std::string str;
        if (!ParseString(str)) return false;
        out = Value(std::move(str));
        return true;
      }
      case 't':
        if (s_.compare(pos_, 4, "true") == 0) {
          pos_ += 4;
          out = Value(true);
          return true;
        }
        return Fail("invalid token");
      case 'f':
        if (s_.compare(pos_, 5, "false") == 0) {
          pos_ += 5;
          out = Value(false);
          return true;
        }
        return Fail("invalid token");
      case 'n':
        if (s_.compare(pos_, 4, "null") == 0) {
          pos_ += 4;
          out = Value();
          return true;
        }
        return Fail("invalid token");
      default: return ParseNumber(out);
    }
  }

  const std::string& error() const { return error_; }

 private:
  bool Fail(const char* msg) {
    if (error_.empty()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s at position %zu", msg, pos_);
      error_ = buf;
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
        // Line comments are not standard JSON, but tolerating them means a
        // hand-edited config with a note in it still loads.
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }

  static void AppendUtf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
      out += (char)cp;
    } else if (cp < 0x800) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool ParseHex4(unsigned& out) {
    if (pos_ + 4 > s_.size()) return Fail("truncated Unicode escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') {
        out |= (unsigned)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        out |= (unsigned)(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        out |= (unsigned)(c - 'A' + 10);
      } else {
        return Fail("invalid hex digit");
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("string expected");
    ++pos_;
    out.clear();
    while (true) {
      if (pos_ >= s_.size()) return Fail("unterminated string");
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return Fail("unterminated escape");
      char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!ParseHex4(cp)) return false;
          // Surrogate pair.
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() && s_[pos_] == '\\' &&
              s_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned lo = 0;
            if (!ParseHex4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
          }
          AppendUtf8(cp, out);
          break;
        }
        default: return Fail("unknown escape");
      }
    }
  }

  bool ParseNumber(Value& out) {
    size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    bool any = false;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      bool digit = (c >= '0' && c <= '9');
      bool expSign = (c == '-' || c == '+') && pos_ > start &&
                     (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E');
      if (digit || c == '.' || c == 'e' || c == 'E' || expSign) {
        if (digit) any = true;
        ++pos_;
      } else {
        break;
      }
    }
    if (!any) return Fail("number expected");
    out = Value(std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr));
    return true;
  }

  bool ParseArray(Value& out) {
    ++pos_;  // consume '['
    out = Value::Array();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Value elem;
      if (!ParseValue(elem)) return false;
      out.Push(std::move(elem));
      SkipWs();
      if (pos_ >= s_.size()) return Fail("unterminated array");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("comma or ] expected");
    }
  }

  bool ParseObject(Value& out) {
    ++pos_;  // consume '{'
    out = Value::Object();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      std::string key;
      if (!ParseString(key)) return false;
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("colon expected");
      ++pos_;
      Value val;
      if (!ParseValue(val)) return false;
      out[key] = std::move(val);
      SkipWs();
      if (pos_ >= s_.size()) return Fail("unterminated object");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("comma or } expected");
    }
  }

  const std::string& s_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

// ------------------------------------------------------------------ Value impl

Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

void Value::Become(Type type) {
  type_ = type;
  str_.clear();
  array_.clear();
  object_.clear();
}

Value& Value::operator=(bool b) {
  Become(Type::Bool);
  bool_ = b;
  return *this;
}

Value& Value::operator=(int i) { return *this = (double)i; }

Value& Value::operator=(double d) {
  Become(Type::Number);
  num_ = d;
  return *this;
}

Value& Value::operator=(const char* s) { return *this = std::string(s ? s : ""); }

Value& Value::operator=(const std::string& s) {
  Become(Type::String);
  str_ = s;
  return *this;
}

bool Value::AsBool(bool def) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return num_ != 0.0;
  return def;
}

double Value::AsNumber(double def) const {
  if (type_ == Type::Number) return num_;
  if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
  return def;
}

int Value::AsInt(int def) const {
  if (type_ == Type::Number) return (int)std::llround(num_);
  if (type_ == Type::Bool) return bool_ ? 1 : 0;
  return def;
}

std::string Value::AsString(std::string_view def) const {
  if (type_ == Type::String) return str_;
  return std::string(def);
}

const Value& Value::operator[](std::string_view key) const {
  for (const auto& kv : object_) {
    if (kv.first == key) return kv.second;
  }
  return NullValue();
}

Value& Value::operator[](std::string_view key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& kv : object_) {
    if (kv.first == key) return kv.second;
  }
  object_.emplace_back(std::string(key), Value());
  return object_.back().second;
}

bool Value::Has(std::string_view key) const {
  for (const auto& kv : object_) {
    if (kv.first == key) return true;
  }
  return false;
}

void Value::Push(Value v) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(v));
}

size_t Value::Size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

const Value& Value::At(size_t i) const {
  if (type_ == Type::Array && i < array_.size()) return array_[i];
  return NullValue();
}

// -------------------------------------------------------------- free functions

Value Parse(const std::string& text, std::string* error) {
  Parser p(text);
  Value v;
  if (!p.ParseValue(v)) {
    if (error) *error = p.error();
    return Value();
  }
  if (error) error->clear();
  return v;
}

std::string Dump(const Value& v, int indent) {
  std::string out;
  // The config comes to about 7 KB; room for it to grow without a copy.
  out.reserve(16384);
  DumpValue(v, indent, 0, out);
  out += '\n';
  return out;
}

}  // namespace json
}  // namespace cap
