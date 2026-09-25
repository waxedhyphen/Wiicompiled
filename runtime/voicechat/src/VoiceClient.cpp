#include "mkwvc/VoiceClient.hpp"
#include "mkwvc/AdaptiveJitterBuffer.hpp"
#include "mkwvc/SpscRingBuffer.hpp"
#include "mkwvc/UdpVoiceTransport.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mkwvc {

namespace {

constexpr std::uint8_t PacketVersion=3;
constexpr std::string_view LegacyPeerId="legacy";
constexpr std::size_t MaxRemotePeers=11;
using PcmFrame=std::array<std::int16_t,VoiceFormat::FrameSamples>;

void writeStreamId(std::span<std::byte> packet,std::uint64_t streamId) {
    for(std::size_t i=0;i<8;++i) packet[6+i]=static_cast<std::byte>((streamId>>(56-i*8))&0xFF);
}

std::uint64_t readStreamId(std::span<const std::byte> packet) {
    std::uint64_t value=0;
    for(std::size_t i=0;i<8;++i) value=(value<<8)|std::to_integer<std::uint64_t>(packet[6+i]);
    return value;
}

void writeSequence(std::span<std::byte> packet,std::uint32_t sequence) {
    packet[14]=static_cast<std::byte>((sequence>>24)&0xFF);
    packet[15]=static_cast<std::byte>((sequence>>16)&0xFF);
    packet[16]=static_cast<std::byte>((sequence>>8)&0xFF);
    packet[17]=static_cast<std::byte>(sequence&0xFF);
}

std::uint32_t readSequence(std::span<const std::byte> packet) {
    return
        (std::to_integer<std::uint32_t>(packet[14])<<24)|
        (std::to_integer<std::uint32_t>(packet[15])<<16)|
        (std::to_integer<std::uint32_t>(packet[16])<<8)|
        std::to_integer<std::uint32_t>(packet[17]);
}

std::uint64_t makeStreamId() {
    std::random_device random;
    const auto randomPart=(static_cast<std::uint64_t>(random())<<32)^static_cast<std::uint64_t>(random());
    const auto timePart=static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto value=randomPart^timePart;
    return value==0 ? 1 : value;
}

bool hasMagic(std::span<const std::byte> packet) {
    return
        packet.size()>=5 &&
        packet[0]==static_cast<std::byte>('M') &&
        packet[1]==static_cast<std::byte>('K') &&
        packet[2]==static_cast<std::byte>('W') &&
        packet[3]==static_cast<std::byte>('V');
}

bool validHeader(std::span<const std::byte> packet) {
    return
        packet.size()>VoiceFormat::PacketHeaderBytes &&
        packet.size()<=VoiceFormat::MaxVoicePacketBytes &&
        hasMagic(packet) &&
        packet[4]==static_cast<std::byte>(PacketVersion);
}

std::uint32_t peakOf(std::span<const std::int16_t> samples) {
    std::uint32_t peak=0;
    for(const auto sample:samples) {
        const auto value=sample<0
            ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(sample))
            : static_cast<std::uint32_t>(sample);
        peak=std::max(peak,value);
    }
    return peak;
}

void applyGainWithLimiter(std::span<std::int16_t> samples,float gain,float& limiterGain) {
    gain=std::max(0.0f,gain);
    float peak=0.0f;
    for(const auto sample:samples) peak=std::max(peak,std::abs(static_cast<float>(sample))*gain);

    constexpr float ceiling=31600.0f;
    const float desired=peak>ceiling ? ceiling/peak : 1.0f;
    if(desired<limiterGain) limiterGain=desired;
    else limiterGain+=std::min(1.0f,desired-limiterGain)*0.06f;
    limiterGain=std::clamp(limiterGain,0.0f,1.0f);

    const float totalGain=gain*limiterGain;
    for(auto& sample:samples) {
        const auto scaled=static_cast<std::int32_t>(std::lround(static_cast<float>(sample)*totalGain));
        sample=static_cast<std::int16_t>(std::clamp(scaled,-32768,32767));
    }
}

}

