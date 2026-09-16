#include "i18n.h"

#include <atomic>

namespace cap {
namespace {

// Read from the UI thread and written only when the setting changes, so a
// plain atomic is enough.
//
// English to match AppSettings, so that anything translated before the
// configuration is loaded comes out in the same language the program will
// settle on. App::Run calls SetLanguage right after loading, so this value is
// only ever seen by startup errors -- which is exactly when a disagreement
// would be hardest to explain.
std::atomic<Language> g_language{Language::English};

// Per thread, and a count rather than a flag so scopes can nest.
thread_local int t_englishScopes = 0;

}  // namespace

void SetLanguage(Language language) {
  g_language.store(language, std::memory_order_relaxed);
}

Language CurrentLanguage() {
  return g_language.load(std::memory_order_relaxed);
}

EnglishScope::EnglishScope() {
  ++t_englishScopes;
}

EnglishScope::~EnglishScope() {
  --t_englishScopes;
}

bool SpeakingEnglish() {
  return t_englishScopes > 0 || g_language.load(std::memory_order_relaxed) == Language::English;
}

const char* T(const char* de, const char* en) {
  return SpeakingEnglish() ? en : de;
}

}  // namespace cap
