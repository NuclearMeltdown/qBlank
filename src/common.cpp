#include "common.h"

namespace cap {

// ---------------------------------------------------------------- string utils

std::string ToUpper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  }
  return s;
}

std::string Trim(const std::string& s) {
  const char* kWhitespace = " \t\r\n";
  size_t b = s.find_first_not_of(kWhitespace);
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(kWhitespace);
  return s.substr(b, e - b + 1);
}

std::string Format(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n);
    std::vsnprintf(out.data(), (size_t)n + 1, fmt, args);
  }
  va_end(args);
  return out;
}

// ---------------------------------------------------------------------- logging

bool ReportError(std::string* error, const Said& said) {
  LogWrite("ERR ", "%s", said.logged.c_str());
  if (error) *error = said.shown;
  return false;
}

// ------------------------------------------------------------------ misc utils

std::string FormatBytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = (double)bytes;
  int unit = 0;
  while (value >= 1024.0 && unit + 1 < (int)(sizeof(kUnits) / sizeof(kUnits[0]))) {
    value /= 1024.0;
    ++unit;
  }
  if (unit == 0) return Format("%llu B", (unsigned long long)bytes);
  // Eine Nachkommastelle nur, solange sie etwas aussagt: bei 412 GB ist die
  // dritte Stelle Rauschen, bei 3,7 TB ist sie die Antwort.
  return Format(value < 10.0 ? "%.1f %s" : "%.0f %s", value, kUnits[unit]);
}

std::string FormatDuration(double seconds) {
  if (!(seconds > 0.0)) return "0 s";
  if (seconds < 60.0) return Format("%.0f s", seconds);
  const long long total = (long long)(seconds + 0.5);
  const long long hours = total / 3600;
  const long long minutes = (total % 3600) / 60;
  if (hours <= 0) return Format("%lld min", minutes);
  return Format("%lld h %lld min", hours, minutes);
}

}  // namespace cap
