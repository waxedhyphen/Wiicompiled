#include "mkwvc/EmbeddedCore.hpp"
#include "mkwvc/IcePeerTransport.hpp"
#include "mkwvc/IceSignal.hpp"
#include "mkwvc/MicrophoneTest.hpp"
#include "mkwvc/SignalingClient.hpp"
#include "mkwvc/VoiceClient.hpp"
#include "mkwvc/VoiceFormat.hpp"
#include "runtime_config.h"

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
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

struct EmbeddedPeerMetadata {
    std::string participantId;
    std::string displayName;
    std::string friendCode;
};

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
    bool enabled=false;
    bool microphoneMuted=false;
    bool deafened=false;
    bool pushToTalk=false;
    bool pushToMute=false;
    bool voiceActivation=false;
    int voiceActivationThreshold=25;
    bool microphoneTest=false;
    bool pushToTalkHeldInput=false;
    bool pushToMuteHeldInput=false;
    mkwvc::EmbeddedVoiceBinding pushToTalkBinding{};
    mkwvc::EmbeddedVoiceBinding pushToMuteBinding{};
    mkwvc::EmbeddedVoiceBinding muteBinding{};
    mkwvc::EmbeddedVoiceBinding deafenBinding{};
    std::string controlError;
    std::string profileId;
    std::string roomInstanceId;
    std::vector<std::string> iceServers{"stun:stun.l.google.com:19302"};
    std::unordered_map<std::string,EmbeddedPeerMetadata> developmentPeers;
    std::unordered_map<std::string,float> savedPeerVolumes;
    std::unordered_map<std::string,std::unique_ptr<EmbeddedPeerLink>> peerLinks;
    bool settingsLoaded=false;
    std::uint64_t identityGeneration = 0;
    std::chrono::steady_clock::time_point nextReconnect{};
};

EmbeddedVoiceSessionState& voiceSessionState() {
    static auto* state = new EmbeddedVoiceSessionState();
    return *state;
}

std::string formatFloat(float value) {
    std::ostringstream out;
    out<<value;
    return out.str();
}

void persistString(std::string_view key,const std::string& value) {
    RuntimeConfigFile::WriteSetting("voicechat",key,RuntimeConfigFile::FormatString(value));
}

void persistFloat(std::string_view key,float value) {
    RuntimeConfigFile::WriteSetting("voicechat",key,formatFloat(value));
}

void persistBool(std::string_view key,bool value) {
    RuntimeConfigFile::WriteSetting("voicechat",key,value ? "true" : "false");
}

void persistInt(std::string_view key,int value) {
    RuntimeConfigFile::WriteSetting("voicechat",key,std::to_string(value));
}

