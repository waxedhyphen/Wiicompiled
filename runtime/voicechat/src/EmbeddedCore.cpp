#include "mkwvc/EmbeddedCore.hpp"
#include "mkwvc/IcePeerTransport.hpp"
#include "mkwvc/IceSignal.hpp"
#include "mkwvc/MicrophoneTest.hpp"
#include "mkwvc/SignalingClient.hpp"
#include "mkwvc/VoiceClient.hpp"
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

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

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

struct EmbeddedPeerLink {
    std::string memberId;
    std::unique_ptr<mkwvc::IcePeerTransport> setup;
    std::optional<mkwvc::IceDescriptionSignal> localDescription;
    std::vector<mkwvc::IceCandidateSignal> localCandidates;
    bool signalSent = false;
    bool remoteApplied = false;
    bool offerer = false;
};

struct EmbeddedVoiceSessionState {
    std::mutex mutex;
    std::unique_ptr<mkwvc::SignalingClient> signaling;
    std::unique_ptr<mkwvc::VoiceClient> voiceClient;
    std::unique_ptr<mkwvc::MicrophoneTest> microphoneTestRuntime;
    mkwvc::EmbeddedVoiceSessionStatus status;
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;
    std::string inputDevice;
    std::string outputDevice;
    mkwvc::AudioProcessingSettings audioProcessing{};
    float microphoneGain=1.0f;
    float playbackVolume=1.0f;
    bool microphoneMuted=false;
    bool deafened=false;
    bool pushToTalk=false;
    bool microphoneTest=false;
    std::string controlError;
    std::string profileId;
    std::string roomInstanceId;
    std::vector<std::string> iceServers{"stun:stun.l.google.com:19302"};
    std::unordered_map<std::string,std::string> developmentPeers;
    std::unordered_map<std::string,std::unique_ptr<EmbeddedPeerLink>> peerLinks;
    std::uint64_t identityGeneration = 0;
    std::chrono::steady_clock::time_point nextReconnect{};
};

EmbeddedVoiceSessionState& voiceSessionState() {
    static auto* state = new EmbeddedVoiceSessionState();
    return *state;
}

bool pushToTalkPressed() noexcept {
#if defined(_WIN32)
    return (GetAsyncKeyState('V')&0x8000)!=0;
#else
    return false;
#endif
}

bool pushToTalkHeld(const EmbeddedVoiceSessionState& state) noexcept {
    return !state.pushToTalk || pushToTalkPressed();
}

void stopMicrophoneTestRuntime(EmbeddedVoiceSessionState& state) {
    if(!state.microphoneTestRuntime) return;
    state.microphoneTestRuntime->stop();
    state.microphoneTestRuntime.reset();
}

void startMicrophoneTestRuntime(EmbeddedVoiceSessionState& state) {
    if(state.microphoneTestRuntime || state.voiceClient || !state.microphoneTest) return;
    auto test=std::make_unique<mkwvc::MicrophoneTest>(state.inputDevice,state.outputDevice);
    test->setAudioProcessingSettings(state.audioProcessing);
    test->setMicrophoneGain(state.microphoneGain);
    test->setPlaybackVolume(state.playbackVolume);
    test->setMonitorEnabled(pushToTalkHeld(state));
    test->start();
    state.microphoneTestRuntime=std::move(test);
}

