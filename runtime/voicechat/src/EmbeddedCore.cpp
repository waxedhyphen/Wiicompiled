#include "mkwvc/EmbeddedCore.hpp"
#include "mkwvc/VoiceFormat.hpp"

namespace mkwvc {

EmbeddedCoreStatus embeddedCoreStatus() noexcept {
    EmbeddedCoreStatus status;
    status.sampleRate = VoiceFormat::SampleRate;
    status.frameDurationMs = VoiceFormat::FrameDurationMs;
    status.frameSamples = static_cast<std::uint32_t>(VoiceFormat::FrameSamples);
    status.voiceClientCompiled = true;
    status.audioDependenciesLinked = false;
    status.iceDependenciesLinked = false;
    return status;
}

}