void loadSettingsLocked(EmbeddedVoiceSessionState& state) {
    if(state.settingsLoaded) return;
    state.settingsLoaded=true;

    try {
        const auto path=RuntimeConfigFile::ResolveConfigPath();
        std::ifstream input(path,std::ios::binary);
        if(input) {
            const auto document=toml::parse(input,RuntimeConfigFile::PathToUtf8(path));
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","enabled")) state.enabled=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","input_device")) state.inputDevice=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","output_device")) state.outputDevice=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","normalization")) state.audioProcessing.normalization=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","noise_suppression")) state.audioProcessing.noiseSuppression=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","noise_strength")) state.audioProcessing.noiseSuppressionStrength=std::clamp(*value,0,100);
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","noise_gate")) state.audioProcessing.noiseGate=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","noise_gate_threshold")) state.audioProcessing.noiseGateThreshold=std::clamp(*value,0,100);
            if(const auto value=RuntimeConfigFile::FindConfigFloat(document,"voicechat","microphone_boost")) state.audioProcessing.microphoneBoost=std::clamp(*value,1.0f,3.0f);
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","compressor")) state.audioProcessing.compressor=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","compressor_strength")) state.audioProcessing.compressorStrength=std::clamp(*value,0,100);
            if(const auto value=RuntimeConfigFile::FindConfigFloat(document,"voicechat","microphone_gain")) state.microphoneGain=std::clamp(*value,0.25f,5.0f);
            if(const auto value=RuntimeConfigFile::FindConfigFloat(document,"voicechat","playback_volume")) state.playbackVolume=std::clamp(*value,0.0f,3.0f);
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","muted")) state.microphoneMuted=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","deafened")) state.deafened=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","push_to_talk")) state.pushToTalk=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","push_to_mute")) state.pushToMute=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<bool>(document,"voicechat","voice_activation")) state.voiceActivation=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","voice_activation_threshold")) state.voiceActivationThreshold=std::clamp(*value,1,100);

            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","ptt_key")) state.pushToTalkBinding.keyboard=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","ptt_controller")) state.pushToTalkBinding.controller=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","push_to_mute_key")) state.pushToMuteBinding.keyboard=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","push_to_mute_controller")) state.pushToMuteBinding.controller=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","mute_key")) state.muteBinding.keyboard=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","mute_controller")) state.muteBinding.controller=*value;
            if(const auto value=RuntimeConfigFile::FindConfigInt(document,"voicechat","deafen_key")) state.deafenBinding.keyboard=*value;
            if(const auto value=RuntimeConfigFile::FindConfigValue<std::string>(document,"voicechat","deafen_controller")) state.deafenBinding.controller=*value;

            if(document.contains("voicechat") && document.at("voicechat").is_table()) {
                for(const auto& [key,value]:document.at("voicechat").as_table()) {
                    (void)value;
                    constexpr std::string_view prefix="peer_volume_";
                    if(!key.starts_with(prefix)) continue;
                    const std::string participantId=key.substr(prefix.size());
                    if(participantId.empty()) continue;
                    if(const auto volume=RuntimeConfigFile::FindConfigFloat(document,"voicechat",key)) {
                        state.savedPeerVolumes[participantId]=std::clamp(*volume,0.0f,3.0f);
                    }
                }
            }
        }

        if(state.pushToTalk && state.voiceActivation) state.voiceActivation=false;
        if(state.enabled) {
            state.inputDevices=mkwvc::AudioEngine::captureDevices();
            state.outputDevices=mkwvc::AudioEngine::playbackDevices();
            if(!state.inputDevice.empty() &&
               std::find(state.inputDevices.begin(),state.inputDevices.end(),state.inputDevice)==state.inputDevices.end()) {
                state.inputDevice.clear();
            }
            if(!state.outputDevice.empty() &&
               std::find(state.outputDevices.begin(),state.outputDevices.end(),state.outputDevice)==state.outputDevices.end()) {
                state.outputDevice.clear();
            }
        }
    } catch(...) {
    }
}

std::string decodeHex(std::string_view value) {
    if(value.size()%2!=0) return {};
    std::string result;
    result.reserve(value.size()/2);
    const auto nibble=[](char ch)->int {
        if(ch>='0' && ch<='9') return ch-'0';
        if(ch>='A' && ch<='F') return ch-'A'+10;
        if(ch>='a' && ch<='f') return ch-'a'+10;
        return -1;
    };
    for(std::size_t i=0;i<value.size();i+=2) {
        const int hi=nibble(value[i]);
        const int lo=nibble(value[i+1]);
        if(hi<0 || lo<0) return {};
        result.push_back(static_cast<char>((hi<<4)|lo));
    }
    return result;
}

mkwvc::EmbeddedVoiceRoomPlayer* roomPlayer(
    EmbeddedVoiceSessionState& state,
    const std::string& participantId) {
    for(auto& player:state.status.roomPlayers) {
        if(player.profileId==participantId) return &player;
    }
    return nullptr;
}

bool pushToTalkHeld(const EmbeddedVoiceSessionState& state) noexcept {
    return !state.pushToTalk || state.pushToTalkHeldInput;
}

