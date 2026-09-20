#include "retro_rewind_voice_bridge.h"

#include "runtime_product.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <atomic>
#include <condition_variable>
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
constexpr unsigned kRoomLeaveConfirmations = 3;

struct RoomLookupState {
    std::mutex mutex;
    std::condition_variable wake;
    RoomSnapshot snapshot;
    std::uint64_t observedIdentityGeneration = UINT64_MAX;
    std::uint64_t desiredIdentityGeneration = 0;
    std::uint64_t workerIdentityGeneration = UINT64_MAX;
    std::string desiredProfileId;
    bool desiredOnline = false;
    bool workerRunning = false;
    bool manualRefreshRequested = false;
    unsigned consecutiveRoomMisses = 0;
    std::chrono::steady_clock::time_point nextReconnectAttempt{};
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
        player.voiceChat = line.substr(first + 1, second - first - 1) == "1";
        player.name = DecodeHex(std::string_view(line).substr(second + 1));
        if (player.name.empty()) {
            player.name = "Player";
        }
        if (!player.profileId.empty()) {
            result.players.push_back(std::move(player));
        }
    }

    if (result.players.size() != expectedPlayers) {
        result.status = "Incomplete Retro Rewind roster response";
        return result;
    }

    // From here on the Worker reply was structurally valid. An empty room is a
    // real negative roster result, not a transport/parser failure.
    result.lookupSucceeded = true;

    if (result.roomId.empty()) {
        result.status = "Online, but not currently present in a public RR room";
        return result;
    }
    if (result.roomInstanceId.empty() || result.created.empty()) {
        result.lookupSucceeded = false;
        result.status = "Incomplete Retro Rewind room response";
        return result;
    }

    result.roomFound = true;
    result.status = "Room found through public RWFC roster (unverified)";
    return result;
}

#if defined(_WIN32)

constexpr auto kRoomPresenceRefreshInterval = std::chrono::seconds(30);
constexpr auto kRoomReconnectDelay = std::chrono::seconds(5);

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
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (value) WinHttpCloseHandle(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

DWORD SendWebSocketText(HINTERNET socket, const std::string& message) {
    return WinHttpWebSocketSend(
        socket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<DWORD>(message.size()));
}

DWORD ReceiveWebSocketText(HINTERNET socket, std::string& message, bool& closed) {
    message.clear();
    closed = false;

    while (message.size() <= kMaxRoomReplyBytes) {
        std::array<char, 4096> buffer{};
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD error = WinHttpWebSocketReceive(
            socket,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            &type);
        if (error != ERROR_SUCCESS) {
            return error;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            closed = true;
            return ERROR_SUCCESS;
        }
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return ERROR_INVALID_DATA;
        }

        message.append(buffer.data(), bytesRead);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return message.size() <= kMaxRoomReplyBytes ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
        }
    }

    return ERROR_INSUFFICIENT_BUFFER;
}

void ApplyRoomResultLocked(RoomLookupState& state, RoomSnapshot result,
                           std::uint64_t generation) {
    result.lookupInFlight = false;
    result.identityGeneration = generation;

    if (!state.desiredOnline || state.desiredIdentityGeneration != generation) {
        return;
    }

    const bool hadConfirmedRoom =
        state.snapshot.roomFound &&
        state.snapshot.identityGeneration == generation;

    if (result.lookupSucceeded && result.roomFound) {
        // Positive room evidence immediately accepts joins and room switches.
        state.snapshot = std::move(result);
        state.consecutiveRoomMisses = 0;
    } else if (result.lookupSucceeded && !result.roomFound && hadConfirmedRoom) {
        // Public roster transitions can briefly omit a participant. Three
        // consecutive structurally valid misses are required before clearing
        // an already confirmed room.
        ++state.consecutiveRoomMisses;
        if (state.consecutiveRoomMisses >= kRoomLeaveConfirmations) {
            state.snapshot = std::move(result);
            state.consecutiveRoomMisses = 0;
        } else {
            state.snapshot.lookupInFlight = false;
            state.snapshot.lookupComplete = true;
            state.snapshot.status =
                "Room still active; background verification miss " +
                std::to_string(state.consecutiveRoomMisses) + "/" +
                std::to_string(kRoomLeaveConfirmations);
        }
    } else if (!result.lookupSucceeded && hadConfirmedRoom) {
        // A Worker/network/parser failure is not evidence of a room leave.
        state.snapshot.lookupInFlight = false;
        state.snapshot.lookupComplete = true;
        state.snapshot.status =
            "Room still active; background verification temporarily unavailable";
    } else {
        state.snapshot = std::move(result);
        if (state.snapshot.lookupSucceeded) {
            state.consecutiveRoomMisses = 0;
        }
    }
}