struct VoiceClient::PeerState {
    PeerState(std::string id,std::unique_ptr<VoiceTransport> value):
        memberId(std::move(id)),
        transport(std::move(value)) {}

    std::string memberId;
    std::unique_ptr<VoiceTransport> transport;
    OpusDecoderCodec decoder;
    AdaptiveJitterBuffer jitterBuffer;
    NetworkSimulator simulator;
    SpscRingBuffer<PcmFrame,32> pcmFrames;
    std::vector<std::uint64_t> retiredStreams;
    std::uint64_t currentStreamId=0;
    bool haveStream=false;
    std::atomic<bool> running{false};
    std::atomic<float> volume{1.0f};
    std::atomic<std::uint64_t> txPackets{0};
    std::atomic<std::uint64_t> rxPackets{0};
    std::atomic<std::uint64_t> rawRxPackets{0};
    std::atomic<std::uint64_t> protocolMismatchPackets{0};
    std::atomic<std::uint64_t> txBytes{0};
    std::atomic<std::uint64_t> rxBytes{0};
    std::atomic<std::uint64_t> simulatedDrops{0};
    std::atomic<std::uint64_t> simulatedDuplicates{0};
    std::atomic<std::uint64_t> simulatedReorders{0};
    std::atomic<std::uint64_t> reorderedRxPackets{0};
    std::atomic<std::uint64_t> latePackets{0};
    std::atomic<std::uint64_t> plcFrames{0};
    std::atomic<std::uint64_t> fecAttempts{0};
    std::atomic<std::uint64_t> decoderErrors{0};
    std::atomic<std::uint64_t> sequenceResyncs{0};
    std::atomic<std::uint64_t> streamRestarts{0};
    std::atomic<std::uint64_t> jitterBufferExpansions{0};
    std::atomic<std::uint32_t> simulationQueueDepth{0};
    std::atomic<std::uint32_t> jitterBufferQueuedPackets{0};
    std::atomic<std::uint32_t> jitterBufferTargetMs{0};
    std::atomic<std::uint32_t> jitterBufferCurrentMs{0};
    std::atomic<std::uint32_t> estimatedJitterMicros{0};
    std::thread receiver;
};

VoiceClient::VoiceClient(std::string peerAddress,std::uint16_t port,std::string captureDevice,std::string playbackDevice):
    VoiceClient(
        std::make_unique<UdpVoiceTransport>(std::move(peerAddress),port),
        std::move(captureDevice),
        std::move(playbackDevice)
    ) {}

VoiceClient::VoiceClient(std::unique_ptr<VoiceTransport> transport,std::string captureDevice,std::string playbackDevice):
    VoiceClient(std::string(LegacyPeerId),std::move(transport),std::move(captureDevice),std::move(playbackDevice)) {}

VoiceClient::VoiceClient(std::string memberId,std::unique_ptr<VoiceTransport> transport,std::string captureDevice,std::string playbackDevice):
    audio_(std::move(captureDevice),std::move(playbackDevice)) {
    if(!transport) throw std::invalid_argument("Voice transport is required");
    addPeer(std::move(memberId),std::move(transport));
}

VoiceClient::~VoiceClient(){stop();}

std::vector<std::shared_ptr<VoiceClient::PeerState>> VoiceClient::peerSnapshot() const {
    std::scoped_lock lock(peersMutex_);
    std::vector<std::shared_ptr<PeerState>> peers;
    peers.reserve(peers_.size());
    for(const auto& [id,peer]:peers_) {
        (void)id;
        peers.push_back(peer);
    }
    return peers;
}

void VoiceClient::startPeerReceiver(const std::shared_ptr<PeerState>& peer) {
    if(!peer || peer->running.exchange(true)) return;
    if(peer->receiver.joinable()) peer->receiver.join();
    peer->receiver=std::thread(&VoiceClient::receiverLoop,this,peer);
}