bool pushToMuteHeld(const EmbeddedVoiceSessionState& state) noexcept {
    return state.pushToMute && state.pushToMuteHeldInput;
}

float voiceActivationThreshold(const EmbeddedVoiceSessionState& state) noexcept {
    const float normalized=static_cast<float>(std::clamp(state.voiceActivationThreshold,1,100))/100.0f;
    return 0.005f+normalized*0.195f;
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
    if(!state.enabled) {
        stopMicrophoneTestRuntime(state);
        return;
    }

    const bool held=pushToTalkHeld(state);
    const bool pushMuted=pushToMuteHeld(state);
    if(state.voiceClient) {
        stopMicrophoneTestRuntime(state);
        state.voiceClient->setMicrophoneGain(state.microphoneGain);
        state.voiceClient->setAudioProcessingSettings(state.audioProcessing);
        state.voiceClient->setPlaybackVolume(state.playbackVolume);
        state.voiceClient->setLocalStatus(state.microphoneMuted,state.deafened);
        state.voiceClient->setVoiceActivation(state.voiceActivation,voiceActivationThreshold(state));
        state.voiceClient->setTransmitEnabled(
            !state.microphoneTest &&
            !state.microphoneMuted &&
            !state.deafened &&
            held &&
            !pushMuted);
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
    std::string_view expectedProfileId,
    std::string& memberId,
    std::string& roomInstanceId,
    std::string& roomId,
    std::string& created,
    std::vector<mkwvc::EmbeddedVoiceRoomPlayer>& players) {
    const auto lines=splitLines(payload);
    if(lines.size()<5 || lines[0].size()!=32 || lines[1].size()!=64) return false;

    std::size_t count=0;
    const auto parsed=std::from_chars(lines[4].data(),lines[4].data()+lines[4].size(),count);
    if(parsed.ec!=std::errc{} || parsed.ptr!=lines[4].data()+lines[4].size() || count>12 || lines.size()<5+count) return false;

    memberId=std::string(lines[0]);
    roomInstanceId=std::string(lines[1]);
    roomId=std::string(lines[2]);
    created=std::string(lines[3]);
    players.clear();
    players.reserve(count);

    bool localFound=false;
    for(std::size_t i=0;i<count;++i) {
        const std::string_view line=lines[5+i];
        const auto first=line.find('\t');
        const auto second=first==std::string_view::npos ? std::string_view::npos : line.find('\t',first+1);
        const auto third=second==std::string_view::npos ? std::string_view::npos : line.find('\t',second+1);
        if(first==std::string_view::npos || second==std::string_view::npos) return false;

        mkwvc::EmbeddedVoiceRoomPlayer player;
        player.profileId=std::string(line.substr(0,first));
        player.voiceChat=line.substr(first+1,second-first-1)=="1";
        player.displayName=decodeHex(third==std::string_view::npos
            ? line.substr(second+1)
            : line.substr(second+1,third-second-1));
        if(player.displayName.empty()) player.displayName="Player";
        if(third!=std::string_view::npos) player.friendCode=std::string(line.substr(third+1));
        if(player.profileId==expectedProfileId) localFound=true;
        if(player.profileId.empty()) return false;
        players.push_back(std::move(player));
    }

    return localFound && !roomId.empty() && !created.empty();
}

bool parseDevelopmentPeer(
    std::string_view payload,
    std::string_view expectedRoomInstanceId,
    std::string& memberId,
    EmbeddedPeerMetadata& metadata) {
    const auto lines=splitLines(payload);
    if(lines.size()<5 ||
       lines[0].size()!=32 ||
       lines[1].empty() ||
       lines[2]!=expectedRoomInstanceId) {
        return false;
    }

    memberId=std::string(lines[0]);
    metadata.participantId=std::string(lines[1]);
    metadata.displayName=decodeHex(lines[4]);
    if(metadata.displayName.empty()) metadata.displayName="Player";
    if(lines.size()>=6) metadata.friendCode=std::string(lines[5]);
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
    for(auto& player:state.status.roomPlayers) player.voiceChat=false;
    state.status.developmentPeerCount=0;
    state.status.peerCount=0;
    state.status.voiceClientRunning=false;
}

void clearVoiceSessionLocked(
    EmbeddedVoiceSessionState& state,
    std::string status) {
    stopMicrophoneTestRuntime(state);
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
    if(const auto found=state.developmentPeers.find(memberId);found!=state.developmentPeers.end()) {
        if(auto* player=roomPlayer(state,found->second.participantId)) player->voiceChat=false;
        state.developmentPeers.erase(found);
    }
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
                const bool pushMuted=pushToMuteHeld(state);
                client->setLocalStatus(state.microphoneMuted,state.deafened);
                client->setVoiceActivation(state.voiceActivation,voiceActivationThreshold(state));
                client->setTransmitEnabled(!state.microphoneTest && !state.microphoneMuted && !state.deafened && held && !pushMuted);
                client->setDeafened(state.deafened || state.microphoneTest);
                client->setMicrophoneTestEnabled(state.microphoneTest && held);
                client->start();
                state.voiceClient=std::move(client);
            } else {
                state.voiceClient->addPeer(
                    memberId,
                    std::move(transport));
            }

            if(const auto metadata=state.developmentPeers.find(memberId);metadata!=state.developmentPeers.end()) {
                if(const auto saved=state.savedPeerVolumes.find(metadata->second.participantId);saved!=state.savedPeerVolumes.end()) {
                    state.voiceClient->setRemoteVolume(memberId,saved->second);
                }
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
        loadSettingsLocked(state);

        if(!state.enabled) {
            if(state.signaling || state.voiceClient || state.microphoneTestRuntime || state.status.lifecycleActive) {
                clearVoiceSessionLocked(state,"Disabled");
            } else {
                state.status.status="Disabled";
            }
            return;
        }

        const bool active =
            input.localRoomActive &&
            !input.profileId.empty();

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
            state.profileId != input.profileId;

        if (roomChanged) {
            clearPeerRuntime(state);
            state.signaling.reset();
            state.profileId = input.profileId;
            state.roomInstanceId.clear();
            state.iceServers={"stun:stun.l.google.com:19302"};
            state.identityGeneration = input.identityGeneration;
            state.nextReconnect = {};
            state.status = {};
        }

        state.status.lifecycleActive = true;
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
                            input.profileId);
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
                    std::string roomId;
                    std::string created;
                    std::vector<EmbeddedVoiceRoomPlayer> players;
                    if(!parseAdmission(
                           event.payload,
                           input.profileId,
                           memberId,
                           admittedRoom,
                           roomId,
                           created,
                           players)) {
                        clearPeerRuntime(state);
                        state.status.developmentAdmissionPending=false;
                        state.status.developmentAdmitted=false;
                        state.status.localMemberId.clear();
                        state.status.status=
                            "Malformed development room admission";
                        break;
                    }

                    state.roomInstanceId=admittedRoom;
                    state.status.developmentAdmissionPending=false;
                    state.status.developmentAdmitted=true;
                    state.status.roomFound=true;
                    state.status.localMemberId=std::move(memberId);
                    state.status.roomInstanceId=admittedRoom;
                    state.status.roomId=std::move(roomId);
                    state.status.roomCreated=std::move(created);
                    state.status.roomPlayers=std::move(players);
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
                    EmbeddedPeerMetadata metadata;
                    if(!state.status.developmentAdmitted ||
                       state.roomInstanceId.empty() ||
                       !parseDevelopmentPeer(
                           event.payload,
                           state.roomInstanceId,
                           memberId,
                           metadata) ||
                       metadata.participantId==input.profileId) {
                        break;
                    }

                    if(auto* player=roomPlayer(state,metadata.participantId)) {
                        player->voiceChat=true;
                        if(!metadata.displayName.empty()) player->displayName=metadata.displayName;
                        if(!metadata.friendCode.empty()) player->friendCode=metadata.friendCode;
                    }
                    state.developmentPeers[memberId]=std::move(metadata);
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
                    state.status.productionAuthorized=
                        !state.roomInstanceId.empty() &&
                        matchesAuthorizedRoom(
                            event.payload,
                            input.profileId,
                            state.roomInstanceId);
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
    loadSettingsLocked(state);

    EmbeddedVoiceControls controls;
    controls.enabled=state.enabled;
    controls.inputDevices=state.inputDevices;
    controls.outputDevices=state.outputDevices;
    controls.inputDevice=state.inputDevice;
    controls.outputDevice=state.outputDevice;
    controls.automaticNormalization=state.audioProcessing.normalization;
    controls.noiseSuppression=state.audioProcessing.noiseSuppression;
    controls.noiseSuppressionStrength=state.audioProcessing.noiseSuppressionStrength;
    controls.noiseGate=state.audioProcessing.noiseGate;
    controls.noiseGateThreshold=state.audioProcessing.noiseGateThreshold;
    controls.microphoneBoost=state.audioProcessing.microphoneBoost;
    controls.compressor=state.audioProcessing.compressor;
    controls.compressorStrength=state.audioProcessing.compressorStrength;
    controls.microphoneGain=state.microphoneGain;
    controls.playbackVolume=state.playbackVolume;
    controls.microphoneMuted=state.microphoneMuted;
    controls.deafened=state.deafened;
    controls.pushToTalk=state.pushToTalk;
    controls.pushToMute=state.pushToMute;
    controls.voiceActivation=state.voiceActivation;
    controls.voiceActivationThreshold=state.voiceActivationThreshold;
    controls.microphoneTest=state.microphoneTest;
    controls.pushToTalkHeld=state.pushToTalkHeldInput;
    controls.pushToMuteHeld=pushToMuteHeld(state);
    controls.pushToTalkBinding=state.pushToTalkBinding;
    controls.pushToMuteBinding=state.pushToMuteBinding;
    controls.muteBinding=state.muteBinding;
    controls.deafenBinding=state.deafenBinding;
    controls.error=state.controlError;

    if(state.voiceClient) {
        const auto stats=state.voiceClient->stats();
        controls.micPeak=stats.micPeak;
        controls.playbackPeak=stats.playbackPeak;
        controls.localSpeaking=stats.transmitting;
        for(const auto& peer:state.voiceClient->peerStats()) {
            EmbeddedVoicePeerControl control;
            control.memberId=peer.memberId;
            if(const auto found=state.developmentPeers.find(peer.memberId);found!=state.developmentPeers.end()) {
                control.participantId=found->second.participantId;
                control.displayName=found->second.displayName;
                control.friendCode=found->second.friendCode;
            }
            control.volume=peer.volume;
            control.voicePeak=peer.voicePeak;
            control.speaking=peer.speaking;
            control.remoteMuted=peer.remoteMuted;
            control.remoteDeafened=peer.remoteDeafened;
            controls.peers.push_back(std::move(control));
        }
    } else if(state.microphoneTestRuntime) {
        controls.micPeak=state.microphoneTestRuntime->micPeak();
    }

    return controls;
}

