#pragma once

// Whether a card asked for single fields (720x288 at 50 Hz, 720x240 at 59.94
// Hz) sends every field, top and bottom in turn, or the same lines each time.
// Platform-neutral so the logic can be checked without a card; qblank_probe
// "fields" feeds it.
//
// It is a question of geometry, answered over the parts of the picture that
// stood still (sample n against n+2, n+1 against n+3) and have vertical
// detail:
// - Same lines: on the same rows n+1 differs from n by the noise alone, one
//   row up or down by the detail. That is a card sending the same field every
//   time, or scaling a frame down to half height.
// - Otherwise the two are fields of an interlaced frame, half a field line
//   apart, and a row of n+1 lies between two rows of n: below them for the
//   bottom field after the top, above for the top after the bottom. Each tile
//   weaves the two both ways and votes for the one that combs less, the same
//   test a deinterlacer uses for field order. +3 and -3 half lines are tried
//   too, for a driver whose window for one field starts a line off.
// Alternating fields vote +1 and -1 half lines in turn.
//
// Why not simply try every offset, whole lines included: a sharp horizontal
// edge sampled every other line fits a whole-line shift exactly as often as
// the true half line, so a menu of hard edges would vote for both. Weaving
// both ways cannot be fooled that way: at an edge the wrong order zigzags or,
// depending on where the edge falls, combs no worse, but never better.
//
// A still picture with vertical detail (a menu, text, a test screen) gives the
// clearest answer. A source whose two fields carry the same picture (240p or
// 288p from a game console, a line-doubled picture) cannot be told from the
// same lines, and needs not be: it looks the same either way.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cap {

// Offsets reported, in half field lines either way: 0, +-1 and +-3.
inline constexpr int kFieldOffsetRange = 3;
inline constexpr int kFieldOffsets = 2 * kFieldOffsetRange + 1;

struct FieldPairResult {
  bool identical = false;  // bit for bit the same as the sample before
  int tiles = 0;           // tiles looked at
  int stillTiles = 0;      // of those, still (n to n+2, n+1 to n+3) and with vertical detail
  // n+1 against n on the same rows, over those, against the nearer of one row
  // up or down: well below 1 on the same lines, about 1 half a line apart.
  float lineRatio = 0.0f;
  int votes[kFieldOffsets] = {};
  int totalVotes = 0;
  bool decided = false;
  int offset = 0;  // in half field lines, positive = lower; valid when decided
};