void VoiceClient::start() {
    if(running_.exchange(true)) return;

    try {
        audio_.start();
        for(const auto& peer:peerSnapshot()) startPeerReceiver(peer);
        sender_=std::thread(&VoiceClient::senderLoop,this);
        mixer_=std::thread(&VoiceClient::mixerLoop,this);
    } catch(...) {
        stop();
        throw;
    }
}

void VoiceClient::stop() {
    running_=false;

    const auto peers=peerSnapshot();
    for(const auto& peer:peers) peer->running=false;

    if(sender_.joinable()) sender_.join();
    if(mixer_.joinable()) mixer_.join();

    for(const auto& peer:peers) {
        if(peer->receiver.joinable()) peer->receiver.join();
        peer->simulator.clear();
    }

    simulationQueueDepth_=0;
    audio_.stop();
}

void VoiceClient::addPeer(std::string memberId,std::unique_ptr<VoiceTransport> transport) {
    if(memberId.empty()) throw std::invalid_argument("Voice peer member ID is empty");
    if(!transport) throw std::invalid_argument("Voice peer transport is required");

    auto peer=std::make_shared<PeerState>(memberId,std::move(transport));

    {
        std::scoped_lock lock(peersMutex_);
        if(peers_.contains(memberId)) throw std::invalid_argument("Voice peer already exists");
        if(peers_.size()>=MaxRemotePeers) throw std::runtime_error("Voice session already has 11 remote peers");
        peers_.emplace(memberId,peer);
    }

    if(running_.load(std::memory_order_relaxed)) {
        try {
            startPeerReceiver(peer);
        } catch(...) {
            std::scoped_lock lock(peersMutex_);
            peers_.erase(memberId);
            throw;
        }
    }
}

void VoiceClient::removePeer(const std::string& memberId) {
    std::shared_ptr<PeerState> peer;

    {
        std::scoped_lock lock(peersMutex_);
        const auto found=peers_.find(memberId);
        if(found==peers_.end()) return;
        peer=found->second;
        peers_.erase(found);
    }

    peer->running=false;
    if(peer->receiver.joinable()) peer->receiver.join();
}

bool VoiceClient::hasPeer(const std::string& memberId) const {
    std::scoped_lock lock(peersMutex_);
    return peers_.contains(memberId);
}

std::size_t VoiceClient::peerCount() const {
    std::scoped_lock lock(peersMutex_);
    return peers_.size();
}

std::vector<std::string> VoiceClient::peerIds() const {
    std::scoped_lock lock(peersMutex_);
    std::vector<std::string> ids;
    ids.reserve(peers_.size());
    for(const auto& [id,peer]:peers_) {
        (void)peer;
        ids.push_back(id);
    }
    return ids;
}

void VoiceClient::setCodecSettings(const OpusCodecSettings& settings) {
    std::scoped_lock lock(settingsMutex_);
    codecSettings_=settings;
}

void VoiceClient::setNetworkSimulation(const NetworkSimulationSettings& settings) {
    std::scoped_lock lock(settingsMutex_);
    networkSimulation_=settings;
}

void VoiceClient::setCaptureDevice(std::string captureDevice) {
    audio_.setCaptureDevice(std::move(captureDevice));
}

void VoiceClient::setPlaybackDevice(std::string playbackDevice) {
    audio_.setPlaybackDevice(std::move(playbackDevice));
}

void VoiceClient::setMicrophoneGain(float gain) {
    microphoneGain_.store(std::clamp(gain,0.25f,5.0f),std::memory_order_relaxed);
}

void VoiceClient::setAudioProcessingSettings(const AudioProcessingSettings& settings) {
    processor_.setSettings(settings);
}

void VoiceClient::setMicrophoneTestEnabled(bool enabled) {
    microphoneTestEnabled_.store(enabled,std::memory_order_relaxed);
}

void VoiceClient::setPlaybackVolume(float volume) {
    playbackVolume_.store(std::clamp(volume,0.0f,3.0f),std::memory_order_relaxed);
}

void VoiceClient::setRemoteVolume(float volume) {
    const auto clamped=std::clamp(volume,0.0f,3.0f);
    for(const auto& peer:peerSnapshot()) peer->volume.store(clamped,std::memory_order_relaxed);
}