void setEmbeddedVoiceEnabled(bool enabled) {
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    if(enabled) {
        inputs=AudioEngine::captureDevices();
        outputs=AudioEngine::playbackDevices();
    }

    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    loadSettingsLocked(state);
    if(state.enabled==enabled) return;

    state.enabled=enabled;
    persistBool("enabled",enabled);
    state.controlError.clear();

    if(!enabled) {
        state.microphoneTest=false;
        state.pushToTalkHeldInput=false;
        state.pushToMuteHeldInput=false;
        clearVoiceSessionLocked(state,"Disabled");
        return;
    }

    state.inputDevices=std::move(inputs);
    state.outputDevices=std::move(outputs);
    if(!state.inputDevice.empty() &&
       std::find(state.inputDevices.begin(),state.inputDevices.end(),state.inputDevice)==state.inputDevices.end()) {
        state.inputDevice.clear();
        persistString("input_device",{});
    }
    if(!state.outputDevice.empty() &&
       std::find(state.outputDevices.begin(),state.outputDevices.end(),state.outputDevice)==state.outputDevices.end()) {
        state.outputDevice.clear();
        persistString("output_device",{});
    }
    state.status.status="Enabled; waiting for Retro Rewind room";
}

void refreshEmbeddedVoiceDevices() {
    auto inputs=AudioEngine::captureDevices();
    auto outputs=AudioEngine::playbackDevices();
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    loadSettingsLocked(state);
    if(!state.enabled) return;
    state.inputDevices=std::move(inputs);
    state.outputDevices=std::move(outputs);
    if(!state.inputDevice.empty() &&
       std::find(state.inputDevices.begin(),state.inputDevices.end(),state.inputDevice)==state.inputDevices.end()) {
        state.inputDevice.clear();
        persistString("input_device",{});
    }
    if(!state.outputDevice.empty() &&
       std::find(state.outputDevices.begin(),state.outputDevices.end(),state.outputDevice)==state.outputDevices.end()) {
        state.outputDevice.clear();
        persistString("output_device",{});
    }
}