void FinishRoomWorker(std::uint64_t generation, std::string status,
                      bool reconnect) {
    RoomLookupState& state = RoomState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.workerIdentityGeneration != generation) {
            return;
        }

        state.workerRunning = false;
        state.workerIdentityGeneration = UINT64_MAX;
        state.snapshot.lookupInFlight = false;

        if (state.desiredOnline && state.desiredIdentityGeneration == generation) {
            if (!status.empty()) {
                state.snapshot.lookupComplete = true;
                if (state.snapshot.roomFound) {
                    state.snapshot.status =
                        "Room still active; signaling connection will reconnect";
                } else {
                    state.snapshot.status = std::move(status);
                }
            }
            state.nextReconnectAttempt = reconnect
                ? std::chrono::steady_clock::now() + kRoomReconnectDelay
                : std::chrono::steady_clock::time_point{};
        } else {
            state.nextReconnectAttempt = {};
        }
    }
    state.wake.notify_all();
}

void PersistentRoomWorker(std::string profileId, std::uint64_t generation) {
    constexpr wchar_t kWorkerHost[] = L"mkw-voicechat-signaling.mkwvoicechat.workers.dev";

    WinHttpHandle session(WinHttpOpen(
        L"MKW VoiceChat WiiCompiled bridge/0.2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpOpen"), true);
        return;
    }

    // Connection setup should fail quickly, while the receive side may remain
    // blocked because the sender refreshes presence every 30 seconds.
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 60000);

    WinHttpHandle connection(WinHttpConnect(
        session.value, kWorkerHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpConnect"), true);
        return;
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
        FinishRoomWorker(generation, WinHttpError("WinHttpOpenRequest"), true);
        return;
    }

    DWORD error = WinHttpSetOption(
        request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        ? ERROR_SUCCESS : GetLastError();
    if (error != ERROR_SUCCESS) {
        FinishRoomWorker(generation, WinHttpError("WebSocket upgrade", error), true);
        return;
    }
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        FinishRoomWorker(generation, WinHttpError("WinHttpSendRequest"), true);
        return;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        FinishRoomWorker(generation, WinHttpError("WinHttpReceiveResponse"), true);
        return;
    }

    WinHttpHandle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
    if (!socket.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpWebSocketCompleteUpgrade"), true);
        return;
    }

    DWORD keepAliveMs = 30000;
    (void)WinHttpSetOption(
        socket.value,
        WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepAliveMs,
        sizeof(keepAliveMs));
    DWORD closeTimeoutMs = 5000;
    (void)WinHttpSetOption(
        socket.value,
        WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
        &closeTimeoutMs,
        sizeof(closeTimeoutMs));

    RoomLookupState& state = RoomState();
    std::atomic<bool> connectionAlive{true};
    std::atomic<DWORD> sendFailure{ERROR_SUCCESS};

    // WinHTTP explicitly permits one WebSocket sender and one receiver to run
    // concurrently. The receiver below consumes Worker-pushed presence updates;
    // this sender handles the initial registration, 30-second verification and
    // manual refresh requests without touching the game/UI thread.
    std::thread sender([&]() {
        const auto sendPresence = [&]() -> bool {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (!state.desiredOnline ||
                    state.desiredIdentityGeneration != generation ||
                    !connectionAlive.load(std::memory_order_acquire)) {
                    return false;
                }
                state.snapshot.lookupInFlight = true;
                state.snapshot.identityGeneration = generation;
                state.snapshot.profileId = profileId;
                if (!state.snapshot.lookupComplete && !state.snapshot.roomFound) {
                    state.snapshot.status = "Verifying Retro Rewind voice presence...";
                }
            }

            const std::string command = "RR_DEBUG_PRESENCE " + profileId;
            const DWORD sendError = SendWebSocketText(socket.value, command);
            if (sendError != ERROR_SUCCESS) {
                sendFailure.store(sendError, std::memory_order_release);
                connectionAlive.store(false, std::memory_order_release);
                state.wake.notify_all();
                (void)WinHttpWebSocketShutdown(
                    socket.value,
                    WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS,
                    nullptr,
                    0);
                return false;
            }
            return true;
        };

        if (!sendPresence()) {
            return;
        }

        while (connectionAlive.load(std::memory_order_acquire)) {
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.wake.wait_for(lock, kRoomPresenceRefreshInterval, [&]() {
                    return !connectionAlive.load(std::memory_order_acquire) ||
                           !state.desiredOnline ||
                           state.desiredIdentityGeneration != generation ||
                           state.manualRefreshRequested;
                });

                stop =
                    !connectionAlive.load(std::memory_order_acquire) ||
                    !state.desiredOnline ||
                    state.desiredIdentityGeneration != generation;

                if (!stop) {
                    // Multiple manual clicks collapse into one immediate refresh.
                    state.manualRefreshRequested = false;
                }
            }

            if (stop) {
                break;
            }
            if (!sendPresence()) {
                return;
            }
        }

        if (connectionAlive.load(std::memory_order_acquire)) {
            const std::string clear = "RR_DEBUG_PRESENCE_CLEAR";
            (void)SendWebSocketText(socket.value, clear);
            (void)WinHttpWebSocketShutdown(
                socket.value,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                nullptr,
                0);
            connectionAlive.store(false, std::memory_order_release);
        }
    });

    std::string terminalStatus;
    bool reconnect = true;

    while (connectionAlive.load(std::memory_order_acquire)) {
        std::string message;
        bool closed = false;
        error = ReceiveWebSocketText(socket.value, message, closed);
        if (error != ERROR_SUCCESS) {
            terminalStatus = WinHttpError("WinHttpWebSocketReceive", error);
            connectionAlive.store(false, std::memory_order_release);
            state.wake.notify_all();
            break;
        }
        if (closed) {
            connectionAlive.store(false, std::memory_order_release);
            state.wake.notify_all();
            terminalStatus = "Voice signaling WebSocket closed";
            break;
        }

        RoomSnapshot result = ParseRoomReply(message, profileId);
        std::lock_guard<std::mutex> lock(state.mutex);
        ApplyRoomResultLocked(state, std::move(result), generation);
    }

    state.wake.notify_all();
    if (sender.joinable()) {
        sender.join();
    }

    const DWORD failedSend = sendFailure.load(std::memory_order_acquire);
    if (failedSend != ERROR_SUCCESS) {
        terminalStatus = WinHttpError("WinHttpWebSocketSend", failedSend);
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // Identity change/offline is an intentional stop. The next identity is
        // started by ServiceRoomLookup without reporting a transport failure.
        if (!state.desiredOnline || state.desiredIdentityGeneration != generation) {
            reconnect = false;
            terminalStatus.clear();
        }
    }

    FinishRoomWorker(generation, std::move(terminalStatus), reconnect);
}