void VoiceClient::setRemoteVolume(const std::string& memberId,float volume) {
    std::shared_ptr<PeerState> peer;
    {
        std::scoped_lock lock(peersMutex_);
        const auto found=peers_.find(memberId);
        if(found==peers_.end()) return;
        peer=found->second;
    }
    peer->volume.store(std::clamp(volume,0.0f,3.0f),std::memory_order_relaxed);
}

void VoiceClient::setTransmitEnabled(bool enabled) {
    transmitEnabled_.store(enabled,std::memory_order_relaxed);
}

void VoiceClient::setDeafened(bool deafened) {
    deafened_.store(deafened,std::memory_order_relaxed);
    audio_.setPlaybackMuted(deafened);
}

std::string VoiceClient::localAddress() const {
    const auto peers=peerSnapshot();
    return peers.empty() ? std::string{} : peers.front()->transport->localAddress();
}

std::string VoiceClient::localAddress(const std::string& memberId) const {
    std::shared_ptr<PeerState> peer;
    {
        std::scoped_lock lock(peersMutex_);
        const auto found=peers_.find(memberId);
        if(found==peers_.end()) return {};
        peer=found->second;
    }
    return peer->transport->localAddress();
}

OpusCodecSettings VoiceClient::codecSettings() const {
    std::scoped_lock lock(settingsMutex_);
    return codecSettings_;
}

NetworkSimulationSettings VoiceClient::networkSimulation() const {
    std::scoped_lock lock(settingsMutex_);
    return networkSimulation_;
}

VoiceStats VoiceClient::stats() const {
    return {
        txPackets_.load(std::memory_order_relaxed),
        rxPackets_.load(std::memory_order_relaxed),
        rawRxPackets_.load(std::memory_order_relaxed),
        protocolMismatchPackets_.load(std::memory_order_relaxed),
        txBytes_.load(std::memory_order_relaxed),
        rxBytes_.load(std::memory_order_relaxed),
        simulatedDrops_.load(std::memory_order_relaxed),
        simulatedDuplicates_.load(std::memory_order_relaxed),
        simulatedReorders_.load(std::memory_order_relaxed),
        reorderedRxPackets_.load(std::memory_order_relaxed),
        latePackets_.load(std::memory_order_relaxed),
        plcFrames_.load(std::memory_order_relaxed),
        fecAttempts_.load(std::memory_order_relaxed),
        decoderErrors_.load(std::memory_order_relaxed),
        sequenceResyncs_.load(std::memory_order_relaxed),
        streamRestarts_.load(std::memory_order_relaxed),
        jitterBufferExpansions_.load(std::memory_order_relaxed),
        simulationQueueDepth_.load(std::memory_order_relaxed),
        jitterBufferQueuedPackets_.load(std::memory_order_relaxed),
        jitterBufferTargetMs_.load(std::memory_order_relaxed),
        jitterBufferCurrentMs_.load(std::memory_order_relaxed),
        static_cast<float>(estimatedJitterMicros_.load(std::memory_order_relaxed))/1000.0f,
        micPeak_.load(std::memory_order_relaxed),
        playbackPeak_.load(std::memory_order_relaxed),
        running_.load(std::memory_order_relaxed)
    };
}