void applyVoiceControls(EmbeddedVoiceSessionState& state) {
    const bool held=pushToTalkHeld(state);
    if(state.voiceClient) {
        stopMicrophoneTestRuntime(state);
        state.voiceClient->setMicrophoneGain(state.microphoneGain);
        state.voiceClient->setAudioProcessingSettings(state.audioProcessing);
        state.voiceClient->setPlaybackVolume(state.playbackVolume);
        state.voiceClient->setTransmitEnabled(!state.microphoneTest && !state.microphoneMuted && !state.deafened && held);
        state.voiceClient->setDeafened(state.deafened || state.microphoneTest);
        state.voiceClient->setMicrophoneTestEnabled(state.microphoneTest && held);
        return;
    }

    if(!state.microphoneTest) {
        stopMicrophoneTestRuntime(state);
        return;
    }

    try {
        startMicrophoneTestRuntime(state);
        if(state.microphoneTestRuntime) state.microphoneTestRuntime->setMonitorEnabled(held);
    } catch(const std::exception& error) {
        stopMicrophoneTestRuntime(state);
        state.microphoneTest=false;
        state.controlError=error.what();
    }
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

std::vector<std::string> parseIceServers(std::string_view payload) {
    std::vector<std::string> servers;
    for (const auto line : splitLines(payload)) {
        if (!line.empty()) {
            servers.emplace_back(line);
        }
    }
    if (servers.empty()) {
        servers.emplace_back("stun:stun.l.google.com:19302");
    }
    return servers;
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

bool parseDevelopmentPeer(
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

void stopVoiceClient(EmbeddedVoiceSessionState& state) {
    if (state.voiceClient) {
        state.voiceClient->stop();
        state.voiceClient.reset();
    }
}

void clearPeerRuntime(EmbeddedVoiceSessionState& state) {
    stopVoiceClient(state);
    state.peerLinks.clear();
    state.developmentPeers.clear();
    state.status.developmentPeerCount=0;
    state.status.peerCount=0;
    state.status.voiceClientRunning=false;
}

void clearVoiceSessionLocked(
    EmbeddedVoiceSessionState& state,
    std::string status) {
    clearPeerRuntime(state);
    state.signaling.reset();
    state.profileId.clear();
    state.roomInstanceId.clear();
    state.iceServers={"stun:stun.l.google.com:19302"};
    state.identityGeneration = 0;
    state.nextReconnect = {};
    state.status = {};
    state.status.status = std::move(status);
}

void removePeer(
    EmbeddedVoiceSessionState& state,
    const std::string& memberId) {
    if (memberId.empty()) return;
    if (state.voiceClient && state.voiceClient->hasPeer(memberId)) {
        state.voiceClient->removePeer(memberId);
    }
    state.peerLinks.erase(memberId);
    state.developmentPeers.erase(memberId);
    state.status.developmentPeerCount=
        static_cast<std::uint32_t>(state.developmentPeers.size());

    if (state.voiceClient && state.voiceClient->peerCount()==0) {
        stopVoiceClient(state);
    }
    state.status.voiceClientRunning=state.voiceClient!=nullptr;
    state.status.peerCount=state.voiceClient
        ? static_cast<std::uint32_t>(state.voiceClient->peerCount())
        : 0;
}

EmbeddedPeerLink& ensurePeerLink(
    EmbeddedVoiceSessionState& state,
    const std::string& memberId,
    bool offerer) {
    const auto existing=state.peerLinks.find(memberId);
    if(existing!=state.peerLinks.end()) return *existing->second;

    auto link=std::make_unique<EmbeddedPeerLink>();
    link->memberId=memberId;
    link->offerer=offerer;
    link->setup=std::make_unique<mkwvc::IcePeerTransport>(
        state.iceServers,
        false);
    if(offerer) {
        link->setup->beginOffer();
    }

    auto* raw=link.get();
    state.peerLinks.emplace(memberId,std::move(link));
    return *raw;
}

void applyRemoteSignal(
    EmbeddedVoiceSessionState& state,
    const mkwvc::SignalingEvent& event) {
    if(!state.status.developmentAdmitted ||
       event.memberId.empty() ||
       !state.developmentPeers.contains(event.memberId)) {
        return;
    }

    const auto bundle=mkwvc::decodeIceSignal(event.payload);
    auto found=state.peerLinks.find(event.memberId);
    if(found==state.peerLinks.end()) {
        if(bundle.description.type!="offer") {
            throw std::runtime_error(
                "Received ICE answer before a development peer offer");
        }
        found=state.peerLinks.emplace(
            event.memberId,
            std::make_unique<EmbeddedPeerLink>()).first;
        auto& link=*found->second;
        link.memberId=event.memberId;
        link.offerer=false;
        link.setup=std::make_unique<mkwvc::IcePeerTransport>(
            state.iceServers,
            false);
    }

    auto& link=*found->second;
    if(!link.setup) return;

    const char* expected=link.offerer ? "answer" : "offer";
    if(bundle.description.type!=expected) {
        throw std::runtime_error(
            std::string("Unexpected development ICE ")+
            bundle.description.type+
            "; expected "+
            expected);
    }

    link.setup->setRemoteDescription(
        bundle.description.sdp,
        bundle.description.type);
    for(const auto& candidate:bundle.candidates) {
        link.setup->addRemoteCandidate(
            candidate.candidate,
            candidate.mid);
    }
    link.remoteApplied=true;
}

void pollPeerLinks(EmbeddedVoiceSessionState& state) {
    if(!state.signaling || !state.status.developmentAdmitted) return;

    std::vector<std::string> failed;
    for(auto& [memberId,holder]:state.peerLinks) {
        auto& link=*holder;
        if(!link.setup) continue;

        try {
            if(auto description=link.setup->takeLocalDescription()) {
                link.localDescription=std::move(*description);
            }

            auto candidates=link.setup->takeLocalCandidates();
            link.localCandidates.insert(
                link.localCandidates.end(),
                std::make_move_iterator(candidates.begin()),
                std::make_move_iterator(candidates.end()));

            if(link.localDescription &&
               link.setup->gatheringComplete() &&
               !link.signalSent) {
                const auto signal=mkwvc::encodeIceSignal({
                    *link.localDescription,
                    link.localCandidates
                });
                state.signaling->sendSignal(memberId,signal);
                link.signalSent=true;
            }

            if(!link.setup->connected() ||
               !link.localDescription ||
               !link.setup->gatheringComplete()) {
                continue;
            }

            if(state.voiceClient &&
               state.voiceClient->hasPeer(memberId)) {
                continue;
            }

            auto transport=
                std::unique_ptr<mkwvc::VoiceTransport>(link.setup.release());

            if(!state.voiceClient) {
                stopMicrophoneTestRuntime(state);
                auto client=std::make_unique<mkwvc::VoiceClient>(
                    memberId,
                    std::move(transport),
                    state.inputDevice,
                    state.outputDevice);
                client->setMicrophoneGain(state.microphoneGain);
                client->setAudioProcessingSettings(state.audioProcessing);
                client->setPlaybackVolume(state.playbackVolume);
                const bool held=pushToTalkHeld(state);
                client->setTransmitEnabled(!state.microphoneTest && !state.microphoneMuted && !state.deafened && held);
                client->setDeafened(state.deafened || state.microphoneTest);
                client->setMicrophoneTestEnabled(state.microphoneTest && held);
                client->start();
                state.voiceClient=std::move(client);
            } else {
                state.voiceClient->addPeer(
                    memberId,
                    std::move(transport));
            }

            state.status.status=
                "UNVERIFIED dev P2P voice connected";
        } catch(const std::exception& error) {
            state.status.status=
                "Development ICE/voice error: "+
                std::string(error.what());
            failed.push_back(memberId);
        }
    }

    for(const auto& memberId:failed) {
        removePeer(state,memberId);
    }

    state.status.voiceClientRunning=state.voiceClient!=nullptr;
    state.status.peerCount=state.voiceClient
        ? static_cast<std::uint32_t>(state.voiceClient->peerCount())
        : 0;
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
                input.localRoomActive
                    ? "Waiting for resolved RR room"
                    : "Inactive");
            applyVoiceControls(state);
            return;
        }

        const bool roomChanged =
            state.identityGeneration != input.identityGeneration ||
            state.profileId != input.profileId ||
            state.roomInstanceId != input.roomInstanceId;

        if (roomChanged) {
            clearPeerRuntime(state);
            state.signaling.reset();
            state.profileId = input.profileId;
            state.roomInstanceId = input.roomInstanceId;
            state.iceServers={"stun:stun.l.google.com:19302"};
            state.identityGeneration = input.identityGeneration;
            state.nextReconnect = {};
            state.status = {};
        }

        state.status.lifecycleActive = true;
        state.status.roomInstanceId = input.roomInstanceId;
        state.status.developmentPeerCount =
            static_cast<std::uint32_t>(state.developmentPeers.size());
        state.status.voiceClientRunning=state.voiceClient!=nullptr;
        state.status.peerCount=state.voiceClient
            ? static_cast<std::uint32_t>(state.voiceClient->peerCount())
            : 0;

        const auto now = std::chrono::steady_clock::now();
        if (!state.signaling &&
            (state.nextReconnect == std::chrono::steady_clock::time_point{} ||
             now >= state.nextReconnect)) {
            try {
                state.signaling =
                    std::make_unique<SignalingClient>(
                        std::string(kSignalingUrl));
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
                state.status.status =
                    "Embedded signaling failed; retrying";
            }
        }

        if (!state.signaling) {
            applyVoiceControls(state);
            return;
        }

        state.signaling->service();
        bool reconnect = false;

        for (auto& event : state.signaling->takeEvents()) {
            switch (event.type) {
                case SignalingEventType::Open:
                    clearPeerRuntime(state);
                    state.status.signalingConnected=true;
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    try {
                        state.signaling->admitRetroRewindDevelopment(
                            input.profileId,
                            input.roomInstanceId);
                        state.status.developmentAdmissionPending=true;
                        state.status.status=
                            "Requesting UNVERIFIED public-roster admission...";
                    } catch (...) {
                        state.status.developmentAdmissionPending=false;
                        state.status.status=
                            "Failed to request development room admission";
                    }
                    break;
                case SignalingEventType::IceServers:
                    state.iceServers=parseIceServers(event.payload);
                    break;
                case SignalingEventType::RetroRewindDevelopmentAdmitted: {
                    std::string memberId;
                    std::string admittedRoom;
                    if(!parseAdmission(
                           event.payload,
                           memberId,
                           admittedRoom) ||
                       admittedRoom!=input.roomInstanceId) {
                        clearPeerRuntime(state);
                        state.status.developmentAdmissionPending=false;
                        state.status.developmentAdmitted=false;
                        state.status.localMemberId.clear();
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
                    clearPeerRuntime(state);
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.status.status=event.payload.empty()
                        ? "Development room admission failed"
                        : "Development admission failed: "+event.payload;
                    break;
                case SignalingEventType::RetroRewindDevelopmentPeerInfo: {
                    std::string memberId;
                    std::string participantId;
                    if(!state.status.developmentAdmitted ||
                       !parseDevelopmentPeer(
                           event.payload,
                           input.roomInstanceId,
                           memberId,
                           participantId) ||
                       participantId==input.profileId) {
                        break;
                    }
                    state.developmentPeers[memberId]=participantId;
                    state.status.developmentPeerCount=
                        static_cast<std::uint32_t>(
                            state.developmentPeers.size());
                    state.status.status=
                        "UNVERIFIED dev peer introduced; starting ICE";
                    break;
                }
                case SignalingEventType::PeerReady:
                    if(!state.status.developmentAdmitted ||
                       event.memberId.empty() ||
                       !state.developmentPeers.contains(event.memberId) ||
                       state.status.localMemberId.empty()) {
                        break;
                    }
                    if(!state.peerLinks.contains(event.memberId) &&
                       (!state.voiceClient ||
                        !state.voiceClient->hasPeer(event.memberId))) {
                        const bool offerer=
                            state.status.localMemberId<event.memberId;
                        ensurePeerLink(
                            state,
                            event.memberId,
                            offerer);
                    }
                    break;
                case SignalingEventType::Signal:
                    applyRemoteSignal(state,event);
                    break;
                case SignalingEventType::PeerLeft:
                    removePeer(state,event.payload);
                    if(state.status.developmentAdmitted) {
                        state.status.status=
                            "UNVERIFIED dev peer left";
                    }
                    break;
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
                    clearPeerRuntime(state);
                    state.status.signalingConnected=false;
                    state.status.productionAuthorizationPending=false;
                    state.status.productionAuthorized=false;
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.status.status=
                        "Embedded signaling disconnected; retrying";
                    reconnect=true;
                    break;
                case SignalingEventType::Error:
                    clearPeerRuntime(state);
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=false;
                    state.status.localMemberId.clear();
                    state.status.status=
                        "Embedded signaling returned an error";
                    break;
                default:
                    break;
            }
        }

        if (reconnect) {
            state.signaling.reset();
            state.nextReconnect = now + kReconnectDelay;
            applyVoiceControls(state);
            return;
        }

        pollPeerLinks(state);
        applyVoiceControls(state);
    } catch (const std::exception& error) {
        auto& state = voiceSessionState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status.status=
            "Embedded voice runtime error: "+
            std::string(error.what());
    } catch (...) {
    }
}

EmbeddedVoiceSessionStatus embeddedVoiceSessionStatus() {
    auto& state = voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.status;
}

EmbeddedVoiceControls embeddedVoiceControls() {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);

    EmbeddedVoiceControls controls;
    controls.inputDevices=state.inputDevices;
    controls.outputDevices=state.outputDevices;
    controls.inputDevice=state.inputDevice;
    controls.outputDevice=state.outputDevice;
    controls.automaticNormalization=state.audioProcessing.normalization;
    controls.noiseSuppression=state.audioProcessing.noiseSuppression;
    controls.noiseSuppressionStrength=state.audioProcessing.noiseSuppressionStrength;
    controls.microphoneGain=state.microphoneGain;
    controls.playbackVolume=state.playbackVolume;
    controls.microphoneMuted=state.microphoneMuted;
    controls.deafened=state.deafened;
    controls.pushToTalk=state.pushToTalk;
    controls.microphoneTest=state.microphoneTest;
    controls.pushToTalkHeld=pushToTalkHeld(state);
    controls.error=state.controlError;

    if(state.voiceClient) {
        const auto stats=state.voiceClient->stats();
        controls.micPeak=stats.micPeak;
        controls.playbackPeak=stats.playbackPeak;
        for(const auto& peer:state.voiceClient->peerStats()) {
            EmbeddedVoicePeerControl control;
            control.memberId=peer.memberId;
            if(const auto found=state.developmentPeers.find(peer.memberId);found!=state.developmentPeers.end()) {
                control.participantId=found->second;
            }
            control.volume=peer.volume;
            controls.peers.push_back(std::move(control));
        }
    } else if(state.microphoneTestRuntime) {
        controls.micPeak=state.microphoneTestRuntime->micPeak();
    }

    return controls;
}

void refreshEmbeddedVoiceDevices() {
    auto inputs=AudioEngine::captureDevices();
    auto outputs=AudioEngine::playbackDevices();
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.inputDevices=std::move(inputs);
    state.outputDevices=std::move(outputs);
}

void setEmbeddedVoiceInputDevice(std::string device) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(device==state.inputDevice) return;
    try {
        if(state.voiceClient) state.voiceClient->setCaptureDevice(device);
        if(state.microphoneTestRuntime) state.microphoneTestRuntime->setCaptureDevice(device);
        state.inputDevice=std::move(device);
        state.controlError.clear();
    } catch(const std::exception& error) {
        state.controlError=error.what();
    }
}

