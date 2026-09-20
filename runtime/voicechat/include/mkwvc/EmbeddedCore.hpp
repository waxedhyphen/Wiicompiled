#pragma once

#include <cstdint>
#include <string>

namespace mkwvc {

struct EmbeddedCoreStatus {
    std::uint32_t sampleRate = 0;
    std::uint32_t frameDurationMs = 0;
    std::uint32_t frameSamples = 0;
    bool voiceClientCompiled = false;
    bool audioDependenciesLinked = false;
    bool iceDependenciesLinked = false;
};

struct EmbeddedVoiceSessionInput {
    bool localRoomActive = false;
    bool roomFound = false;
    bool roomAuthorized = false;
    std::string profileId;
    std::string roomInstanceId;
};

struct EmbeddedVoiceSessionStatus {
    bool lifecycleActive = false;
    bool signalingConnected = false;
    bool roomAuthorized = false;
    bool voiceClientRunning = false;
    std::uint32_t peerCount = 0;
    std::string roomInstanceId;
    std::string status;
};

EmbeddedCoreStatus embeddedCoreStatus() noexcept;
void serviceEmbeddedVoiceSession(const EmbeddedVoiceSessionInput& input) noexcept;
EmbeddedVoiceSessionStatus embeddedVoiceSessionStatus();

}