std::vector<PeerVoiceStats> VoiceClient::peerStats() const {
    const auto peers=peerSnapshot();
    std::vector<PeerVoiceStats> result;
    result.reserve(peers.size());

    for(const auto& peer:peers) {
        result.push_back({
            peer->memberId,
            peer->txPackets.load(std::memory_order_relaxed),
            peer->rxPackets.load(std::memory_order_relaxed),
            peer->rawRxPackets.load(std::memory_order_relaxed),
            peer->protocolMismatchPackets.load(std::memory_order_relaxed),
            peer->txBytes.load(std::memory_order_relaxed),
            peer->rxBytes.load(std::memory_order_relaxed),
            peer->simulatedDrops.load(std::memory_order_relaxed),
            peer->simulatedDuplicates.load(std::memory_order_relaxed),
            peer->simulatedReorders.load(std::memory_order_relaxed),
            peer->reorderedRxPackets.load(std::memory_order_relaxed),
            peer->latePackets.load(std::memory_order_relaxed),
            peer->plcFrames.load(std::memory_order_relaxed),
            peer->fecAttempts.load(std::memory_order_relaxed),
            peer->decoderErrors.load(std::memory_order_relaxed),
            peer->sequenceResyncs.load(std::memory_order_relaxed),
            peer->streamRestarts.load(std::memory_order_relaxed),
            peer->jitterBufferExpansions.load(std::memory_order_relaxed),
            peer->simulationQueueDepth.load(std::memory_order_relaxed),
            peer->jitterBufferQueuedPackets.load(std::memory_order_relaxed),
            peer->jitterBufferTargetMs.load(std::memory_order_relaxed),
            peer->jitterBufferCurrentMs.load(std::memory_order_relaxed),
            static_cast<float>(peer->estimatedJitterMicros.load(std::memory_order_relaxed))/1000.0f,
            peer->volume.load(std::memory_order_relaxed),
            peer->running.load(std::memory_order_relaxed)
        });
    }

    std::sort(result.begin(),result.end(),[](const PeerVoiceStats& a,const PeerVoiceStats& b) {
        return a.memberId<b.memberId;
    });
    return result;
}