void setEmbeddedVoiceOutputDevice(std::string device) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(device==state.outputDevice) return;
    try {
        if(state.voiceClient) state.voiceClient->setPlaybackDevice(device);
        if(state.microphoneTestRuntime) state.microphoneTestRuntime->setPlaybackDevice(device);
        state.outputDevice=std::move(device);
        state.controlError.clear();
    } catch(const std::exception& error) {
        state.controlError=error.what();
    }
}

void setEmbeddedVoiceProcessing(bool normalization,bool noiseSuppression,int noiseSuppressionStrength) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.audioProcessing.normalization=normalization;
    state.audioProcessing.noiseSuppression=noiseSuppression;
    state.audioProcessing.noiseSuppressionStrength=std::clamp(noiseSuppressionStrength,0,100);
    if(state.voiceClient) state.voiceClient->setAudioProcessingSettings(state.audioProcessing);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setAudioProcessingSettings(state.audioProcessing);
    state.controlError.clear();
}

void setEmbeddedVoiceMicrophoneGain(float gain) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.microphoneGain=std::clamp(gain,0.25f,5.0f);
    if(state.voiceClient) state.voiceClient->setMicrophoneGain(state.microphoneGain);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setMicrophoneGain(state.microphoneGain);
    state.controlError.clear();
}

void setEmbeddedVoicePlaybackVolume(float volume) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.playbackVolume=std::clamp(volume,0.0f,3.0f);
    if(state.voiceClient) state.voiceClient->setPlaybackVolume(state.playbackVolume);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setPlaybackVolume(state.playbackVolume);
    state.controlError.clear();
}

void setEmbeddedVoiceMicrophoneMuted(bool muted) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.microphoneMuted=muted;
    applyVoiceControls(state);
}

void setEmbeddedVoiceDeafened(bool deafened) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.deafened=deafened;
    applyVoiceControls(state);
}

void setEmbeddedVoicePushToTalk(bool enabled) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pushToTalk=enabled;
    if(enabled) state.microphoneMuted=false;
    applyVoiceControls(state);
}

void setEmbeddedVoiceMicrophoneTest(bool enabled) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.microphoneTest=enabled;
    state.controlError.clear();
    applyVoiceControls(state);
}

void setEmbeddedVoicePeerVolume(const std::string& memberId,float volume) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(state.voiceClient) state.voiceClient->setRemoteVolume(memberId,std::clamp(volume,0.0f,3.0f));
}

}
