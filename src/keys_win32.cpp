#include "keys_win32.h"

#include "common.h"
#include "common_win32.h"
#include "text_win32.h"

namespace cap {

// The numbers in the file were Windows' own to begin with, so on Windows the
// legacy column is exactly the virtual-key table.
Key KeyFromVirtualKey(unsigned vk) {
  return vk < 256 ? KeyFromLegacyCode((int)vk) : Key::None;
}

unsigned VirtualKeyOf(Key key) {
  return (unsigned)LegacyCode(key);
}

std::string KeyLabel(Key key) {
  const unsigned vk = VirtualKeyOf(key);

  // From the layout, so the label matches the key cap in front of the user
  // rather than a US keyboard.
  const UINT scan = ::MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
  wchar_t name[64] = {};
  if (scan != 0 && ::GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0) return ToUtf8(name);

  if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
  if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
  return Format("VK 0x%02X", vk);
}

}  // namespace cap