void VoiceClient::senderLoop() {
    std::array<std::int16_t,VoiceFormat::FrameSamples> samples{};
    std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> packet{};
    std::array<std::int16_t,VoiceFormat::FrameSamples> monitor{};

    packet[0]=static_cast<std::byte>('M');
    packet[1]=static_cast<std::byte>('K');
    packet[2]=static_cast<std::byte>('W');
    packet[3]=static_cast<std::byte>('V');
    packet[4]=static_cast<std::byte>(PacketVersion);
    packet[5]=static_cast<std::byte>(0);

    const auto streamId=makeStreamId();
    writeStreamId(packet,streamId);

    std::uint32_t sequence=0;
    std::size_t filled=0;
    float microphoneLimiterGain=1.0f;
    float monitorLimiterGain=1.0f;
    OpusCodecSettings appliedSettings{};
    bool settingsApplied=false;

    const auto flushPeer=[&](const std::shared_ptr<PeerState>& peer) {
        std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> outgoing{};
        std::size_t size=0;

        while(running_.load(std::memory_order_relaxed) &&
              peer->running.load(std::memory_order_relaxed) &&
              peer->simulator.popReady(outgoing,size)) {
            try {
                if(peer->transport->send(std::span<const std::byte>(outgoing.data(),size))) {
                    txPackets_.fetch_add(1,std::memory_order_relaxed);
                    txBytes_.fetch_add(size,std::memory_order_relaxed);
                    peer->txPackets.fetch_add(1,std::memory_order_relaxed);
                    peer->txBytes.fetch_add(size,std::memory_order_relaxed);
                }
            } catch(...) {
                peer->running=false;
                break;
            }
        }
    };

    while(running_.load(std::memory_order_relaxed)) {
        try {
            auto peers=peerSnapshot();
            for(const auto& peer:peers) flushPeer(peer);

            filled+=audio_.readCaptured(std::span<std::int16_t>(samples).subspan(filled));
            if(filled<VoiceFormat::FrameSamples) {
                std::uint32_t queued=0;
                for(const auto& peer:peers) {
                    const auto peerQueued=static_cast<std::uint32_t>(peer->simulator.queuedPackets());
                    peer->simulationQueueDepth.store(peerQueued,std::memory_order_relaxed);
                    queued+=peerQueued;
                }
                simulationQueueDepth_.store(queued,std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            processor_.processCapture(samples);

            const auto gain=microphoneGain_.load(std::memory_order_relaxed);
            applyGainWithLimiter(samples,gain,microphoneLimiterGain);

            const bool microphoneTest=microphoneTestEnabled_.load(std::memory_order_relaxed);
            const auto processedPeak=peakOf(samples);
            if(microphoneTest) {
                std::copy(samples.begin(),samples.end(),monitor.begin());
                const auto monitorVolume=playbackVolume_.load(std::memory_order_relaxed);
                applyGainWithLimiter(monitor,monitorVolume,monitorLimiterGain);
                audio_.queueMonitor(monitor);
            }

            if(!transmitEnabled_.load(std::memory_order_relaxed)) std::fill(samples.begin(),samples.end(),0);

            const auto settings=codecSettings();
            if(!settingsApplied || settings!=appliedSettings) {
                encoder_.configure(settings);
                appliedSettings=settings;
                settingsApplied=true;
            }

            micPeak_.store(microphoneTest ? processedPeak : peakOf(samples),std::memory_order_relaxed);
            writeSequence(packet,sequence++);

            const auto payload=encoder_.encode(
                samples,
                std::span<std::byte>(packet).subspan(VoiceFormat::PacketHeaderBytes)
            );
            const auto packetSize=VoiceFormat::PacketHeaderBytes+payload;
            const auto simulationSettings=networkSimulation();

            peers=peerSnapshot();
            std::uint32_t queued=0;
            for(const auto& peer:peers) {
                if(!peer->running.load(std::memory_order_relaxed)) continue;

                const auto event=peer->simulator.schedule(
                    std::span<const std::byte>(packet.data(),packetSize),
                    simulationSettings
                );

                simulatedDrops_.fetch_add(event.dropped,std::memory_order_relaxed);
                simulatedDuplicates_.fetch_add(event.duplicated,std::memory_order_relaxed);
                simulatedReorders_.fetch_add(event.reordered,std::memory_order_relaxed);
                peer->simulatedDrops.fetch_add(event.dropped,std::memory_order_relaxed);
                peer->simulatedDuplicates.fetch_add(event.duplicated,std::memory_order_relaxed);
                peer->simulatedReorders.fetch_add(event.reordered,std::memory_order_relaxed);

                flushPeer(peer);
                const auto peerQueued=static_cast<std::uint32_t>(peer->simulator.queuedPackets());
                peer->simulationQueueDepth.store(peerQueued,std::memory_order_relaxed);
                queued+=peerQueued;
            }

            simulationQueueDepth_.store(queued,std::memory_order_relaxed);
            filled=0;
        } catch(...) {
            running_=false;
            break;
        }
    }
}

void VoiceClient::receiverLoop(const std::shared_ptr<PeerState>& peer) {
    std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> packet{};
    std::array<std::int16_t,VoiceFormat::FrameSamples> samples{};

    const auto queueDecoded=[&](std::size_t count) {
        PcmFrame frame{};
        const auto copyCount=std::min(count,frame.size());
        std::copy_n(samples.begin(),copyCount,frame.begin());
        peer->pcmFrames.push(std::span<const PcmFrame>(&frame,1));
    };

    const auto conceal=[&](bool expansion) {
        try {
            const auto decoded=peer->decoder.conceal(samples);
            if(expansion) {
                jitterBufferExpansions_.fetch_add(1,std::memory_order_relaxed);
                peer->jitterBufferExpansions.fetch_add(1,std::memory_order_relaxed);
            } else {
                plcFrames_.fetch_add(1,std::memory_order_relaxed);
                peer->plcFrames.fetch_add(1,std::memory_order_relaxed);
            }
            queueDecoded(decoded);
        } catch(...) {
            decoderErrors_.fetch_add(1,std::memory_order_relaxed);
            peer->decoderErrors.fetch_add(1,std::memory_order_relaxed);
        }
    };

    const auto updateJitterStats=[&]() {
        const auto jitterStats=peer->jitterBuffer.stats();
        jitterBufferQueuedPackets_.store(jitterStats.queuedPackets,std::memory_order_relaxed);
        jitterBufferTargetMs_.store(jitterStats.targetDelayMs,std::memory_order_relaxed);
        jitterBufferCurrentMs_.store(jitterStats.currentDelayMs,std::memory_order_relaxed);
        const auto jitterMicros=
            static_cast<std::uint32_t>(std::max(0.0f,jitterStats.estimatedJitterMs)*1000.0f);
        estimatedJitterMicros_.store(jitterMicros,std::memory_order_relaxed);
        peer->jitterBufferQueuedPackets.store(jitterStats.queuedPackets,std::memory_order_relaxed);
        peer->jitterBufferTargetMs.store(jitterStats.targetDelayMs,std::memory_order_relaxed);
        peer->jitterBufferCurrentMs.store(jitterStats.currentDelayMs,std::memory_order_relaxed);
        peer->estimatedJitterMicros.store(jitterMicros,std::memory_order_relaxed);
    };

    const auto processReadyFrames=[&]() {
        for(int drained=0;drained<16;++drained) {
            const auto ready=peer->jitterBuffer.popReady();
            if(!ready) break;

            if(ready->kind==JitterBufferFrameKind::Hold) {
                conceal(true);
                continue;
            }

            if(ready->kind==JitterBufferFrameKind::Missing) {
                if(ready->fecPayloadSize>0) {
                    try {
                        const auto decoded=peer->decoder.decode(
                            std::span<const std::byte>(ready->fecPayload.data(),ready->fecPayloadSize),
                            samples,
                            true
                        );
                        fecAttempts_.fetch_add(1,std::memory_order_relaxed);
                        peer->fecAttempts.fetch_add(1,std::memory_order_relaxed);
                        queueDecoded(decoded);
                        continue;
                    } catch(...) {
                        decoderErrors_.fetch_add(1,std::memory_order_relaxed);
                        peer->decoderErrors.fetch_add(1,std::memory_order_relaxed);
                    }
                }

                conceal(false);
                continue;
            }

            try {
                const auto decoded=peer->decoder.decode(
                    std::span<const std::byte>(ready->payload.data(),ready->payloadSize),
                    samples,
                    false
                );
                queueDecoded(decoded);
            } catch(...) {
                decoderErrors_.fetch_add(1,std::memory_order_relaxed);
                peer->decoderErrors.fetch_add(1,std::memory_order_relaxed);
                conceal(false);
            }
        }

        updateJitterStats();
    };

    while(running_.load(std::memory_order_relaxed) &&
          peer->running.load(std::memory_order_relaxed)) {
        const auto size=peer->transport->receive(packet);

        if(size>0) {
            rawRxPackets_.fetch_add(1,std::memory_order_relaxed);
            peer->rawRxPackets.fetch_add(1,std::memory_order_relaxed);
            const auto received=std::span<const std::byte>(packet.data(),size);

            if(!validHeader(received)) {
                if(hasMagic(received) && received[4]!=static_cast<std::byte>(PacketVersion)) {
                    protocolMismatchPackets_.fetch_add(1,std::memory_order_relaxed);
                    peer->protocolMismatchPackets.fetch_add(1,std::memory_order_relaxed);
                }
                processReadyFrames();
                continue;
            }

            rxPackets_.fetch_add(1,std::memory_order_relaxed);
            rxBytes_.fetch_add(size,std::memory_order_relaxed);
            peer->rxPackets.fetch_add(1,std::memory_order_relaxed);
            peer->rxBytes.fetch_add(size,std::memory_order_relaxed);

            const auto streamId=readStreamId(received);
            const auto sequence=readSequence(received);

            if(!peer->haveStream) {
                peer->currentStreamId=streamId;
                peer->haveStream=true;
                peer->decoder.reset();
                peer->jitterBuffer.reset(false);
            } else if(streamId!=peer->currentStreamId) {
                if(std::find(peer->retiredStreams.begin(),peer->retiredStreams.end(),streamId)!=peer->retiredStreams.end()) {
                    latePackets_.fetch_add(1,std::memory_order_relaxed);
                    peer->latePackets.fetch_add(1,std::memory_order_relaxed);
                    processReadyFrames();
                    continue;
                }

                peer->retiredStreams.push_back(peer->currentStreamId);
                if(peer->retiredStreams.size()>16) peer->retiredStreams.erase(peer->retiredStreams.begin());

                peer->currentStreamId=streamId;
                peer->decoder.reset();
                peer->jitterBuffer.reset(false);
                streamRestarts_.fetch_add(1,std::memory_order_relaxed);
                peer->streamRestarts.fetch_add(1,std::memory_order_relaxed);
            }

            const auto payload=received.subspan(VoiceFormat::PacketHeaderBytes);
            auto pushResult=peer->jitterBuffer.push(sequence,payload);

            if(pushResult==JitterBufferPushResult::TooFarAhead) {
                sequenceResyncs_.fetch_add(1,std::memory_order_relaxed);
                peer->sequenceResyncs.fetch_add(1,std::memory_order_relaxed);
                peer->decoder.reset();
                peer->jitterBuffer.reset(true);
                pushResult=peer->jitterBuffer.push(sequence,payload);
            }

            if(pushResult==JitterBufferPushResult::Late || pushResult==JitterBufferPushResult::Duplicate) {
                latePackets_.fetch_add(1,std::memory_order_relaxed);
                peer->latePackets.fetch_add(1,std::memory_order_relaxed);
            } else if(pushResult==JitterBufferPushResult::AcceptedReordered) {
                reorderedRxPackets_.fetch_add(1,std::memory_order_relaxed);
                peer->reorderedRxPackets.fetch_add(1,std::memory_order_relaxed);
            }
        }

        processReadyFrames();
    }

    peer->running=false;
}

void VoiceClient::mixerLoop() {
    auto nextMix=std::chrono::steady_clock::now();
    float limiterGain=1.0f;

    while(running_.load(std::memory_order_relaxed)) {
        nextMix+=std::chrono::milliseconds(VoiceFormat::FrameDurationMs);

        std::array<float,VoiceFormat::FrameSamples> mixed{};
        PcmFrame frame{};
        bool anyFrame=false;

        const auto peers=peerSnapshot();
        for(const auto& peer:peers) {
            const auto popped=peer->pcmFrames.pop(std::span<PcmFrame>(&frame,1));
            if(popped==0) continue;

            anyFrame=true;
            const auto volume=peer->volume.load(std::memory_order_relaxed);
            for(std::size_t i=0;i<frame.size();++i) mixed[i]+=static_cast<float>(frame[i])*volume;
        }

        if(deafened_.load(std::memory_order_relaxed)) {
            playbackPeak_.store(0,std::memory_order_relaxed);
        } else if(anyFrame) {
            PcmFrame output{};
            const auto globalVolume=playbackVolume_.load(std::memory_order_relaxed);
            float peak=0.0f;
            for(const auto sample:mixed) peak=std::max(peak,std::abs(sample*globalVolume));

            constexpr float ceiling=31600.0f;
            const float desired=peak>ceiling ? ceiling/peak : 1.0f;
            if(desired<limiterGain) limiterGain=desired;
            else limiterGain+=(desired-limiterGain)*0.05f;
            limiterGain=std::clamp(limiterGain,0.0f,1.0f);

            const float finalGain=globalVolume*limiterGain;
            for(std::size_t i=0;i<output.size();++i) {
                const auto scaled=static_cast<std::int32_t>(std::lround(mixed[i]*finalGain));
                output[i]=static_cast<std::int16_t>(std::clamp(scaled,-32768,32767));
            }

            playbackPeak_.store(peakOf(output),std::memory_order_relaxed);
            audio_.queuePlayback(std::span<const std::int16_t>(output));
        } else {
            playbackPeak_.store(0,std::memory_order_relaxed);
            limiterGain+=(1.0f-limiterGain)*0.05f;
        }

        std::this_thread::sleep_until(nextMix);
        const auto now=std::chrono::steady_clock::now();
        if(nextMix+std::chrono::milliseconds(VoiceFormat::FrameDurationMs*4)<now) nextMix=now;
    }
}

}
