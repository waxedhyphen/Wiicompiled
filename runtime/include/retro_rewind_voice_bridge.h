#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// Public-RWFC roster discovery is deliberately diagnostic only. It proves the
// in-process identity -> signaling Worker -> RR room path, but it does not
// authorize voice membership until the separate trusted RR verifier exists.
struct RoomPlayer {
    std::string profileId;
    std::string name;
    bool voiceChat = false;
};

struct RoomSnapshot {
    bool lookupInFlight = false;
    bool lookupComplete = false;
    bool lookupSucceeded = false;
    bool localRoomActive = false;
    bool roomFound = false;
    bool signalingConnected = false;
    bool presenceFrameSent = false;
    bool signalingReplyReceived = false;
    std::string status;
    std::string profileId;
    std::string roomId;
    std::string roomInstanceId;
    std::string created;
    std::vector<RoomPlayer> players;
    std::uint64_t identityGeneration = 0;
};

// Called from the host runtime once per presented frame. A live GPCM identity
// keeps one persistent signaling WebSocket alive in a background worker. Local
// RKNet room state gates voice presence immediately: leaving the game room
// clears voice presence without waiting for the public roster poll. Worker-
 // pushed presence updates are consumed without network I/O on the game/UI thread.
void ServiceRoomLookup() noexcept;
RoomSnapshot Room();

} // namespace RetroRewindVoiceBridge