void setEmbeddedVoiceInputDevice(std::string device) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(!state.enabled || device==state.inputDevice) return;
    try {
        if(state.voiceClient) state.voiceClient->setCaptureDevice(device);
        if(state.microphoneTestRuntime) state.microphoneTestRuntime->setCaptureDevice(device);
        state.inputDevice=std::move(device);
        persistString("input_device",state.inputDevice);
        state.controlError.clear();
    } catch(const std::exception& error) {
        state.controlError=error.what();
    }
}

void setEmbeddedVoiceOutputDevice(std::string device) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(!state.enabled || device==state.outputDevice) return;
    try {
        if(state.voiceClient) state.voiceClient->setPlaybackDevice(device);
        if(state.microphoneTestRuntime) state.microphoneTestRuntime->setPlaybackDevice(device);
        state.outputDevice=std::move(device);
        persistString("output_device",state.outputDevice);
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
    persistBool("normalization",state.audioProcessing.normalization);
    persistBool("noise_suppression",state.audioProcessing.noiseSuppression);
    persistInt("noise_strength",state.audioProcessing.noiseSuppressionStrength);
    state.controlError.clear();
}

void setEmbeddedVoiceAdvancedProcessing(bool noiseGate,int noiseGateThreshold,float microphoneBoost,bool compressor,int compressorStrength) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.audioProcessing.noiseGate=noiseGate;
    state.audioProcessing.noiseGateThreshold=std::clamp(noiseGateThreshold,0,100);
    state.audioProcessing.microphoneBoost=std::clamp(microphoneBoost,1.0f,3.0f);
    state.audioProcessing.compressor=compressor;
    state.audioProcessing.compressorStrength=std::clamp(compressorStrength,0,100);
    if(state.voiceClient) state.voiceClient->setAudioProcessingSettings(state.audioProcessing);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setAudioProcessingSettings(state.audioProcessing);
    persistBool("noise_gate",state.audioProcessing.noiseGate);
    persistInt("noise_gate_threshold",state.audioProcessing.noiseGateThreshold);
    persistFloat("microphone_boost",state.audioProcessing.microphoneBoost);
    persistBool("compressor",state.audioProcessing.compressor);
    persistInt("compressor_strength",state.audioProcessing.compressorStrength);
    state.controlError.clear();
}

