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
#include <utility>

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
    state.nextReconnect = {};
    state.status = {};
    state.status.status = std::move(status);
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
            state.profileId != input.profileId ||
            state.roomInstanceId != input.roomInstanceId;

        if (roomChanged) {
            state.signaling.reset();
            state.profileId = input.profileId;
            state.roomInstanceId = input.roomInstanceId;
            state.nextReconnect = {};
            state.status = {};
        }

        state.status.lifecycleActive = true;
        state.status.roomAuthorized = input.roomAuthorized;
        state.status.roomInstanceId = input.roomInstanceId;
        state.status.voiceClientRunning = false;
        state.status.peerCount = 0;

        const auto now = std::chrono::steady_clock::now();
        if (!state.signaling &&
            (state.nextReconnect == std::chrono::steady_clock::time_point{} ||
             now >= state.nextReconnect)) {
            try {
                state.signaling =
                    std::make_unique<SignalingClient>(std::string(kSignalingUrl));
                state.status.signalingConnected = false;
                state.status.status = "Connecting embedded signaling...";
            } catch (...) {
                state.nextReconnect = now + kReconnectDelay;
                state.status.signalingConnected = false;
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
                    state.status.status = input.roomAuthorized
                        ? "RR admission authorized; peer orchestration pending"
                        : "Core signaling connected; verified RR admission required";
                    break;
                case SignalingEventType::TransportError:
                case SignalingEventType::Closed:
                    state.status.signalingConnected = false;
                    state.status.status = "Embedded signaling disconnected; retrying";
                    reconnect = true;
                    break;
                case SignalingEventType::Error:
                    state.status.status = "Embedded signaling returned an error";
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