// `a` to `d` are four consecutive samples, luma only, `width` bytes per row,
// rows top first. Returns where `b` sits against `a`; `c` and `d` only tell
// which parts of the picture stood still.
inline FieldPairResult CompareFieldPair(const uint8_t* a, const uint8_t* b, const uint8_t* c,
                                        const uint8_t* d, int width, int height) {
  FieldPairResult out;
  const size_t bytes = (size_t)width * (size_t)height;
  out.identical = std::memcmp(a, b, bytes) == 0;
  if (out.identical) return out;

  // 32 samples by 8 rows: small enough that a still menu next to a moving
  // picture still has whole tiles to itself, large enough to average the noise
  // of 256 samples. The border is left out, since the blanking there is black
  // and static and would out-vote a picture.
  const int kTileW = 32;
  const int kTileH = 8;
  const int kReach = 3;
  const int x0 = width / 20;
  const int x1 = width - x0;
  const int y0 = std::max(height / 20, kReach);
  const int y1 = std::min(height - height / 20, height - kReach);
  const int kOdd[4] = {-3, -1, 1, 3};

  struct Tile {
    float stillA;  // mean |a - c|
    float stillB;  // mean |b - d|
    float same;    // mean |a - b|, the same row
    float nearer;  // mean |a - b| one row up or down, whichever changes less
    float detail;  // mean |a(row) - a(row + 1)|
    float comb[4];
  };
  std::vector<Tile> tiles;
  for (int ty = y0; ty + kTileH <= y1; ty += kTileH) {
    for (int tx = x0; tx + kTileW <= x1; tx += kTileW) {
      int stillA = 0;
      int stillB = 0;
      int same = 0;
      int up = 0;
      int down = 0;
      int detail = 0;
      int comb[4] = {};
      for (int x = tx; x < tx + kTileW; ++x) {
        // The column woven into one sequence of 16 rows for each offset.
        // +1 and -1 take the same rows of both samples, only in the other
        // order, so an edge where the tile starts or ends favours neither;
        // on the same lines both sequences are the same and the tile
        // abstains.
        int w[4][2 * kTileH];
        for (int i = 0; i < kTileH; ++i) {
          const size_t o = (size_t)(ty + i) * width + x;
          const int av = a[o];
          const int bv = b[o];
          stillA += std::abs(av - (int)c[o]);
          stillB += std::abs(bv - (int)d[o]);
          same += std::abs(av - bv);
          up += std::abs((int)a[o - width] - bv);
          down += std::abs((int)a[o + width] - bv);
          detail += std::abs(av - (int)a[o + width]);
          w[0][2 * i] = bv;  // -3: row i of b between rows i - 2 and i - 1 of a
          w[0][2 * i + 1] = a[o - width];
          w[1][2 * i] = bv;  // -1
          w[1][2 * i + 1] = av;
          w[2][2 * i] = av;  // +1
          w[2][2 * i + 1] = bv;
          w[3][2 * i] = a[o + width];  // +3
          w[3][2 * i + 1] = bv;
        }
        for (int i = 0; i < 4; ++i)
          for (int k = 1; k + 1 < 2 * kTileH; ++k)
            comb[i] += std::abs(2 * w[i][k] - w[i][k - 1] - w[i][k + 1]);
      }
      const float n = (float)(kTileW * kTileH);
      Tile t;
      t.stillA = stillA / n;
      t.stillB = stillB / n;
      t.same = same / n;
      t.nearer = std::min(up, down) / n;
      t.detail = detail / n;
      for (int i = 0; i < 4; ++i) t.comb[i] = comb[i] / (float)(kTileW * (2 * kTileH - 2));
      tiles.push_back(t);
    }
  }
  out.tiles = (int)tiles.size();
  if (tiles.empty()) return out;

  // Still means within the noise. A still tile changes by the noise alone,
  // flat or detailed, so a low percentile of the change is the noise as long
  // as a third of the picture stands or is flat. The cap keeps a picture that
  // moves everywhere from calling itself still.
  std::vector<float> changes;
  changes.reserve(tiles.size());
  for (const Tile& t : tiles) changes.push_back(t.stillA);
  const size_t low = changes.size() * 3 / 10;
  std::nth_element(changes.begin(), changes.begin() + low, changes.end());
  const float noise = changes[low];
  const float stillLimit = std::min(8.0f, 1.5f * noise + 0.5f);
  // Vertical detail well above what the noise alone makes.
  const float detailLimit = 2.0f * noise + 2.0f;

  double sumSame = 0.0;
  double sumNearer = 0.0;
  for (const Tile& t : tiles) {
    if (t.stillA > stillLimit || t.stillB > stillLimit || t.detail < detailLimit) continue;
    ++out.stillTiles;
    sumSame += t.same;
    sumNearer += t.nearer;
    int best = 0;
    for (int i = 1; i < 4; ++i)
      if (t.comb[i] < t.comb[best]) best = i;
    float second = 1e9f;
    for (int i = 0; i < 4; ++i)
      if (i != best) second = std::min(second, t.comb[i]);
    // A clear winner only: where an edge falls so that both orders comb
    // alike, the tile abstains.
    if (t.comb[best] < 0.75f * second) {
      ++out.votes[kOdd[best] + kFieldOffsetRange];
      ++out.totalVotes;
    }
  }
  if (out.stillTiles < 12) return out;
  out.lineRatio = (float)(sumSame / std::max(sumNearer, 1e-6));

  int top = 0;
  for (int i = 1; i < kFieldOffsets; ++i)
    if (out.votes[i] > out.votes[top]) top = i;
  if (out.totalVotes >= 12 && out.votes[top] * 10 >= out.totalVotes * 7) {
    out.decided = true;
    out.offset = top - kFieldOffsetRange;
    return out;
  }

  // Same lines: on the same rows n+1 differs from n by the noise alone, a row
  // up or down by the detail. Half a line apart, a horizontal edge lies
  // either between row y of n and row y of n+1 or between that and the next
  // row of n, and over many edges the two changes come out alike, so the
  // ratio stays near 1. The reference is n+1 itself, not the change to n+2:
  // where a field went missing, n+2 lies on the other lines than n, and a
  // ratio against it would call half a line "same". The weave abstains here,
  // since on the same lines both orders comb alike.
  if (out.lineRatio < 0.5f) {
    out.decided = true;
    out.offset = 0;
  }
  return out;
}

