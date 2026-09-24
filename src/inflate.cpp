#include "inflate.h"

#include <cstring>

namespace cap {
namespace {

constexpr int kMaxBits = 15;  // the longest code deflate has
// Codes up to this long are looked up in one step, the rest walked bit by bit.
// Nearly every code in practice is this short.
constexpr int kFastBits = 10;

// The stream is read least significant bit first. Past the end come zeros, and
// they are counted: whether the stream ran off the end is asked once a block,
// rather than on every read.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : next_(data), end_(data + size) {}

  uint32_t Peek(int n) {
    if (count_ < n) Refill();
    return (uint32_t)(bits_ & ((1ull << n) - 1));
  }
  void Drop(int n) {
    bits_ >>= n;
    count_ -= n;
  }
  uint32_t Take(int n) {
    const uint32_t value = Peek(n);
    Drop(n);
    return value;
  }
  void AlignToByte() { Drop(count_ & 7); }
  // False once a bit from beyond the end has been used.
  bool Within() const { return past_ * 8 <= (size_t)count_; }

 private:
  void Refill() {
    while (count_ <= 56) {
      uint64_t byte = 0;
      if (next_ < end_) {
        byte = *next_++;
      } else {
        ++past_;
      }
      bits_ |= byte << count_;
      count_ += 8;
    }
  }

  const uint8_t* next_;
  const uint8_t* end_;
  uint64_t bits_ = 0;
  int count_ = 0;
  size_t past_ = 0;
};

// A canonical Huffman code, as deflate describes it: only the length of each
// symbol's code, the codes themselves following from the lengths.
struct Huffman {
  uint16_t count[kMaxBits + 1];  // how many codes of each length
  uint16_t symbol[288];          // the symbols in code order
  // The next kFastBits of the stream to (length << 9 | symbol), for the codes
  // no longer than that; 0 where the code is longer or unused.
  uint16_t fast[1 << kFastBits];
};

// False when the lengths ask for more codes than there are bit patterns. Fewer
// is allowed -- deflate uses that for a single distance code -- and decoding a
// pattern no symbol has fails there instead.
bool Build(Huffman* h, const uint8_t* lengths, int n) {
  std::memset(h->count, 0, sizeof(h->count));
  for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left = (left << 1) - h->count[len];
    if (left < 0) return false;
  }
  uint16_t offset[kMaxBits + 2];
  offset[1] = 0;
  for (int len = 1; len <= kMaxBits; ++len) offset[len + 1] = offset[len] + h->count[len];
  for (int i = 0; i < n; ++i) {
    if (lengths[i]) h->symbol[offset[lengths[i]]++] = (uint16_t)i;
  }

  // The codes are sent most significant bit first, into a stream read least
  // significant first -- so a table indexed by the stream holds them reversed.
  std::memset(h->fast, 0, sizeof(h->fast));
  int code = 0;
  int index = 0;
  for (int len = 1; len <= kFastBits; ++len) {
    for (int k = 0; k < h->count[len]; ++k, ++code, ++index) {
      int reversed = 0;
      for (int b = 0; b < len; ++b) reversed |= ((code >> b) & 1) << (len - 1 - b);
      const uint16_t entry = (uint16_t)(len << 9 | h->symbol[index]);
      for (int f = reversed; f < (1 << kFastBits); f += 1 << len) h->fast[f] = entry;
    }
    code <<= 1;
  }
  return true;
}

