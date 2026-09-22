#pragma once

// A question the program has to ask before it has any settings to draw with.
//
// Its own window, its own device, its own Dear ImGui context, all of it gone
// again the moment the question is answered. That is more machinery than a
// MessageBox, and it buys one thing: the first window somebody sees after a
// rename looks like the program they installed, not like a system error. It can
// also show what the answer depends on -- which files are involved, when each
// was last written -- which a message box cannot.

#include <string>
#include <vector>

namespace cap {

struct StartupQuestion {
  std::string heading;
  std::string body;

  // Shown as a small two-column list between body and buttons. Meant for the
  // facts the answer turns on, not for prose.
  struct Row {
    std::string label;
    std::string detail;
    bool highlight = false;  // draws in the accent colour
  };
  std::vector<Row> rows;

  std::string acceptLabel;
  std::string rejectLabel;
  std::string footnote;
};

enum class StartupAnswer {
  Postpone,  // window closed without answering -- decide nothing
  Accept,
  Reject,
};

// Runs its own message loop until the question is answered. Needs COM only for
// the theme, which is already up by the time this is called.
StartupAnswer AskAtStartup(const StartupQuestion& question);

}  // namespace cap