void setEmbeddedVoiceMicrophoneGain(float gain) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.microphoneGain=std::clamp(gain,0.25f,5.0f);
    if(state.voiceClient) state.voiceClient->setMicrophoneGain(state.microphoneGain);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setMicrophoneGain(state.microphoneGain);
    persistFloat("microphone_gain",state.microphoneGain);
    state.controlError.clear();
}

void setEmbeddedVoicePlaybackVolume(float volume) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.playbackVolume=std::clamp(volume,0.0f,3.0f);
    if(state.voiceClient) state.voiceClient->setPlaybackVolume(state.playbackVolume);
    if(state.microphoneTestRuntime) state.microphoneTestRuntime->setPlaybackVolume(state.playbackVolume);
    persistFloat("playback_volume",state.playbackVolume);
    state.controlError.clear();
}

void setEmbeddedVoiceMicrophoneMuted(bool muted) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.microphoneMuted=muted;
    persistBool("muted",state.microphoneMuted);
    applyVoiceControls(state);
}

void setEmbeddedVoiceDeafened(bool deafened) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.deafened=deafened;
    persistBool("deafened",state.deafened);
    applyVoiceControls(state);
}

void setEmbeddedVoicePushToTalk(bool enabled) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pushToTalk=enabled;
    if(enabled) {
        state.voiceActivation=false;
        state.microphoneMuted=false;
        persistBool("voice_activation",false);
        persistBool("muted",false);
    }
    persistBool("push_to_talk",state.pushToTalk);
    applyVoiceControls(state);
}

