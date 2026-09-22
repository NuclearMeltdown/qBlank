#pragma once

// The Windows half of keys.h: virtual-key codes to keys and back.

#include "keys.h"

namespace cap {

// Key::None for codes that are no key on the list -- mouse buttons, gamepad
// controls, unassigned numbers.
Key KeyFromVirtualKey(unsigned vk);
unsigned VirtualKeyOf(Key key);

}  // namespace cap
