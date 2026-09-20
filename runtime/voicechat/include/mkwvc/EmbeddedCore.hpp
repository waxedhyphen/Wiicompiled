#pragma once

#include <cstdint>

namespace mkwvc {

struct EmbeddedCoreStatus {
    std::uint32_t sampleRate = 0;
    std::uint32_t frameDurationMs = 0;
    std::uint32_t frameSamples = 0;
    bool voiceClientCompiled = false;
    bool audioDependenciesLinked = false;
    bool iceDependenciesLinked = false;
};

EmbeddedCoreStatus embeddedCoreStatus() noexcept;

}
