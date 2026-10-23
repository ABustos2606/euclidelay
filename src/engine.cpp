// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "engine.hpp"
#include "pattern.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
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
    glide_ = static_cast<int>(std::lround(kGlideSeconds * rate));
    if (glide_ < 1) glide_ = 1;
    for (Voice& v : voices_)
        if (!v.allocate(kBufferSeconds, rate)) return false;
    // A run writes its samples before its taps read them, so it must not
    // write over anything its longest read still needs: the buffer's room
    // beyond the longest loop (16 steps of 1/4 at 30 BPM), 30 ms.
    const double slowest = step_samples(kStepBeats[std::size(kStepBeats) - 1], kPorts[bpm].min, rate);
    const long room = static_cast<long>(voices_[0].size()) - std::lround(kMaxSteps * slowest);
    chunk_ = room < kChunk ? static_cast<int>(room) : kChunk;
    return chunk_ >= 1;
}

void Engine::activate(const Settings& settings) {
    for (uint32_t n = 0; n < kVoices; ++n) {
        voices_[n].clear();
        if (!settings.voice[n].on) voices_[n].stop();
    }
    enable_ramp_ = settings.enabled ? enable_fade_ : 0;
    apply(settings, true);
}

void Engine::apply(const Settings& s, bool at_once) {
    const double step = step_samples(kStepBeats[s.step], s.bpm, rate_);
    const float pole = tone_pole(s.tone, rate_);
    enabled_ = s.enabled;
    if (at_once) {
        pole_.reset(pole);
        dry_.reset(s.dry);
        echoes_.reset(s.echoes);
    } else {
        pole_.set(pole, glide_);
        dry_.set(s.dry, glide_);
        echoes_.set(s.echoes, glide_);
    }
    for (uint32_t n = 0; n < kVoices; ++n) {
        const VoiceSettings& v = s.voice[n];
        Voice& voice = voices_[n];
        const bool was = voice.active();
        voice.switch_to(v.on);
        // Constant power: a centred voice is -3 dB on each side.
        const double angle = (v.pan + 1.0) * M_PI / 4.0;
        const float l = static_cast<float>(v.level * std::cos(angle));
        const float r = static_cast<float>(v.level * std::sin(angle));
        // A voice that is off, or coming on, has nothing to glide from.
        const bool fresh = at_once || !was || !voice.active();
        if (fresh) {
            gain_l_[n].reset(l);
            gain_r_[n].reset(r);
        } else {
            gain_l_[n].set(l, glide_);
            gain_r_[n].set(r, glide_);
        }
        if (!voice.active()) continue;
        const Taps taps = make_taps(pattern(v.steps, v.hits, v.shift), v.steps, step);
        if (fresh) voice.start(taps, v.feedback);
        else voice.set(taps, v.feedback);
    }
}

void Engine::run(const float* in, float* left, float* right, uint32_t frames, const Settings& s) {
    FlushToZero ftz;
    apply(s, false);
    for (uint32_t done = 0; done < frames;) {
        // A chunk of input, ending before a non-finite sample: that one starts
        // the next chunk as silence, with every voice emptied (the non-finite
        // guard), as if it had been met sample by sample.
        const uint32_t most = std::min<uint32_t>(chunk_, frames - done);
        uint32_t n = 0;
        for (; n < most; ++n) {
            float x = in[done + n];
            if (!std::isfinite(x)) {
                if (n > 0) break;
                x = 0.0f;
                for (Voice& voice : voices_) voice.empty();
            }
            // Enabled: the input fades in and out of the loops.
            const float on = enable_gain();
            x_[n] = x;
            on_[n] = on;
            into_[n] = x * on;
            poles_[n] = pole_.next();
            l_[n] = r_[n] = 0.0f;
            if (enabled_) {
                if (enable_ramp_ < enable_fade_) ++enable_ramp_;
            } else if (enable_ramp_ > 0) {
                --enable_ramp_;
            }
        }
        for (uint32_t k = 0; k < kVoices; ++k) {
            Voice& voice = voices_[k];
            if (!voice.active()) continue;
            voice.run(into_, poles_, v_, static_cast<int>(n));
            for (uint32_t i = 0; i < n; ++i) {
                l_[i] += gain_l_[k].next() * v_[i];
                r_[i] += gain_r_[k].next() * v_[i];
            }
        }
        // While off the dry sound is at unity, whatever Dry is.
        for (uint32_t i = 0; i < n; ++i) {
            const float dry_gain = dry_.next(), echoes = echoes_.next();
            const float dry_sound = (on_[i] * dry_gain + (1.0f - on_[i])) * x_[i];
            left[done + i] = dry_sound + echoes * soft_clip(l_[i]);
            right[done + i] = dry_sound + echoes * soft_clip(r_[i]);
        }
        done += n;
    }
}

} // namespace euclidelay
