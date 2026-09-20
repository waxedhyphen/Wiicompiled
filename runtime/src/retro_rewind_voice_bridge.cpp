#include "retro_rewind_voice_bridge.h"

#include "runtime_product.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RetroRewindVoiceBridge {
namespace {

constexpr std::uint16_t kGpcmPort = 29900;
constexpr std::size_t kMaxBufferedBytes = 32 * 1024;
constexpr std::string_view kFinal = "\\final\\";

struct SocketState {
    std::string inbound;
    std::string outbound;
    std::string gameName;
};

std::mutex g_mutex;
std::unordered_map<std::uint32_t, SocketState> g_sockets;
IdentitySnapshot g_identity;
std::uint32_t g_identitySocket = UINT32_MAX;

bool IsDecimal(std::string_view value, bool allowNegative = false) {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (allowNegative && value.front() == '-') {
        index = 1;
        if (index == value.size()) {
            return false;
        }
    }
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(index), value.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string ValueForKey(std::string_view message, std::string_view key) {
    std::string marker;
    marker.reserve(key.size() + 2);
    marker.push_back('\\');
    marker.append(key);
    marker.push_back('\\');

    const std::size_t start = message.find(marker);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t valueEnd = message.find('\\', valueStart);
    if (valueEnd == std::string_view::npos) {
        return {};
    }
    return std::string(message.substr(valueStart, valueEnd - valueStart));
}

void TrimBuffer(std::string& buffer) {
    if (buffer.size() <= kMaxBufferedBytes) {
        return;
    }

    // Keep only a bounded tail. GameSpy commands are tiny compared with this
    // limit; retaining unbounded peer data would let one connection grow host
    // memory indefinitely.
    buffer.erase(0, buffer.size() - kMaxBufferedBytes);
}

void ClearIdentityLocked() {
    if (g_identity.online || !g_identity.profileId.empty() ||
        !g_identity.sessionKey.empty() || !g_identity.gameName.empty()) {
        ++g_identity.generation;
    }
    g_identity.online = false;
    g_identity.profileId.clear();
    g_identity.sessionKey.clear();
    g_identity.gameName.clear();
    g_identitySocket = UINT32_MAX;
}

void HandleOutboundLocked(std::uint32_t wiiFd, std::string_view message) {
    SocketState& state = g_sockets[wiiFd];

    if (const std::string gameName = ValueForKey(message, "gamename");
        !gameName.empty() && gameName.size() <= 32) {
        state.gameName = gameName;
    }

    // A GameSpy logout explicitly ends the live bearer session even before the
    // TCP socket is physically closed.
    if (message.find("\\logout\\") != std::string_view::npos &&
        g_identitySocket == wiiFd) {
        ClearIdentityLocked();
    }
}

void HandleInboundLocked(std::uint32_t wiiFd, std::string_view message) {
    // Successful GPCM login response:
    //   \lc\2\sesskey\...\profileid\...\...\final\
    if (message.find("\\lc\\2\\") == std::string_view::npos) {
        return;
    }

    const std::string profileId = ValueForKey(message, "profileid");
    const std::string sessionKey = ValueForKey(message, "sesskey");
    if (!IsDecimal(profileId) || profileId.size() > 10 ||
        !IsDecimal(sessionKey, true) || sessionKey.size() > 11) {
        return;
    }

    SocketState& state = g_sockets[wiiFd];
    const std::string gameName = state.gameName.empty() ? "mariokartwii" : state.gameName;

    const bool changed = !g_identity.online ||
                         g_identity.profileId != profileId ||
                         g_identity.sessionKey != sessionKey ||
                         g_identity.gameName != gameName ||
                         g_identitySocket != wiiFd;

    g_identity.online = true;
    g_identity.profileId = profileId;
    g_identity.sessionKey = sessionKey;
    g_identity.gameName = gameName;
    g_identitySocket = wiiFd;
    if (changed) {
        ++g_identity.generation;
    }
}

template <typename Handler>
void FeedLocked(std::uint32_t wiiFd, std::string& buffer,
                const std::uint8_t* data, std::size_t size, Handler&& handler) {
    if (!data || size == 0) {
        return;
    }

    buffer.append(reinterpret_cast<const char*>(data), size);
    TrimBuffer(buffer);

    while (true) {
        const std::size_t end = buffer.find(kFinal);
        if (end == std::string::npos) {
            break;
        }

        const std::size_t packetEnd = end + kFinal.size();
        std::string packet = buffer.substr(0, packetEnd);
        buffer.erase(0, packetEnd);
        handler(wiiFd, packet);
    }
}

bool ShouldObserve(std::uint16_t peerPort) {
    // Some GameSpy sockets are nonblocking and can exchange their first packet
    // before the HLE has recorded the connected peer port. Accept port 0 as an
    // unknown-yet candidate; the parser still requires the exact GPCM login
    // message shape before it captures anything.
    return RuntimeProduct::IsRetroRewind() &&
           (peerPort == 0 || peerPort == kGpcmPort);
}

} // namespace

void ObserveGpcmSend(std::uint32_t wiiFd, std::uint16_t peerPort,
                     const std::uint8_t* data, std::size_t size) noexcept {
    if (!ShouldObserve(peerPort)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        SocketState& state = g_sockets[wiiFd];
        FeedLocked(wiiFd, state.outbound, data, size, HandleOutboundLocked);
    } catch (...) {
        // Voice integration is observational. It must never break the game's
        // networking path if host allocation/parsing fails.
    }
}

void ObserveGpcmReceive(std::uint32_t wiiFd, std::uint16_t peerPort,
                        const std::uint8_t* data, std::size_t size) noexcept {
    if (!ShouldObserve(peerPort)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        SocketState& state = g_sockets[wiiFd];
        FeedLocked(wiiFd, state.inbound, data, size, HandleInboundLocked);
    } catch (...) {
        // See ObserveGpcmSend: the bridge must remain side-effect free.
    }
}

void OnSocketClosed(std::uint32_t wiiFd, std::uint16_t peerPort) noexcept {
    (void)peerPort;
    if (!RuntimeProduct::IsRetroRewind()) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sockets.erase(wiiFd);
        if (g_identitySocket == wiiFd) {
            ClearIdentityLocked();
        }
    } catch (...) {
    }
}

IdentitySnapshot Snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_identity;
}

} // namespace RetroRewindVoiceBridge
