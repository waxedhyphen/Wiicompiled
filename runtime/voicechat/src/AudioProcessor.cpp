#include "mkwvc/AudioProcessor.hpp"

#include <speex/speex_preprocess.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace mkwvc {

namespace {

float rmsOf(std::span<const std::int16_t> samples) {
    if(samples.empty()) return 0.0f;
    double energy=0.0;
    for(const auto sample:samples) energy+=static_cast<double>(sample)*static_cast<double>(sample);
    return static_cast<float>(std::sqrt(energy/static_cast<double>(samples.size())));
}

float peakOf(std::span<const std::int16_t> samples) {
    float peak=0.0f;
    for(const auto sample:samples) peak=std::max(peak,std::abs(static_cast<float>(sample)));
    return peak;
}

}

class AudioProcessor::Impl {
public:
    Impl() {
        preprocess_=speex_preprocess_state_init(
            static_cast<int>(VoiceFormat::FrameSamples),
            static_cast<int>(VoiceFormat::SampleRate));
        if(!preprocess_) throw std::runtime_error("Failed to initialize microphone preprocessor");
        applySettingsLocked();
    }

    ~Impl() {
        if(preprocess_) speex_preprocess_state_destroy(preprocess_);
    }

    void setSettings(const AudioProcessingSettings& settings) {
        std::scoped_lock lock(mutex_);
        settings_=settings;
        settings_.noiseSuppressionStrength=std::clamp(settings_.noiseSuppressionStrength,0,100);
        settings_.noiseGateThreshold=std::clamp(settings_.noiseGateThreshold,0,100);
        settings_.microphoneBoost=std::clamp(settings_.microphoneBoost,1.0f,3.0f);
        settings_.compressorStrength=std::clamp(settings_.compressorStrength,0,100);
        normalizationGain_=1.0f;
        gateGain_=1.0f;
        gateOpen_=true;
        applySettingsLocked();
    }

    AudioProcessingSettings settings() const {
        std::scoped_lock lock(mutex_);
        return settings_;
    }

    void processCapture(std::span<std::int16_t> samples) {
        if(samples.size()!=VoiceFormat::FrameSamples) return;

        std::scoped_lock lock(mutex_);
        if(settings_.noiseSuppression) speex_preprocess_run(preprocess_,samples.data());

        if(settings_.noiseGate) {
            const float rms=rmsOf(samples);
            const float openThreshold=120.0f+static_cast<float>(settings_.noiseGateThreshold)*32.0f;
            const float closeThreshold=openThreshold*0.72f;
            if(gateOpen_) {
                if(rms<closeThreshold) gateOpen_=false;
            } else if(rms>=openThreshold) {
                gateOpen_=true;
            }

            const float target=gateOpen_ ? 1.0f : 0.0f;
            const float speed=gateOpen_ ? 0.55f : 0.28f;
            gateGain_+=(target-gateGain_)*speed;
            if(gateGain_<0.001f) {
                std::fill(samples.begin(),samples.end(),0);
            } else if(gateGain_<0.999f) {
                for(auto& sample:samples) {
                    sample=static_cast<std::int16_t>(std::lround(static_cast<float>(sample)*gateGain_));
                }
            }
        } else {
            gateGain_=1.0f;
            gateOpen_=true;
        }

        if(settings_.normalization) {
            const float rms=rmsOf(samples);
            const float peak=peakOf(samples);
            constexpr float gateRms=350.0f;
            constexpr float targetRms=5200.0f;
            constexpr float ceiling=30000.0f;

            if(rms<gateRms) {
                if(normalizationGain_>1.0f) normalizationGain_=1.0f;
                else normalizationGain_+=(1.0f-normalizationGain_)*0.08f;
            } else {
                const float targetGain=std::clamp(targetRms/std::max(rms,1.0f),0.55f,2.5f);
                const float smoothing=targetGain<normalizationGain_ ? 0.45f : 0.035f;
                normalizationGain_+=(targetGain-normalizationGain_)*smoothing;
            }

            if(peak>0.0f) normalizationGain_=std::min(normalizationGain_,ceiling/peak);
            normalizationGain_=std::clamp(normalizationGain_,0.35f,2.5f);

            if(std::abs(normalizationGain_-1.0f)>=0.001f) {
                for(auto& sample:samples) {
                    const auto scaled=static_cast<std::int32_t>(
                        std::lround(static_cast<float>(sample)*normalizationGain_));
                    sample=static_cast<std::int16_t>(std::clamp(scaled,-32768,32767));
                }
            }
        } else {
            normalizationGain_=1.0f;
        }

        const float boost=settings_.microphoneBoost;
        const bool compress=settings_.compressor && settings_.compressorStrength>0;
        const float threshold=std::clamp(
            14500.0f-static_cast<float>(settings_.compressorStrength)*65.0f,
            6500.0f,
            14500.0f);
        const float ratio=1.0f+static_cast<float>(settings_.compressorStrength)*0.07f;

        if(boost!=1.0f || compress) {
            for(auto& sample:samples) {
                float value=static_cast<float>(sample)*boost;
                if(compress) {
                    const float sign=value<0.0f ? -1.0f : 1.0f;
                    float magnitude=std::abs(value);
                    if(magnitude>threshold) magnitude=threshold+(magnitude-threshold)/ratio;
                    value=sign*magnitude;
                }
                sample=static_cast<std::int16_t>(
                    std::clamp(static_cast<std::int32_t>(std::lround(value)),-32768,32767));
            }
        }
    }

private:
    void applySettingsLocked() {
        int denoise=settings_.noiseSuppression ? 1 : 0;
        int agc=0;
        int noiseSuppress=-(5+(settings_.noiseSuppressionStrength*35)/100);

        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_DENOISE,&denoise);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC,&agc);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&noiseSuppress);
    }

    mutable std::mutex mutex_;
    AudioProcessingSettings settings_{};
    SpeexPreprocessState* preprocess_=nullptr;
    float normalizationGain_=1.0f;
    float gateGain_=1.0f;
    bool gateOpen_=true;
};

AudioProcessor::AudioProcessor():impl_(std::make_unique<Impl>()) {}
AudioProcessor::~AudioProcessor()=default;
void AudioProcessor::setSettings(const AudioProcessingSettings& settings){impl_->setSettings(settings);}
AudioProcessingSettings AudioProcessor::settings() const{return impl_->settings();}
void AudioProcessor::processCapture(std::span<std::int16_t> samples){impl_->processCapture(samples);}

}
