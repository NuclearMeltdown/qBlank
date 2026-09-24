#pragma once

#include <string>
#include <vector>

namespace cap {

// Feste Nummern statt eines mitlaufenden Zaehlers: welcher Reiter zuletzt
// offen war, steht in der Konfiguration, und die Reihenfolge haengt davon ab,
// ob ein Geraet ausgewaehlt ist. Ein Zaehler wuerde dieselbe Zahl je nach Lage
// auf verschiedene Reiter zeigen lassen.
enum SettingsTab {
  kTabSource = 0,
  kTabPicture,
  kTabHdr,
  kTabAudio,
  kTabDisplay,
  kTabRecord,
  kTabEncoder,
  kTabKeys,
  kTabProfiles,
  kTabUpdates,
  kTabGeneral,  // later than the rest, and first on screen
  kTabCount,
};

// Everything but General, Source and Updates is only shown once a video device
// is picked.
inline bool SettingsTabNeedsDevice(int tab) {
  return tab != kTabGeneral && tab != kTabSource && tab != kTabUpdates;
}

struct SettingsSearchHit {
  int entry;
  int score;
  // The query matched the label as it reads on screen. Anything else -- a
  // synonym, the other language, a typo -- is a guess and listed as one.
  bool direct;
};

// Best first. Empty for an empty query.
std::vector<SettingsSearchHit> SearchSettings(const char* query);

const char* SearchLabel(int entry);  // in the current language
std::string SearchPlace(int entry);  // "Tab › Section"
const char* SearchKey(int entry);    // the anchor to jump to; nullptr for a whole tab
int SearchTab(int entry);

}  // namespace cap