void setEmbeddedVoicePushToMute(bool enabled) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pushToMute=enabled;
    if(!enabled) state.pushToMuteHeldInput=false;
    persistBool("push_to_mute",state.pushToMute);
    applyVoiceControls(state);
}

void setEmbeddedVoiceVoiceActivation(bool enabled,int threshold) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.voiceActivation=enabled;
    state.voiceActivationThreshold=std::clamp(threshold,1,100);
    if(enabled) {
        state.pushToTalk=false;
        persistBool("push_to_talk",false);
    }
    persistBool("voice_activation",state.voiceActivation);
    persistInt("voice_activation_threshold",state.voiceActivationThreshold);
    applyVoiceControls(state);
}

void setEmbeddedVoiceHotkeyState(bool pushToTalkHeldValue,bool pushToMuteHeldValue) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pushToTalkHeldInput=pushToTalkHeldValue;
    state.pushToMuteHeldInput=pushToMuteHeldValue;
    applyVoiceControls(state);
}

void setEmbeddedVoiceBinding(EmbeddedVoiceBindingAction action,std::int32_t keyboard,std::string controller) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    loadSettingsLocked(state);

    EmbeddedVoiceBinding* binding=nullptr;
    std::string_view keyName;
    std::string_view controllerName;
    switch(action) {
        case EmbeddedVoiceBindingAction::PushToTalk:
            binding=&state.pushToTalkBinding;
            keyName="ptt_key";
            controllerName="ptt_controller";
            break;
        case EmbeddedVoiceBindingAction::PushToMute:
            binding=&state.pushToMuteBinding;
            keyName="push_to_mute_key";
            controllerName="push_to_mute_controller";
            break;
        case EmbeddedVoiceBindingAction::ToggleMute:
            binding=&state.muteBinding;
            keyName="mute_key";
            controllerName="mute_controller";
            break;
        case EmbeddedVoiceBindingAction::ToggleDeafen:
            binding=&state.deafenBinding;
            keyName="deafen_key";
            controllerName="deafen_controller";
            break;
    }
    if(!binding) return;
    binding->keyboard=keyboard;
    binding->controller=std::move(controller);
    persistInt(keyName,binding->keyboard);
    persistString(controllerName,binding->controller);
}

void setEmbeddedVoiceMicrophoneTest(bool enabled) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(!state.enabled) return;
    state.microphoneTest=enabled;
    state.controlError.clear();
    applyVoiceControls(state);
}

