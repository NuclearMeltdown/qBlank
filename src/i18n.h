#pragma once

// Language switching.
//
// Strings are written inline at the call site as T("deutsch", "english")
// instead of living in a table behind numeric ids. With a UI this size that
// trade is worth it: the two languages cannot drift apart, every string is
// greppable where it is used, and there is no id bookkeeping to get wrong.

#include <string>

namespace cap {

enum class Language { German, English };

void SetLanguage(Language language);
Language CurrentLanguage();

// Picks the matching variant for the language currently set.
const char* T(const char* de, const char* en);

// Same, for places that build a std::string.
inline const char* Tr(const char* de, const char* en) { return T(de, en); }

// The log is English whatever the interface speaks. It ends up pasted into
// GitHub issues, and whoever reads it there has not necessarily seen the
// German interface. While one of these is alive, T() on this thread answers in
// English.
class EnglishScope {
 public:
  EnglishScope();
  ~EnglishScope();
  EnglishScope(const EnglishScope&) = delete;
  EnglishScope& operator=(const EnglishScope&) = delete;
};

// True when T() on this thread answers in English right now -- the setting, or
// an EnglishScope.
bool SpeakingEnglish();

// A message that is both shown and logged: `shown` in the interface language,
// `logged` in English. Built with CAP_SAID.
struct Said {
  std::string shown;
  std::string logged;
};

template <typename Build>
Said Say(Build&& build) {
  Said said;
  said.shown = build();
  if (SpeakingEnglish()) {
    said.logged = said.shown;
  } else {
    EnglishScope english;
    said.logged = build();
  }
  return said;
}

// Evaluates `expr` once for the interface and, under a German interface, once
// more in English for the log. So `expr` must not have side effects -- read
// GetLastError() into a local before, not inside.
#define CAP_SAID(expr) ::cap::Say([&]() -> std::string { return std::string(expr); })

// A message passed on from further down, where it was logged already. Nothing
// to log a second time.
inline Said Relayed(std::string shown) {
  Said said;
  said.shown = std::move(shown);
  return said;
}

}  // namespace cap
