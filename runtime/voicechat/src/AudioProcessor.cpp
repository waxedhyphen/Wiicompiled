#include "mkwvc/AudioProcessor.hpp"

#include <speex/speex_preprocess.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace mkwvc {

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
        settings_.noiseSuppressionStrength=std::clamp(
            settings_.noiseSuppressionStrength,0,100);
        applySettingsLocked();
    }

    AudioProcessingSettings settings() const {
        std::scoped_lock lock(mutex_);
        return settings_;
    }

    void processCapture(std::span<std::int16_t> samples) {
        if(samples.size()!=VoiceFormat::FrameSamples) return;

        std::scoped_lock lock(mutex_);
        if(settings_.normalization || settings_.noiseSuppression) {
            speex_preprocess_run(preprocess_,samples.data());
        }
    }

private:
    void applySettingsLocked() {
        int denoise=settings_.noiseSuppression ? 1 : 0;
        int agc=settings_.normalization ? 1 : 0;
        int noiseSuppress=5+(settings_.noiseSuppressionStrength*35)/100;
        float agcLevel=8000.0f;
        int agcIncrement=12;
        int agcDecrement=-24;
        int agcMaxGain=24;

        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_DENOISE,&denoise);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC,&agc);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&noiseSuppress);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC_LEVEL,&agcLevel);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC_INCREMENT,&agcIncrement);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC_DECREMENT,&agcDecrement);
        speex_preprocess_ctl(preprocess_,SPEEX_PREPROCESS_SET_AGC_MAX_GAIN,&agcMaxGain);
    }

    mutable std::mutex mutex_;
    AudioProcessingSettings settings_{};
    SpeexPreprocessState* preprocess_=nullptr;
};

AudioProcessor::AudioProcessor():impl_(std::make_unique<Impl>()) {}
AudioProcessor::~AudioProcessor()=default;
void AudioProcessor::setSettings(const AudioProcessingSettings& settings){impl_->setSettings(settings);}
AudioProcessingSettings AudioProcessor::settings() const{return impl_->settings();}
void AudioProcessor::processCapture(std::span<std::int16_t> samples){impl_->processCapture(samples);}

}
