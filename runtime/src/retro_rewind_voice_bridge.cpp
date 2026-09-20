#include "retro_rewind_voice_bridge.h"

#include "runtime_product.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#endif

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

constexpr std::size_t kMaxRoomReplyBytes = 64 * 1024;

struct RoomLookupState {
    std::mutex mutex;
    RoomSnapshot snapshot;
    std::uint64_t observedIdentityGeneration = UINT64_MAX;
    std::uint64_t lookupToken = 0;
};

RoomLookupState& RoomState() {
    // Intentionally process-lifetime storage: lookup workers are detached so
    // shutdown can never race a C++ static destructor for this state.
    static RoomLookupState* state = new RoomLookupState();
    return *state;
}

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
    // Successful GPCM login response ending with the GameSpy final terminator.
    // Example fields include lc=2, sesskey, profileid and the GameSpy final terminator.
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


std::vector<std::string> SplitLines(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string DecodeHex(std::string_view value) {
    if (value.size() % 2 != 0) {
        return {};
    }
    std::string decoded;
    decoded.reserve(value.size() / 2);
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = nibble(value[i]);
        const int lo = nibble(value[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
    }
    return decoded;
}

RoomSnapshot ParseRoomReply(const std::string& message, std::string_view expectedProfileId) {
    RoomSnapshot result;
    result.lookupComplete = true;

    if (message.rfind("RR_DEBUG_FAIL ", 0) == 0) {
        result.status = message.substr(14);
        return result;
    }
    if (message.rfind("ERROR ", 0) == 0) {
        result.status = message.substr(6);
        return result;
    }
    constexpr std::string_view prefix = "RR_DEBUG_STATUS\n";
    if (message.rfind(prefix, 0) != 0) {
        result.status = "Unexpected signaling response";
        return result;
    }

    const std::vector<std::string> lines = SplitLines(message.substr(prefix.size()));
    if (lines.size() < 5 || lines[0] != expectedProfileId) {
        result.status = "Malformed Retro Rewind room response";
        return result;
    }

    result.profileId = lines[0];
    result.roomId = lines[1];
    result.roomInstanceId = lines[2];
    result.created = lines[3];

    std::size_t expectedPlayers = 0;
    const auto parsed = std::from_chars(lines[4].data(), lines[4].data() + lines[4].size(), expectedPlayers);
    if (parsed.ec != std::errc{} || parsed.ptr != lines[4].data() + lines[4].size() || expectedPlayers > 12) {
        result.status = "Malformed Retro Rewind roster count";
        return result;
    }

    for (std::size_t i = 5; i < lines.size() && result.players.size() < expectedPlayers; ++i) {
        const std::string& line = lines[i];
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }
        RoomPlayer player;
        player.profileId = line.substr(0, first);
        player.name = DecodeHex(std::string_view(line).substr(second + 1));
        if (player.name.empty()) {
            player.name = "Player";
        }
        if (!player.profileId.empty()) {
            result.players.push_back(std::move(player));
        }
    }

    if (result.roomId.empty()) {
        result.status = "Online, but not currently present in a public RR room";
        return result;
    }
    if (result.roomInstanceId.empty() || result.created.empty() || result.players.size() != expectedPlayers) {
        result.status = "Incomplete Retro Rewind room response";
        return result;
    }

    result.roomFound = true;
    result.status = "Room found through public RWFC roster (unverified)";
    return result;
}

#if defined(_WIN32)

std::string WinHttpError(const char* operation, DWORD error = GetLastError()) {
    return std::string(operation) + " failed (WinHTTP " + std::to_string(error) + ")";
}

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

RoomSnapshot LookupRoomViaSignalingWorker(const std::string& profileId) {
    constexpr wchar_t kWorkerHost[] = L"mkw-voicechat-signaling.mkwvoicechat.workers.dev";

    WinHttpHandle session(WinHttpOpen(
        L"MKW VoiceChat WiiCompiled bridge/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.value) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpOpen");
        return result;
    }
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);

    WinHttpHandle connection(WinHttpConnect(session.value, kWorkerHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpConnect");
        return result;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.value,
        L"GET",
        L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.value) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpOpenRequest");
        return result;
    }

    DWORD error = WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        ? ERROR_SUCCESS : GetLastError();
    if (error != ERROR_SUCCESS) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WebSocket upgrade", error);
        return result;
    }
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpSendRequest");
        return result;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpReceiveResponse");
        return result;
    }

    WinHttpHandle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
    if (!socket.value) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpWebSocketCompleteUpgrade");
        return result;
    }

    const std::string command = "RR_DEBUG_LOOKUP " + profileId;
    error = WinHttpWebSocketSend(socket.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 const_cast<char*>(command.data()),
                                 static_cast<DWORD>(command.size()));
    if (error != ERROR_SUCCESS) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = WinHttpError("WinHttpWebSocketSend", error);
        return result;
    }

    std::string message;
    while (message.size() <= kMaxRoomReplyBytes) {
        std::array<char, 4096> buffer{};
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        error = WinHttpWebSocketReceive(socket.value, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        &bytesRead, &type);
        if (error != ERROR_SUCCESS) {
            RoomSnapshot result;
            result.lookupComplete = true;
            result.status = WinHttpError("WinHttpWebSocketReceive", error);
            return result;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            RoomSnapshot result;
            result.lookupComplete = true;
            result.status = "Signaling socket closed before RR room reply";
            return result;
        }
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            RoomSnapshot result;
            result.lookupComplete = true;
            result.status = "Signaling Worker returned a non-text RR room reply";
            return result;
        }
        message.append(buffer.data(), bytesRead);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            break;
        }
    }

    WinHttpWebSocketClose(socket.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    if (message.size() > kMaxRoomReplyBytes) {
        RoomSnapshot result;
        result.lookupComplete = true;
        result.status = "Retro Rewind room reply exceeded size limit";
        return result;
    }
    return ParseRoomReply(message, profileId);
}

