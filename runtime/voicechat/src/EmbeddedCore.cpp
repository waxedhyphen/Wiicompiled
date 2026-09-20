#include "mkwvc/EmbeddedCore.hpp"
#include "mkwvc/SignalingClient.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <miniaudio.h>
#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#elif __has_include(<opus.h>)
#include <opus.h>
#else
#error "Opus headers were not found"
#endif
#include <rtc/rtc.hpp>
#include <speex/speex_preprocess.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using MiniAudioVersionFn = const char* (*)(void);
using OpusVersionFn = const char* (*)(void);
using SpeexInitFn = SpeexPreprocessState* (*)(int, int);
using RtcPreloadFn = void (*)(void);

MiniAudioVersionFn volatile g_miniaudioLinkProbe = &ma_version_string;
OpusVersionFn volatile g_opusLinkProbe = &opus_get_version_string;
SpeexInitFn volatile g_speexLinkProbe = &speex_preprocess_state_init;
RtcPreloadFn volatile g_rtcLinkProbe = &rtc::Preload;

constexpr std::string_view kSignalingUrl =
    "wss://mkw-voicechat-signaling.mkwvoicechat.workers.dev/";
constexpr auto kReconnectDelay = std::chrono::seconds(5);

struct EmbeddedVoiceSessionState {
    std::mutex mutex;
    std::unique_ptr<mkwvc::SignalingClient> signaling;
    mkwvc::EmbeddedVoiceSessionStatus status;
    std::string profileId;
    std::string roomInstanceId;
    std::unordered_map<std::string,std::string> authorizedPeers;
    std::uint64_t identityGeneration = 0;
    std::chrono::steady_clock::time_point nextReconnect{};
};

EmbeddedVoiceSessionState& voiceSessionState() {
    static auto* state = new EmbeddedVoiceSessionState();
    return *state;
}

void clearVoiceSessionLocked(EmbeddedVoiceSessionState& state, std::string status) {
    state.signaling.reset();
    state.profileId.clear();
    state.roomInstanceId.clear();
    state.authorizedPeers.clear();
    state.identityGeneration = 0;
    state.nextReconnect = {};
    state.status = {};
    state.status.status = std::move(status);
}

std::vector<std::string_view> splitLines(std::string_view value) {
    std::vector<std::string_view> lines;
    while (true) {
        const auto end = value.find('\n');
        lines.push_back(value.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        value.remove_prefix(end + 1);
    }
    return lines;
}

bool matchesAuthorizedRoom(
    std::string_view payload,
    std::string_view profileId,
    std::string_view roomInstanceId) {
    const auto lines = splitLines(payload);
    return lines.size() >= 3 &&
           lines[0] == profileId &&
           lines[2] == roomInstanceId;
}

bool parseAdmission(
    std::string_view payload,
    std::string& memberId,
    std::string& roomInstanceId) {
    const auto lines=splitLines(payload);
    if(lines.size()<2 || lines[0].size()!=32 || lines[1].size()!=64) return false;
    memberId=std::string(lines[0]);
    roomInstanceId=std::string(lines[1]);
    return true;
}

bool parseAuthorizedPeer(
    std::string_view payload,
    std::string_view expectedRoomInstanceId,
    std::string& memberId,
    std::string& participantId) {
    const auto lines=splitLines(payload);
    if(lines.size()<5 ||
       lines[0].size()!=32 ||
       lines[1].empty() ||
       lines[2]!=expectedRoomInstanceId) {
        return false;
    }
    memberId=std::string(lines[0]);
    participantId=std::string(lines[1]);
    return true;
}

}

