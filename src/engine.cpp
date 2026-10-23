// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "engine.hpp"
#include "pattern.hpp"
#include <cmath>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace euclidelay {
namespace {
// A control's value, or its default if it isn't connected or isn't finite,
// clamped to its range.
float control(const float* const* ports, uint32_t index) {
    const PortSpec& spec = kPorts[index];
    const float* port = ports ? ports[index] : nullptr;
    const float value = port && std::isfinite(*port) ? *port : spec.def;
    return std::fmin(spec.max, std::fmax(spec.min, value));
}

int integer(const float* const* ports, uint32_t index) { return static_cast<int>(std::lround(control(ports, index))); }

// Flush-to-zero for the length of run(): the loops decay towards zero
// forever, and denormals are slow. Restored on the way out.
class FlushToZero {
public:
#if defined(__aarch64__)
    FlushToZero() {
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        asm volatile("msr fpcr, %0" : : "r"(saved_ | (uint64_t{1} << 24)));  // FZ
    }
    ~FlushToZero() { asm volatile("msr fpcr, %0" : : "r"(saved_)); }
private:
    uint64_t saved_;
#elif defined(__SSE__)
    FlushToZero() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040); }  // FTZ, DAZ
    ~FlushToZero() { _mm_setcsr(saved_); }
private:
    unsigned saved_;
#endif
};
} // namespace

Settings read_settings(const float* const* ports) {
    Settings s;
    s.enabled = control(ports, enabled) >= 0.5f;
    s.bpm = control(ports, bpm);
    s.step = integer(ports, step);
    s.dry = control(ports, dry);
    s.echoes = control(ports, echoes);
    s.tone = control(ports, tone);
    for (uint32_t n = 0; n < kVoices; ++n) {
        VoiceSettings& v = s.voice[n];
        v.on = control(ports, voice_port(n, voice)) >= 0.5f;
        v.steps = integer(ports, voice_port(n, steps));
        v.hits = integer(ports, voice_port(n, hits));
        v.shift = integer(ports, voice_port(n, shift));
        v.feedback = control(ports, voice_port(n, feedback));
        v.level = control(ports, voice_port(n, level));
        v.pan = control(ports, voice_port(n, pan));
    }
    return s;
}

Settings default_settings() { return read_settings(nullptr); }

bool Engine::allocate(double rate) {
    rate_ = rate;
    enable_fade_ = static_cast<int>(std::lround(kEnableSeconds * rate));
    if (enable_fade_ < 1) enable_fade_ = 1;
    inv_enable_fade_ = 1.0f / static_cast<float>(enable_fade_);
    for (Voice& v : voices_)
        if (!v.allocate(kBufferSeconds, rate)) return false;
    return true;
}

void Engine::activate(const Settings& settings) {
    for (uint32_t n = 0; n < kVoices; ++n) {
        voices_[n].clear();
        if (!settings.voice[n].on) voices_[n].stop();
    }
    enable_ramp_ = settings.enabled ? enable_fade_ : 0;
    apply(settings);
}

// Each voice's switch, taps, Feedback and Tone, and its gains on each side.
void Engine::apply(const Settings& s) {
    const double step = step_samples(kStepBeats[s.step], s.bpm, rate_);
    const float pole = tone_pole(s.tone, rate_);
    for (uint32_t n = 0; n < kVoices; ++n) {
        const VoiceSettings& v = s.voice[n];
        Voice& voice = voices_[n];
        voice.switch_to(v.on);
        if (voice.active())
            voice.set(make_taps(pattern(v.steps, v.hits, v.shift), v.steps, step), v.feedback, pole);
        // Constant power: a centred voice is -3 dB on each side.
        const double angle = (v.pan + 1.0) * M_PI / 4.0;
        gain_l_[n] = static_cast<float>(v.level * std::cos(angle));
        gain_r_[n] = static_cast<float>(v.level * std::sin(angle));
    }
}

void Engine::run(const float* in, float* left, float* right, uint32_t frames, const Settings& s) {
    FlushToZero ftz;
    apply(s);
    const float dry_gain = s.dry, echoes = s.echoes;
    for (uint32_t i = 0; i < frames; ++i) {
        float x = in[i];
        if (!std::isfinite(x)) {
            // A bad input sample: silence, and every voice starts again empty
            // (the non-finite guard).
            x = 0.0f;
            for (Voice& voice : voices_) voice.empty();
        }
        // Enabled: the input fades in and out of the loops; while off the
        // dry sound is at unity, whatever Dry is.
        const float on = enable_gain();
        const float into = x * on;
        float l = 0.0f, r = 0.0f;
        for (uint32_t n = 0; n < kVoices; ++n) {
            Voice& voice = voices_[n];
            if (!voice.active()) continue;
            const float v = voice.process(into);
            l += gain_l_[n] * v;
            r += gain_r_[n] * v;
        }
        const float dry_sound = (on * dry_gain + (1.0f - on)) * x;
        left[i] = dry_sound + echoes * soft_clip(l);
        right[i] = dry_sound + echoes * soft_clip(r);
        if (s.enabled) {
            if (enable_ramp_ < enable_fade_) ++enable_ramp_;
        } else if (enable_ramp_ > 0) {
            --enable_ramp_;
        }
    }
}

} // namespace euclidelay
