#pragma once

// Which audio tracks a recording gets: the card's sound and every recording
// source (RecordSource), each with its TrackMode.
//
// A mix is only worth a track when at least two sounds go into it. Without one
// a source asked to be mixed only gets a track of its own instead, and the
// card's sound keeps its own track unless everything else is in the mix alone.
// With a single source this is what the microphone always did: Both is three
// tracks, Mixed one, Separate two.
//
// Counted rather than listed, so the estimate on the recording tab can ask
// every frame without allocating.

#include "config.h"

namespace cap {

class TrackPlan {
 public:
  explicit TrackPlan(bool haveMain) : haveMain_(haveMain), members_(haveMain ? 1 : 0) {}

  // One call per source, in order, before any of the questions below.
  void Add(TrackMode mode) {
    ++sources_;
    if (mode != TrackMode::Separate) ++members_;
    if (mode != TrackMode::Mixed) ++kept_;
  }

  bool mix() const { return members_ >= 2; }
  // The card's sound on a track of its own.
  bool main() const { return haveMain_ && (!mix() || kept_ > 0); }
  // A source of this mode on a track of its own, or in the mix.
  bool own(TrackMode mode) const { return !mix() || mode != TrackMode::Mixed; }
  bool mixed(TrackMode mode) const { return mix() && mode != TrackMode::Separate; }

  int count() const { return (mix() ? 1 + kept_ : sources_) + (main() ? 1 : 0); }

 private:
  bool haveMain_;
  int members_;     // sounds in the mix
  int sources_ = 0;
  int kept_ = 0;    // sources that want a track of their own
};

}  // namespace cap
