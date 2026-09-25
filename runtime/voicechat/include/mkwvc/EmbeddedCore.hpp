#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mkwvc {

struct EmbeddedCoreStatus {
    std::uint32_t sampleRate = 0;
    std::uint32_t frameDurationMs = 0;
    std::uint32_t frameSamples = 0;
    bool voiceClientCompiled = false;
    bool audioDependenciesLinked = false;
    bool iceDependenciesLinked = false;
};

struct EmbeddedVoiceSessionInput {
    bool localRoomActive = false;
    std::string profileId;
    std::string sessionKey;
    std::string gameName;
    std::uint64_t identityGeneration = 0;
};

struct EmbeddedVoiceRoomPlayer {
    std::string profileId;
    std::string displayName;
    std::string friendCode;
    bool voiceChat = false;
};

struct EmbeddedVoiceSessionStatus {
    bool lifecycleActive = false;
    bool signalingConnected = false;
    bool productionAuthorizationPending = false;
    bool productionAuthorized = false;
    bool developmentAdmissionPending = false;
    bool developmentAdmitted = false;
    bool voiceClientRunning = false;
    bool roomFound = false;
    std::uint32_t developmentPeerCount = 0;
    std::uint32_t peerCount = 0;
    std::string roomId;
    std::string roomInstanceId;
    std::string roomCreated;
    std::string localMemberId;
    std::vector<EmbeddedVoiceRoomPlayer> roomPlayers;
    std::string status;
};

struct EmbeddedVoicePeerControl {
    std::string memberId;
    std::string participantId;
    std::string displayName;
    std::string friendCode;
    float volume = 1.0f;
};

struct EmbeddedVoiceControls {
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;
    std::string inputDevice;
    std::string outputDevice;
    bool automaticNormalization = true;
    bool noiseSuppression = true;
    int noiseSuppressionStrength = 50;
    float microphoneGain = 1.0f;
    float playbackVolume = 1.0f;
    bool microphoneMuted = false;
    bool deafened = false;
    bool pushToTalk = false;
    bool microphoneTest = false;
    bool pushToTalkHeld = false;
    std::uint32_t micPeak = 0;
    std::uint32_t playbackPeak = 0;
    std::vector<EmbeddedVoicePeerControl> peers;
    std::string error;
};

EmbeddedCoreStatus embeddedCoreStatus() noexcept;
void serviceEmbeddedVoiceSession(const EmbeddedVoiceSessionInput& input) noexcept;
EmbeddedVoiceSessionStatus embeddedVoiceSessionStatus();
EmbeddedVoiceControls embeddedVoiceControls();
void refreshEmbeddedVoiceDevices();
void setEmbeddedVoiceInputDevice(std::string device);
void setEmbeddedVoiceOutputDevice(std::string device);
void setEmbeddedVoiceProcessing(bool normalization,bool noiseSuppression,int noiseSuppressionStrength);
void setEmbeddedVoiceMicrophoneGain(float gain);
void setEmbeddedVoicePlaybackVolume(float volume);
void setEmbeddedVoiceMicrophoneMuted(bool muted);
void setEmbeddedVoiceDeafened(bool deafened);
void setEmbeddedVoicePushToTalk(bool enabled);
void setEmbeddedVoiceMicrophoneTest(bool enabled);
void setEmbeddedVoicePeerVolume(const std::string& memberId,float volume);

}