#else

RoomSnapshot LookupRoomViaSignalingWorker(const std::string&) {
    RoomSnapshot result;
    result.lookupComplete = true;
    result.status = "RR signaling lookup is not implemented on this host platform yet";
    return result;
}

#endif

bool StartRoomLookup(const IdentitySnapshot& identity) noexcept {
    if (!identity.online || identity.profileId.empty()) {
        return false;
    }

    RoomLookupState& state = RoomState();
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.snapshot.lookupInFlight) {
            return false;
        }
        token = ++state.lookupToken;
        state.snapshot = {};
        state.snapshot.lookupInFlight = true;
        state.snapshot.status = "Looking up current Retro Rewind room...";
        state.snapshot.profileId = identity.profileId;
        state.snapshot.identityGeneration = identity.generation;
    }

    try {
        std::thread([profileId = identity.profileId, generation = identity.generation, token]() {
            RoomSnapshot result = LookupRoomViaSignalingWorker(profileId);
            result.lookupInFlight = false;
            result.identityGeneration = generation;

            RoomLookupState& state = RoomState();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.lookupToken != token) {
                return;
            }
            state.snapshot = std::move(result);
        }).detach();
        return true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.lookupToken == token) {
            state.snapshot.lookupInFlight = false;
            state.snapshot.lookupComplete = true;
            state.snapshot.status = "Failed to start RR room lookup worker";
        }
        return false;
    }
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

void ServiceRoomLookup() noexcept {
    try {
        const IdentitySnapshot identity = Snapshot();
        RoomLookupState& state = RoomState();

        if (!identity.online || identity.profileId.empty()) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.observedIdentityGeneration != identity.generation ||
                state.snapshot.lookupInFlight || state.snapshot.lookupComplete) {
                state.observedIdentityGeneration = identity.generation;
                ++state.lookupToken;
                state.snapshot = {};
                state.snapshot.identityGeneration = identity.generation;
                state.snapshot.status = "Waiting for live Retro Rewind identity";
            }
            return;
        }

        bool needsLookup = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            needsLookup = state.observedIdentityGeneration != identity.generation;
        }
        if (needsLookup && StartRoomLookup(identity)) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.observedIdentityGeneration = identity.generation;
        }
    } catch (...) {
    }
}

void RequestRoomLookup() noexcept {
    try {
        const IdentitySnapshot identity = Snapshot();
        (void)StartRoomLookup(identity);
    } catch (...) {
    }
}

RoomSnapshot Room() {
    RoomLookupState& state = RoomState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot;
}

} // namespace RetroRewindVoiceBridge
