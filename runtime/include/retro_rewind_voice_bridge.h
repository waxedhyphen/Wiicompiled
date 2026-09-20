#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace RetroRewindVoiceBridge {

// Host-only snapshot of the live Retro Rewind GPCM identity observed by the
// WiiCompiled networking HLE. The session key is intentionally memory-only:
// callers must never persist or log it.
struct IdentitySnapshot {
    bool online = false;
    std::string profileId;
    std::string sessionKey;
    std::string gameName;
    std::uint64_t generation = 0;
};

void ObserveGpcmSend(std::uint32_t wiiFd, std::uint16_t peerPort,
                     const std::uint8_t* data, std::size_t size) noexcept;
void ObserveGpcmReceive(std::uint32_t wiiFd, std::uint16_t peerPort,
                        const std::uint8_t* data, std::size_t size) noexcept;
void OnSocketClosed(std::uint32_t wiiFd, std::uint16_t peerPort) noexcept;

IdentitySnapshot Snapshot();

} // namespace RetroRewindVoiceBridge
