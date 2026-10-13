#pragma once
// lockdownd GetValue(DeviceName) over a usbmux tunnel to port 62078. Only a fallback for when
// the sideband handshake does not deliver a name; the iOS app is the primary source.
#include <stdint.h>
#include <string>

namespace cf::lockdown {

inline constexpr uint16_t kPort = 62078;

bool queryDeviceName(uint32_t deviceId, std::string& name, std::string& err);

}  // namespace cf::lockdown