enum class FieldVerdict {
  TooLittle,        // not enough still picture with vertical detail
  Alternating,      // half a line down and up in turn: every field, top and bottom
  AlternatingGaps,  // the same, but now and then a field twice or missing
  SameLines,        // no offset: the same field each time, or a scaled frame
  Mixed,
};

struct FieldSummary {
  int pairs = 0;      // pairs of consecutive samples compared
  int identical = 0;  // of those, repeats bit for bit
  int decided = 0;
  int perOffset[kFieldOffsets] = {};
  // Pairs half a line apart, each against the last such pair before it: the
  // offset turned round as often as strictly alternating fields would turn it
  // over that many samples (in step), or not (out of step). A field missing
  // or twice puts the rest one out of step once.
  int inStep = 0;
  int outOfStep = 0;
  FieldVerdict verdict = FieldVerdict::TooLittle;
};

// Walks a run of consecutive samples and sums up the pair results.
class FieldParityMeter {
 public:
  // Starts over; the next sample does not follow the last one.
  void Break() {
    have_ = 0;
    haveOdd_ = false;
  }

  void Add(const uint8_t* luma, int width, int height) {
    const size_t bytes = (size_t)width * (size_t)height;
    if (width != width_ || height != height_) {
      Break();
      width_ = width;
      height_ = height;
      for (auto& h : hist_) h.assign(bytes, 0);
    }
    // hist_[0] is the oldest of the four; repeat_[k] says hist_[k] is
    // hist_[k - 1] again, bit for bit.
    std::rotate(hist_, hist_ + 1, hist_ + 4);
    std::rotate(repeat_, repeat_ + 1, repeat_ + 4);
    std::memcpy(hist_[3].data(), luma, bytes);
    repeat_[3] = have_ > 0 && std::memcmp(hist_[2].data(), luma, bytes) == 0;
    ++index_;
    if (have_ < 4) ++have_;
    if (have_ < 4) return;

    ++sum_.pairs;
    if (repeat_[1]) {
      ++sum_.identical;
      return;
    }
    // A repeat among the two after the pair puts one of them on the other
    // lines than the sample it is held against, so nothing there is known to
    // be still.
    if (repeat_[2] || repeat_[3]) return;
    const FieldPairResult r = CompareFieldPair(hist_[0].data(), hist_[1].data(), hist_[2].data(),
                                               hist_[3].data(), width, height);
    if (!r.decided) return;
    ++sum_.decided;
    ++sum_.perOffset[r.offset + kFieldOffsetRange];
    if (!(r.offset & 1)) return;
    if (haveOdd_) {
      const bool turned = ((index_ - lastOddIndex_) & 1) != 0;
      if (r.offset == (turned ? -lastOdd_ : lastOdd_)) {
        ++sum_.inStep;
      } else {
        ++sum_.outOfStep;
      }
    }
    haveOdd_ = true;
    lastOdd_ = r.offset;
    lastOddIndex_ = index_;
  }

  FieldSummary Summary() const {
    FieldSummary s = sum_;
    const int same = s.perOffset[kFieldOffsetRange];
    const int turns = s.inStep + s.outOfStep;
    if (s.decided < 10) {
      s.verdict = FieldVerdict::TooLittle;
    } else if (same * 10 >= s.decided * 8) {
      s.verdict = FieldVerdict::SameLines;
    } else if (turns >= 5 && s.outOfStep * 50 <= turns && (same + s.identical) * 50 <= s.pairs) {
      // One in fifty passes as a wrong call; more is a field that came twice
      // or not at all.
      s.verdict = FieldVerdict::Alternating;
    } else if (turns >= 5 && s.inStep * 4 >= turns * 3) {
      s.verdict = FieldVerdict::AlternatingGaps;
    } else {
      s.verdict = FieldVerdict::Mixed;
    }
    return s;
  }

 private:
  std::vector<uint8_t> hist_[4];
  bool repeat_[4] = {};
  int have_ = 0;
  int width_ = 0;
  int height_ = 0;
  long long index_ = 0;
  bool haveOdd_ = false;
  int lastOdd_ = 0;
  long long lastOddIndex_ = 0;
  FieldSummary sum_;
};

}  // namespace cap