void setEmbeddedVoicePeerVolume(const std::string& memberId,float volume) {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    loadSettingsLocked(state);
    const float clamped=std::clamp(volume,0.0f,3.0f);
    if(state.voiceClient) state.voiceClient->setRemoteVolume(memberId,clamped);
    if(const auto found=state.developmentPeers.find(memberId);found!=state.developmentPeers.end()) {
        state.savedPeerVolumes[found->second.participantId]=clamped;
        persistFloat("peer_volume_"+found->second.participantId,clamped);
    }
}

void resetEmbeddedVoiceAudioSettings() {
    auto& state=voiceSessionState();
    std::lock_guard<std::mutex> lock(state.mutex);
    loadSettingsLocked(state);

    state.inputDevice.clear();
    state.outputDevice.clear();
    state.audioProcessing={};
    state.microphoneGain=1.0f;
    state.playbackVolume=1.0f;
    state.microphoneMuted=false;
    state.deafened=false;
    state.pushToTalk=false;
    state.pushToMute=false;
    state.voiceActivation=false;
    state.voiceActivationThreshold=25;
    state.microphoneTest=false;
    state.pushToTalkHeldInput=false;
    state.pushToMuteHeldInput=false;

    persistString("input_device",{});
    persistString("output_device",{});
    persistBool("normalization",true);
    persistBool("noise_suppression",true);
    persistInt("noise_strength",50);
    persistBool("noise_gate",false);
    persistInt("noise_gate_threshold",25);
    persistFloat("microphone_boost",1.0f);
    persistBool("compressor",false);
    persistInt("compressor_strength",50);
    persistFloat("microphone_gain",1.0f);
    persistFloat("playback_volume",1.0f);
    persistBool("muted",false);
    persistBool("deafened",false);
    persistBool("push_to_talk",false);
    persistBool("push_to_mute",false);
    persistBool("voice_activation",false);
    persistInt("voice_activation_threshold",25);

    for(auto& [participantId,volume]:state.savedPeerVolumes) {
        volume=1.0f;
        persistFloat("peer_volume_"+participantId,1.0f);
    }
    try {
        if(state.voiceClient) {
            state.voiceClient->setCaptureDevice({});
            state.voiceClient->setPlaybackDevice({});
            state.voiceClient->setAudioProcessingSettings(state.audioProcessing);
            state.voiceClient->setMicrophoneGain(1.0f);
            state.voiceClient->setPlaybackVolume(1.0f);
            state.voiceClient->setRemoteVolume(1.0f);
        }
        stopMicrophoneTestRuntime(state);
        applyVoiceControls(state);
        state.controlError.clear();
    } catch(const std::exception& error) {
        state.controlError=error.what();
    }
}

void playEmbeddedVoiceTestTone() {
    std::string outputDevice;
    {
        auto& state=voiceSessionState();
        std::lock_guard<std::mutex> lock(state.mutex);
        loadSettingsLocked(state);
        if(!state.enabled) return;
        outputDevice=state.outputDevice;
    }

    std::thread([outputDevice=std::move(outputDevice)]() {
        try {
            mkwvc::AudioEngine audio({},outputDevice);
            audio.start();
            std::array<std::int16_t,mkwvc::VoiceFormat::FrameSamples> frame{};
            double phase=0.0;
            constexpr double step=6.2831853071795864769*440.0/static_cast<double>(mkwvc::VoiceFormat::SampleRate);
            for(int frameIndex=0;frameIndex<25;++frameIndex) {
                for(auto& sample:frame) {
                    sample=static_cast<std::int16_t>(std::sin(phase)*6500.0);
                    phase+=step;
                    if(phase>=6.2831853071795864769) phase-=6.2831853071795864769;
                }
                audio.queuePlayback(frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(mkwvc::VoiceFormat::FrameDurationMs));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            audio.stop();
        } catch(...) {
        }
    }).detach();
}

}
