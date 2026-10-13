#pragma once
#include <string>
#include "options.h"

namespace cf {

struct EnvResult {
    bool ok = true;            // all hard requirements met
    uint32_t osBuild = 0;
    bool amds = false;
    bool nvdec = false;
    bool mfRegistered = false, dsRegistered64 = false, dsRegistered32 = false;
    std::string gpuReport;
};

// Prints each check. Hard failures (Windows 10, no NVDEC, no AMDS when needed) set ok=false
// with a message saying exactly what to do.
EnvResult checkEnvironment(const Options& o, bool needAmds);

uint32_t windowsBuild();

// Why AMDS might list no iPhone even though one is plugged in: looks at which driver owns each
// present Apple (VID_05AC) USB device. Empty when nothing is wrong or no Apple device is present.
std::string appleUsbDriverHint();

}  // namespace cf