#else

constexpr auto kRoomReconnectDelay = std::chrono::seconds(5);

void FinishRoomWorker(std::uint64_t generation, std::string status,
                      bool reconnect) {
    RoomLookupState& state = RoomState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.workerIdentityGeneration != generation) {
            return;
        }

        state.workerRunning = false;
        state.workerIdentityGeneration = UINT64_MAX;
        state.snapshot.lookupInFlight = false;

        if (state.desiredOnline && state.desiredIdentityGeneration == generation) {
            if (!status.empty()) {
                state.snapshot.lookupComplete = true;
                if (state.snapshot.roomFound) {
                    state.snapshot.status =
                        "Room still active; signaling connection will reconnect";
                } else {
                    state.snapshot.status = std::move(status);
                }
            }
            state.nextReconnectAttempt = reconnect
                ? std::chrono::steady_clock::now() + kRoomReconnectDelay
                : std::chrono::steady_clock::time_point{};
        } else {
            state.nextReconnectAttempt = {};
        }
    }
    state.wake.notify_all();
}

void PersistentRoomWorker(std::string, std::uint64_t generation) {
    FinishRoomWorker(generation,
                     "RR signaling presence is not implemented on this host platform yet",
                     false);
}

#endif

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
        const auto now = std::chrono::steady_clock::now();

        bool notifyWorker = false;
        bool startWorker = false;

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.desiredOnline = identity.online && !identity.profileId.empty();
            state.desiredIdentityGeneration = identity.generation;
            state.desiredProfileId = identity.profileId;

            if (!state.desiredOnline) {
                notifyWorker = state.workerRunning;
                state.manualRefreshRequested = false;
                state.nextReconnectAttempt = {};

                if (state.observedIdentityGeneration != identity.generation ||
                    state.snapshot.lookupInFlight ||
                    state.snapshot.lookupComplete ||
                    state.snapshot.roomFound) {
                    state.observedIdentityGeneration = identity.generation;
                    state.snapshot = {};
                    state.snapshot.identityGeneration = identity.generation;
                    state.snapshot.status = "Waiting for live Retro Rewind identity";
                    state.consecutiveRoomMisses = 0;
                }
            } else {
                const bool identityChanged =
                    state.observedIdentityGeneration != identity.generation;

                if (identityChanged) {
                    state.observedIdentityGeneration = identity.generation;
                    state.consecutiveRoomMisses = 0;
                    state.manualRefreshRequested = false;
                    state.nextReconnectAttempt = {};

                    // Never carry a confirmed room from an old GPCM session into
                    // a new identity generation.
                    state.snapshot = {};
                    state.snapshot.identityGeneration = identity.generation;
                    state.snapshot.profileId = identity.profileId;
                    state.snapshot.status = "Connecting to voice signaling...";
                }

                if (state.workerRunning) {
                    if (state.workerIdentityGeneration != identity.generation) {
                        notifyWorker = true;
                    }
                } else if (state.nextReconnectAttempt == std::chrono::steady_clock::time_point{} ||
                           now >= state.nextReconnectAttempt) {
                    state.workerRunning = true;
                    state.workerIdentityGeneration = identity.generation;
                    state.manualRefreshRequested = false;
                    state.snapshot.lookupInFlight = true;
                    state.snapshot.identityGeneration = identity.generation;
                    state.snapshot.profileId = identity.profileId;
                    if (!state.snapshot.lookupComplete && !state.snapshot.roomFound) {
                        state.snapshot.status = "Connecting to voice signaling...";
                    }
                    startWorker = true;
                }
            }
        }

        if (notifyWorker) {
            state.wake.notify_all();
        }

        if (startWorker) {
            try {
                std::thread(
                    PersistentRoomWorker,
                    identity.profileId,
                    identity.generation).detach();
            } catch (...) {
                FinishRoomWorker(
                    identity.generation,
                    "Failed to start persistent RR signaling worker",
                    true);
            }
        }
    } catch (...) {
    }
}

void RequestRoomLookup() noexcept {
    try {
        const IdentitySnapshot identity = Snapshot();
        if (!identity.online || identity.profileId.empty()) {
            return;
        }

        RoomLookupState& state = RoomState();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.manualRefreshRequested = true;
            state.nextReconnectAttempt = {};
        }
        state.wake.notify_all();
    } catch (...) {
    }
}

RoomSnapshot Room() {
    RoomLookupState& state = RoomState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot;
}

} // namespace RetroRewindVoiceBridge
