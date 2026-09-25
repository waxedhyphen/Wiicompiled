#include "mkwvc/MicrophoneTest.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>

namespace mkwvc {

namespace {

std::uint32_t peakOf(std::span<const std::int16_t> samples) {
    std::uint32_t peak=0;
    for(const auto sample:samples) {
        const auto value=sample<0 ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(sample)) : static_cast<std::uint32_t>(sample);
        peak=std::max(peak,value);
    }
    return peak;
}

void applyGain(std::span<std::int16_t> samples,float gain) {
    if(gain==1.0f) return;
    for(auto& sample:samples) {
        const auto scaled=static_cast<std::int32_t>(static_cast<float>(sample)*gain);
        sample=static_cast<std::int16_t>(std::clamp(scaled,-32768,32767));
    }
}

}

MicrophoneTest::MicrophoneTest(std::string captureDevice,std::string playbackDevice):
    audio_(std::move(captureDevice),std::move(playbackDevice)) {}

MicrophoneTest::~MicrophoneTest(){stop();}

void MicrophoneTest::start() {
    if(running_.exchange(true)) return;
    try {
        audio_.start();
        thread_=std::thread(&MicrophoneTest::loop,this);
    } catch(...) {
        running_=false;
        audio_.stop();
        throw;
    }
}

void MicrophoneTest::stop() {
    running_=false;
    if(thread_.joinable()) thread_.join();
    audio_.stop();
    micPeak_=0;
}

void MicrophoneTest::setCaptureDevice(std::string captureDevice) {
    audio_.setCaptureDevice(std::move(captureDevice));
}

void MicrophoneTest::setPlaybackDevice(std::string playbackDevice) {
    audio_.setPlaybackDevice(std::move(playbackDevice));
}

void MicrophoneTest::setMicrophoneGain(float gain) {
    microphoneGain_.store(std::clamp(gain,0.25f,5.0f),std::memory_order_relaxed);
}

void MicrophoneTest::setPlaybackVolume(float volume) {
    playbackVolume_.store(std::clamp(volume,0.0f,3.0f),std::memory_order_relaxed);
}

void MicrophoneTest::setMonitorEnabled(bool enabled) {
    monitorEnabled_.store(enabled,std::memory_order_relaxed);
}

void MicrophoneTest::setAudioProcessingSettings(const AudioProcessingSettings& settings) {
    processor_.setSettings(settings);
}

std::uint32_t MicrophoneTest::micPeak() const {
    return micPeak_.load(std::memory_order_relaxed);
}

bool MicrophoneTest::running() const {
    return running_.load(std::memory_order_relaxed);
}

void MicrophoneTest::loop() {
    std::array<std::int16_t,VoiceFormat::FrameSamples> samples{};
    std::array<std::int16_t,VoiceFormat::FrameSamples> monitor{};
    std::size_t filled=0;

    while(running_) {
        filled+=audio_.readCaptured(std::span<std::int16_t>(samples).subspan(filled));
        if(filled<VoiceFormat::FrameSamples) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        processor_.processCapture(samples);
        applyGain(samples,microphoneGain_.load(std::memory_order_relaxed));
        micPeak_.store(peakOf(samples),std::memory_order_relaxed);

        if(monitorEnabled_.load(std::memory_order_relaxed)) {
            std::copy(samples.begin(),samples.end(),monitor.begin());
            applyGain(monitor,playbackVolume_.load(std::memory_order_relaxed));
            audio_.queueMonitor(monitor);
        }
        filled=0;
    }
}

}