namespace mkwvc {

EmbeddedCoreStatus embeddedCoreStatus() noexcept {
    EmbeddedCoreStatus status;
    status.sampleRate = VoiceFormat::SampleRate;
    status.frameDurationMs = VoiceFormat::FrameDurationMs;
    status.frameSamples = static_cast<std::uint32_t>(VoiceFormat::FrameSamples);
    status.voiceClientCompiled = true;
    status.audioDependenciesLinked =
        g_miniaudioLinkProbe != nullptr &&
        g_opusLinkProbe != nullptr &&
        g_speexLinkProbe != nullptr;
    status.iceDependenciesLinked = g_rtcLinkProbe != nullptr;
    return status;
}

void serviceEmbeddedVoiceSession(const EmbeddedVoiceSessionInput& input) noexcept {
    try {
        auto& state = voiceSessionState();
        std::lock_guard<std::mutex> lock(state.mutex);

        const bool active =
            input.localRoomActive &&
            input.roomFound &&
            !input.profileId.empty() &&
            !input.roomInstanceId.empty();

        if (!active) {
            clearVoiceSessionLocked(
                state,
                input.localRoomActive ? "Waiting for resolved RR room" : "Inactive");
            return;
        }

        const bool roomChanged =
            state.identityGeneration != input.identityGeneration ||
            state.profileId != input.profileId ||
            state.roomInstanceId != input.roomInstanceId;

        if (roomChanged) {
            state.signaling.reset();
            state.profileId = input.profileId;
            state.roomInstanceId = input.roomInstanceId;
            state.authorizedPeers.clear();
            state.identityGeneration = input.identityGeneration;
            state.nextReconnect = {};
            state.status = {};
        }

        state.status.lifecycleActive = true;
        state.status.roomInstanceId = input.roomInstanceId;
        state.status.voiceClientRunning = false;
        state.status.authorizedPeerCount =
            static_cast<std::uint32_t>(state.authorizedPeers.size());
        state.status.peerCount = 0;

        const auto now = std::chrono::steady_clock::now();
        if (!state.signaling &&
            (state.nextReconnect == std::chrono::steady_clock::time_point{} ||
             now >= state.nextReconnect)) {
            try {
                state.signaling =
                    std::make_unique<SignalingClient>(std::string(kSignalingUrl));
                state.status.signalingConnected = false;
                state.status.authorizationPending = false;
                state.status.roomAuthorized = false;
                state.status.status = "Connecting embedded signaling...";
            } catch (...) {
                state.nextReconnect = now + kReconnectDelay;
                state.status.signalingConnected = false;
                state.status.authorizationPending = false;
                state.status.roomAuthorized = false;
                state.status.status = "Embedded signaling failed; retrying";
            }
        }

        if (!state.signaling) {
            return;
        }

        state.signaling->service();
        bool reconnect = false;

        for (auto& event : state.signaling->takeEvents()) {
            switch (event.type) {
                case SignalingEventType::Open:
                    state.status.signalingConnected = true;
                    state.status.roomAuthorized = false;
                    if (input.sessionKey.empty() || input.gameName.empty()) {
                        state.status.authorizationPending = false;
                        state.status.status = "Live RR credentials unavailable";
                        break;
                    }
                    try {
                        state.signaling->authenticateRetroRewind(
                            input.profileId,
                            input.sessionKey,
                            input.gameName);
                        state.status.authorizationPending = true;
                        state.status.status = "Authenticating live RR session...";
                    } catch (...) {
                        state.status.authorizationPending = false;
                        state.status.status = "Failed to submit RR authentication";
                    }
                    break;
                case SignalingEventType::RetroRewindStatus: {
                    state.status.authorizationPending = false;
                    const bool authorized=matchesAuthorizedRoom(
                        event.payload,
                        input.profileId,
                        input.roomInstanceId);
                    state.status.roomAuthorized=authorized;
                    if(!authorized) {
                        state.status.voiceRoomAdmissionPending=false;
                        state.status.voiceRoomAdmitted=false;
                        state.status.localMemberId.clear();
                        state.authorizedPeers.clear();
                        state.status.authorizedPeerCount=0;
                        state.status.status="Verified RR room does not match local room instance";
                        break;
                    }

                    if(!state.status.voiceRoomAdmitted &&
                       !state.status.voiceRoomAdmissionPending) {
                        try {
                            state.signaling->admitRetroRewindRoom(input.roomInstanceId);
                            state.status.voiceRoomAdmissionPending=true;
                            state.status.status="RR authorized; requesting voice-room admission...";
                        } catch (...) {
                            state.status.voiceRoomAdmissionPending=false;
                            state.status.status="Failed to request RR voice-room admission";
                        }
                    }
                    break;
                }
                case SignalingEventType::RetroRewindAdmitted: {
                    std::string memberId;
                    std::string admittedRoom;
                    if(!parseAdmission(event.payload,memberId,admittedRoom) ||
                       admittedRoom!=input.roomInstanceId) {
                        state.status.voiceRoomAdmissionPending=false;
                        state.status.voiceRoomAdmitted=false;
                        state.status.localMemberId.clear();
                        state.authorizedPeers.clear();
                        state.status.authorizedPeerCount=0;
                        state.status.status="Malformed or mismatched RR voice-room admission";
                        break;
                    }
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=true;
                    state.status.localMemberId=std::move(memberId);
                    state.status.status="Authorized RR voice room admitted; waiting for peers";
                    break;
                }
                case SignalingEventType::RetroRewindAdmissionFailed:
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.authorizedPeerCount=0;
                    state.status.status=event.payload.empty()
                        ? "RR voice-room admission failed"
                        : "RR voice-room admission failed: "+event.payload;
                    break;
                case SignalingEventType::RetroRewindPeerInfo: {
                    std::string memberId;
                    std::string participantId;
                    if(!state.status.voiceRoomAdmitted ||
                       !parseAuthorizedPeer(
                           event.payload,
                           input.roomInstanceId,
                           memberId,
                           participantId) ||
                       participantId==input.profileId) {
                        break;
                    }
                    state.authorizedPeers[memberId]=participantId;
                    state.status.authorizedPeerCount=
                        static_cast<std::uint32_t>(state.authorizedPeers.size());
                    state.status.status="Authorized RR peer introduced; ICE not started yet";
                    break;
                }
                case SignalingEventType::RetroRewindAuthFailed:
                    state.status.authorizationPending = false;
                    state.status.roomAuthorized = false;
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.authorizedPeerCount=0;
                    state.status.status = event.payload.empty()
                        ? "RR voice admission failed"
                        : "RR admission failed: " + event.payload;
                    break;
                case SignalingEventType::RetroRewindAuthRequired:
                    state.status.authorizationPending = false;
                    state.status.roomAuthorized = false;
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.authorizedPeerCount=0;
                    state.status.status = "RR voice admission expired";
                    break;
                case SignalingEventType::TransportError:
                case SignalingEventType::Closed:
                    state.status.signalingConnected = false;
                    state.status.authorizationPending = false;
                    state.status.roomAuthorized = false;
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.authorizedPeerCount=0;
                    state.status.status = "Embedded signaling disconnected; retrying";
                    reconnect = true;
                    break;
                case SignalingEventType::Error:
                    state.status.authorizationPending = false;
                    state.status.roomAuthorized = false;
                    state.status.voiceRoomAdmissionPending=false;
                    state.status.voiceRoomAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.authorizedPeerCount=0;
                    state.status.status = "Embedded signaling returned an error";
                    break;
                case SignalingEventType::PeerLeft:
                    if(!event.payload.empty()) {
                        state.authorizedPeers.erase(event.payload);
                        state.status.authorizedPeerCount=
                            static_cast<std::uint32_t>(state.authorizedPeers.size());
                    }
                    break;
                default:
                    break;
            }
        }

        if (reconnect) {
            state.signaling.reset();
            state.nextReconnect = now + kReconnectDelay;
        }
    } catch (...) {
    }
}

EmbeddedVoiceSessionStatus embeddedVoiceSessionStatus() {
    auto& state = voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.status;
}

}
