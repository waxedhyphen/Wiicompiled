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
        state.status.developmentPeerCount =
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
                state.status.productionAuthorizationPending = false;
                state.status.productionAuthorized = false;
                state.status.developmentAdmissionPending = false;
                state.status.developmentAdmitted = false;
                state.status.status = "Connecting embedded signaling...";
            } catch (...) {
                state.nextReconnect = now + kReconnectDelay;
                state.status.signalingConnected = false;
                state.status.productionAuthorizationPending = false;
                state.status.productionAuthorized = false;
                state.status.developmentAdmissionPending = false;
                state.status.developmentAdmitted = false;
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
                    state.status.signalingConnected=true;
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=false;
                    state.status.developmentAdmitted=false;
                    state.authorizedPeers.clear();
                    state.status.developmentPeerCount=0;
                    try {
                        state.signaling->admitRetroRewindDevelopment(
                            input.profileId,
                            input.roomInstanceId);
                        state.status.developmentAdmissionPending=true;
                        state.status.status=
                            "Requesting unverified public-roster development admission...";
                    } catch (...) {
                        state.status.developmentAdmissionPending=false;
                        state.status.status=
                            "Failed to request development room admission";
                    }
                    break;
                case SignalingEventType::RetroRewindDevelopmentAdmitted: {
                    std::string memberId;
                    std::string admittedRoom;
                    if(!parseAdmission(event.payload,memberId,admittedRoom) ||
                       admittedRoom!=input.roomInstanceId) {
                        state.status.developmentAdmissionPending=false;
                        state.status.developmentAdmitted=false;
                        state.status.localMemberId.clear();
                        state.authorizedPeers.clear();
                        state.status.developmentPeerCount=0;
                        state.status.status=
                            "Malformed or mismatched development admission";
                        break;
                    }
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=true;
                    state.status.localMemberId=std::move(memberId);
                    state.status.status=
                        "UNVERIFIED dev room admitted; waiting for peers";
                    break;
                }
                case SignalingEventType::RetroRewindDevelopmentAdmissionFailed:
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.developmentPeerCount=0;
                    state.status.status=event.payload.empty()
                        ? "Development room admission failed"
                        : "Development admission failed: "+event.payload;
                    break;
                case SignalingEventType::RetroRewindDevelopmentPeerInfo: {
                    std::string memberId;
                    std::string participantId;
                    if(!state.status.developmentAdmitted ||
                       !parseAuthorizedPeer(
                           event.payload,
                           input.roomInstanceId,
                           memberId,
                           participantId) ||
                       participantId==input.profileId) {
                        break;
                    }
                    state.authorizedPeers[memberId]=participantId;
                    state.status.developmentPeerCount=
                        static_cast<std::uint32_t>(state.authorizedPeers.size());
                    state.status.status=
                        "UNVERIFIED dev peer introduced; ICE not started yet";
                    break;
                }
                case SignalingEventType::RetroRewindStatus:
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=matchesAuthorizedRoom(
                        event.payload,
                        input.profileId,
                        input.roomInstanceId);
                    break;
                case SignalingEventType::RetroRewindAuthFailed:
                case SignalingEventType::RetroRewindAuthRequired:
                case SignalingEventType::RetroRewindAdmissionFailed:
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=false;
                    break;
                case SignalingEventType::RetroRewindAdmitted:
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=true;
                    break;
                case SignalingEventType::TransportError:
                case SignalingEventType::Closed:
                    state.status.signalingConnected=false;
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=false;
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.developmentPeerCount=0;
                    state.status.status="Embedded signaling disconnected; retrying";
                    reconnect=true;
                    break;
                case SignalingEventType::Error:
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.authorizedPeers.clear();
                    state.status.developmentPeerCount=0;
                    state.status.status="Embedded signaling returned an error";
                    break;
                case SignalingEventType::PeerLeft:
                    if(!event.payload.empty()) {
                        state.authorizedPeers.erase(event.payload);
                        state.status.developmentPeerCount=
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
