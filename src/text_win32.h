#pragma once

// UTF-8 and UTF-16, where the two meet. The program keeps its text in UTF-8;
// the W functions of the Windows API take UTF-16. Only code that talks to those
// functions includes this.
//
// Neither direction throws: what does not convert cleanly comes out as U+FFFD.
// PathToUtf8 and Utf8ToPath in common.h are the same conversions for paths.

#include <string>

namespace cap {

std::string ToUtf8(const std::wstring& w);
std::wstring ToWide(const std::string& s);

}  // namespace cap
