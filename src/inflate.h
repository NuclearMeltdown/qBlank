#pragma once

// Data the executable carries deflated, and the one thing done with it: unpack
// it. The shaders are the bulk of what the executable carries besides code --
// the SPIR-V for Vulkan alone is a quarter of a megabyte -- and they shrink to
// under a third, so the build packs them (tools/embed_packed.cmake) and the
// renderer that is actually used unpacks its own, once, when it starts.
//
// Only the format and not a library: deflate (RFC 1951) is small enough to read
// in a few hundred lines, and the only input is what the build wrote.

#include <cstddef>
#include <cstdint>

namespace cap {

// Raw deflate data and the number of bytes it unpacks to.
struct Packed {
  const uint8_t* data;
  size_t size;
  size_t unpackedSize;
};

// Unpacks `packed` into `out`, which holds exactly `packed.unpackedSize` bytes.
// False when the data is broken or does not come to exactly that size.
bool Inflate(const Packed& packed, uint8_t* out);

}  // namespace cap
