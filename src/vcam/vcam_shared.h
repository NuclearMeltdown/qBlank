#pragma once

// What qBlank and the virtual camera's DirectShow filter agree on.
//
// The two do not run in the same process, but from Windows' point of view they
// are the same user in the same session: DirectShow instantiates the filter
// inside whichever application opened the camera -- OBS, Discord, a browser --
// and that application is the one the user started. This is the single biggest
// difference from the Media Foundation source that used to sit here, which
// Windows loaded into the Frame Server service in session 0. Everything under
// Local\ follows from it: no SeCreateGlobalPrivilege, no service to restart,
// and qBlank creates the shared objects itself instead of waiting for a
// service to do it first.
//
// It also means there is one filter instance per consumer, each negotiating its
// own format. So the consumer table below is not a nicety -- it is the shape of
// the thing.

#include <stdint.h>

#include "app_identity.h"

namespace cap {
namespace vcam {

// ------------------------------------------------------------------ identity

// The filter's COM class. Deliberately not the CLSID the Media Foundation
// source used: an installation left over from CapView 2.x must not be mistaken
// for this one, and a fresh class means the two can even sit side by side while
// the old one is being removed.
// {A326E6EC-3F70-468B-A826-4F9D42CB5C8E}
inline constexpr wchar_t kFilterClsidString[] = L"{A326E6EC-3F70-468B-A826-4F9D42CB5C8E}";

// The CLSID of the Media Foundation source from CapView 2.x. Kept only so the
// installer can recognise and remove it.
inline constexpr wchar_t kLegacySourceClsidString[] = L"{A1E4F2C7-6B3D-4A58-9E21-7C0D5B8F3A46}";

// What the camera is called in every application's device list. Follows the
// program's name: the CLSID is what identifies the filter, and an installation
// left over from an older name is removed rather than kept, so there is nothing
// for the old wording to stay consistent with.
inline constexpr wchar_t kFilterName[] = CAP_APP_NAME L" Virtual Camera";

// -------------------------------------------------------------- object names

// Local\ rather than Global\: both halves are the same user in the same session
// now, so the session namespace is enough and needs no privilege to create in.
// The cost is that a consumer running inside an app container cannot see these,
// which rules out Store apps. That is a deliberate trade -- nothing anyone
// wants to feed a capture card into is a Store app, and the alternative is
// asking every user for administrator rights on every start.
inline constexpr wchar_t kControlSectionName[] = L"Local\\CapViewVCamControl";

// The pictures live in a section of their own, named after a generation number,
// because their size follows the source and the source changes. Growing a
// mapped section in place is not possible, so a format change makes a new one
// and bumps the generation; consumers notice and remap. Sizing one section for
// the largest picture the camera will ever carry is the alternative, and at 8K
// that is three hundred megabytes reserved to show a SNES.
inline constexpr wchar_t kFrameSectionPrefix[] = L"Local\\CapViewVCamFrames-";

// One auto-reset event per consumer slot, so a published picture wakes every
// reader exactly once. A single shared event cannot do that: whoever waited
// first would eat it.
inline constexpr wchar_t kWakeEventPrefix[] = L"Local\\CapViewVCamWake-";

// ---------------------------------------------------------------- versioning

inline constexpr uint32_t kMagic = 0x34565643u;  // 'CVV4'

// Raise this whenever anything below changes shape. The executable and the
// filter are installed separately -- the update check replaces only the
// executable, and Windows keeps the DLL locked while a camera is in use -- so
// the two genuinely can end up from different builds. Undetected that produces
// a black picture and no explanation, which is the worst kind of failure;
// detected, it is one sentence telling the user to install the camera again.
inline constexpr uint32_t kVersion = 4u;

// ------------------------------------------------------------------- picture

// The layouts a slot can hold. NV12 is the ordinary eight bit picture every
// consumer understands. P010 is ten bit on the PQ curve with BT.2020 primaries,
// published only when the source is HDR and the setting asks for it, because
// almost nothing on the other end knows what to do with an HDR webcam yet.
enum : uint32_t { kPixelNv12 = 0u, kPixelP010 = 1u };

// Bytes one picture occupies, given its luma stride. Both layouts are planar
// with half-height chroma, so both are stride * height * 3 / 2.
inline uint64_t PictureBytes(uint32_t strideY, uint32_t height) {
  return (uint64_t)strideY * height * 3ull / 2ull;
}

// Bytes per luma sample: one for NV12, two for P010.
inline uint32_t BytesPerSample(uint32_t pixel) { return pixel == kPixelP010 ? 2u : 1u; }

// Three slots is enough that a reader never waits on the writer: one being
// written, one being read, one spare.
inline constexpr uint32_t kSlotCount = 3u;

// Where the pixels start in the frame section. The header is nowhere near this
// big; the gap is there so every slot begins on a page boundary and a copy
// never straddles one needlessly.
inline constexpr uint32_t kFrameDataOffset = 4096u;

// One published picture. `sequence` is a seqlock: odd while the slot is being
// written, even when it holds something whole. A reader that sees the same even
// value before and after copying knows the copy is not torn.
struct alignas(64) SlotHeader {
  volatile uint32_t sequence;
  uint32_t width;
  uint32_t height;
  uint32_t strideY;
  uint32_t pixel;
  uint32_t frameIndex;  // running count, so a reader can tell new from repeat
  int64_t timestamp100ns;
};

// The head of one frame section. Everything here is fixed for the lifetime of
// that section: when the source changes shape, a new section is made rather
// than this being rewritten, which is what lets a reader hold a mapping without
// ever locking against the writer.
struct FrameSectionHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t generation;
  uint32_t slotCount;
  uint32_t width;
  uint32_t height;
  uint32_t strideY;
  uint32_t pixel;
  uint64_t slotBytes;  // distance from one slot's pixels to the next