// The next symbol, or -1 when the bits are no code.
int Decode(BitReader& in, const Huffman& h) {
  const uint32_t next = in.Peek(kMaxBits);
  const uint16_t entry = h.fast[next & ((1u << kFastBits) - 1)];
  if (entry) {
    in.Drop(entry >> 9);
    return entry & 511;
  }
  // Canonical codes of one length are consecutive numbers, so a code is found
  // by counting through the lengths.
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    code |= (next >> (len - 1)) & 1;
    const int count = h.count[len];
    if (code - count < first) {
      in.Drop(len);
      return h.symbol[index + (code - first)];
    }
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Output {
  uint8_t* data;
  size_t size;
  size_t pos;
};

// One compressed block's literals and back references, up to its end code.
bool Codes(BitReader& in, const Huffman& lengths, const Huffman& distances, Output& out) {
  for (;;) {
    int symbol = Decode(in, lengths);
    if (symbol < 0) return false;
    if (symbol < 256) {
      if (out.pos == out.size) return false;
      out.data[out.pos++] = (uint8_t)symbol;
      continue;
    }
    if (symbol == 256) return true;
    symbol -= 257;
    if (symbol >= 29) return false;
    const size_t length = kLengthBase[symbol] + in.Take(kLengthExtra[symbol]);
    symbol = Decode(in, distances);
    if (symbol < 0 || symbol >= 30) return false;
    const size_t distance = kDistanceBase[symbol] + in.Take(kDistanceExtra[symbol]);
    if (distance > out.pos || length > out.size - out.pos) return false;
    // Byte by byte: a reference may overlap what it is writing.
    uint8_t* to = out.data + out.pos;
    const uint8_t* from = to - distance;
    for (size_t i = 0; i < length; ++i) to[i] = from[i];
    out.pos += length;
  }
}

bool Stored(BitReader& in, Output& out) {
  in.AlignToByte();
  const uint32_t length = in.Take(16);
  if ((in.Take(16) ^ 0xffff) != length || length > out.size - out.pos) return false;
  for (uint32_t i = 0; i < length; ++i) out.data[out.pos++] = (uint8_t)in.Take(8);
  return true;
}

void FixedTables(Huffman* lengths, Huffman* distances) {
  uint8_t l[288];
  int i = 0;
  for (; i < 144; ++i) l[i] = 8;
  for (; i < 256; ++i) l[i] = 9;
  for (; i < 280; ++i) l[i] = 7;
  for (; i < 288; ++i) l[i] = 8;
  Build(lengths, l, 288);
  for (i = 0; i < 30; ++i) l[i] = 5;
  Build(distances, l, 30);
}

bool DynamicTables(BitReader& in, Huffman* lengths, Huffman* distances) {
  static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int literalCount = (int)in.Take(5) + 257;
  const int distanceCount = (int)in.Take(5) + 1;
  const int codeCount = (int)in.Take(4) + 4;
  if (literalCount > 286 || distanceCount > 30) return false;

  // First the code the lengths themselves are written in.
  uint8_t l[286 + 30] = {};
  for (int i = 0; i < codeCount; ++i) l[kOrder[i]] = (uint8_t)in.Take(3);
  Huffman codeLengths;
  if (!Build(&codeLengths, l, 19)) return false;

  const int total = literalCount + distanceCount;
  for (int i = 0; i < total;) {
    const int symbol = Decode(in, codeLengths);
    if (symbol < 0) return false;
    if (symbol < 16) {
      l[i++] = (uint8_t)symbol;
      continue;
    }
    uint8_t value = 0;
    int repeat = 0;
    if (symbol == 16) {
      if (i == 0) return false;
      value = l[i - 1];
      repeat = 3 + (int)in.Take(2);
    } else if (symbol == 17) {
      repeat = 3 + (int)in.Take(3);
    } else {
      repeat = 11 + (int)in.Take(7);
    }
    if (repeat > total - i) return false;
    while (repeat-- > 0) l[i++] = value;
  }
  if (l[256] == 0) return false;  // a block has to be able to end
  return Build(lengths, l, literalCount) && Build(distances, l + literalCount, distanceCount);
}

}  // namespace

bool Inflate(const Packed& packed, uint8_t* out) {
  BitReader in(packed.data, packed.size);
  Output output = {out, packed.unpackedSize, 0};
  Huffman lengths;
  Huffman distances;
  bool last = false;
  while (!last) {
    last = in.Take(1) != 0;
    bool ok = false;
    switch (in.Take(2)) {
      case 0:
        ok = Stored(in, output);
        break;
      case 1:
        FixedTables(&lengths, &distances);
        ok = Codes(in, lengths, distances, output);
        break;
      case 2:
        ok = DynamicTables(in, &lengths, &distances) && Codes(in, lengths, distances, output);
        break;
      default:
        break;
    }
    if (!ok || !in.Within()) return false;
  }
  return output.pos == output.size;
}

}  // namespace cap
