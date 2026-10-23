// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "ports.hpp"
#include "voice.hpp"
#include <cstdint>

// The four voices and the mix (docs/design.md, "Mix and stereo", "On and
// off", "Choosing voices"):
//     out = Dry * x  +  Echoes * clip( sum over voices of Level * g(Pan) * v )
// Milestone 2: Level, Pan, Feedback, Tone, Dry and Echoes take effect at the
// start of each slice, with no smoothing yet; the Voice n switches and
// Enabled fade.
namespace euclidelay {

// Enabled's fade of the input into the loops, in seconds.
inline constexpr double kEnableSeconds = 0.010;
// The echo clipper is transparent below its knee, -6 dBFS, and never goes
// above its ceiling, -0.2 dBFS (tune).
inline constexpr float kClipKnee = 0.5f, kClipCeiling = 0.98f;

// One voice's controls, clamped.
struct VoiceSettings {
    bool on = false;
    int steps = 1, hits = 0, shift = 0;
    float feedback = 0, level = 0, pan = 0;
};

// Every control, clamped to its range; a missing or non-finite one counts as
// its default. Start from default_settings(), not from Settings{}.
struct Settings {
    bool enabled = false;
    float bpm = 120;
    int step = 0;
    float dry = 0, echoes = 0, tone = 0;
    VoiceSettings voice[kVoices];
};

// The settings from the control ports, indexed as in ports.hpp; entries may
// be null. All null gives the defaults.
Settings read_settings(const float* const* ports);
Settings default_settings();

// The soft clipper on each echo side: s itself up to kClipKnee, then bending
// smoothly (with no change of slope at the knee) towards kClipCeiling.
inline float soft_clip(float s) {
    const float a = std::fabs(s);
    if (a <= kClipKnee) return s;
    constexpr float room = kClipCeiling - kClipKnee;
    return std::copysign(kClipKnee + room * std::tanh((a - kClipKnee) / room), s);
}

class Engine {
public:
    // The four loop buffers. False if the memory isn't there. Never called
    // from run().
    bool allocate(double rate);
    // Empties every voice and sets every fade to where `settings` puts it.
    void activate(const Settings& settings);
    // `frames` samples; `settings` hold for all of them. Everything carries
    // on across calls, so Ingen's slices give the same output as one call
    // per period. `in` may be the same buffer as either output.
    void run(const float* in, float* left, float* right, uint32_t frames, const Settings& settings);

    const Voice& voice(uint32_t n) const { return voices_[n]; }

private:
    void apply(const Settings& settings);
    float enable_gain() const { return enable_ramp_ >= enable_fade_ ? 1.0f : enable_ramp_ * inv_enable_fade_; }

    double rate_ = 0;
    Voice voices_[kVoices];
    float gain_l_[kVoices] = {}, gain_r_[kVoices] = {};  // Level * pan gain
    int enable_ramp_ = 0, enable_fade_ = 1;               // 0 off .. enable_fade_ on
    float inv_enable_fade_ = 1;
};

} // namespace euclidelay