  SlotHeader slots[kSlotCount];
};

inline uint64_t FrameSectionBytes(uint64_t slotBytes) {
  return kFrameDataOffset + kSlotCount * slotBytes;
}

inline uint8_t* SlotData(void* base, uint64_t slotBytes, uint32_t slot) {
  return static_cast<uint8_t*>(base) + kFrameDataOffset + slot * slotBytes;
}

// ----------------------------------------------------------- consumer table

// Eight at once is more than anyone will reach -- OBS, Discord and a browser is
// three -- but the table is cheap and a full one only means the ninth consumer
// goes unnamed, not that it goes unserved.
inline constexpr uint32_t kConsumerSlots = 8u;

// How long a consumer's heartbeat may go quiet before qBlank stops listing it.
// Generous on purpose: a filter that has been instantiated but not yet started
// only touches this when its format is negotiated, and a consumer sitting in a
// preview dialog can be slow about that.
inline constexpr uint32_t kConsumerStaleMs = 3000u;

// One application reading the camera, as seen from the filter inside it.
struct ConsumerSlot {
  volatile uint32_t inUse;  // claimed by a filter instance
  uint32_t pid;
  wchar_t name[64];  // the consuming executable's file name, for the UI

  // What this consumer negotiated. Every instance answers for itself; that two
  // of them disagree is normal and is the reason this is a table.
  volatile uint32_t width;
  volatile uint32_t height;
  volatile uint32_t pixel;
  volatile int64_t frameInterval100ns;  // 0 until a format is settled

  volatile uint32_t streaming;     // running, as opposed to merely connected
  volatile uint32_t framesServed;  // samples handed to this consumer
  volatile uint32_t framesFresh;   // of those, ones carrying a new picture
  volatile uint32_t lastSeenMs;    // GetTickCount at the last sign of life
};

// ------------------------------------------------------------- control block

struct ControlBlock {
  uint32_t magic;
  uint32_t version;
  uint32_t stateBytes;  // sizeof(ControlBlock) as the creator understood it

  // qBlank to the filters: what the source is right now. A filter reads these
  // to decide what to advertise, and re-reads them when the generation moves.
  volatile uint32_t sourceWidth;
  volatile uint32_t sourceHeight;
  volatile int64_t sourceInterval100ns;  // 10,000,000 / fps, DirectShow's unit

  // What the frame section currently holds. This is also the answer to whether
  // the camera offers its ten bit form, because there is nothing to offer it
  // from otherwise -- one field rather than two that can disagree.
  volatile uint32_t sourcePixel;

  volatile uint32_t producerAlive;
  volatile uint32_t frameGeneration;  // names the current frame section
  volatile uint32_t framesPublished;

  ConsumerSlot consumers[kConsumerSlots];
};

// ------------------------------------------------------------------- helpers

// Both names carry a number, so both are built the same way. The buffer is the
// caller's because this header is included by a DLL that would rather not drag
// in std::wstring.
inline void FormatName(wchar_t* out, size_t count, const wchar_t* prefix, uint32_t number) {
  size_t i = 0;
  for (; prefix[i] && i + 1 < count; ++i) out[i] = prefix[i];

  wchar_t digits[16];
  int n = 0;
  if (number == 0) {
    digits[n++] = L'0';
  } else {
    while (number > 0 && n < 15) {
      digits[n++] = (wchar_t)(L'0' + (number % 10));
      number /= 10;
    }
  }
  while (n > 0 && i + 1 < count) out[i++] = digits[--n];
  out[i] = 0;
}

}  // namespace vcam
}  // namespace cap
