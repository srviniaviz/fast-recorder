#pragma once

#include <string>

namespace fastrecord::recording {

struct NvencProbeResult {
    bool runtimeLibraryFound{false};
    bool sdkBindingsCompiled{false};
    bool encodeSessionOpened{false};
    bool h264Supported{false};
    std::wstring description;
};

NvencProbeResult probeNvenc();

} // namespace fastrecord::recording

