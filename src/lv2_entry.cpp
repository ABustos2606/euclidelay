// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "pattern.hpp"
#include "ports.hpp"
#include "voice.hpp"
#include <lv2/core/lv2.h>
#include <cmath>
#include <cstdint>
#include <new>

namespace euclidelay {
namespace {
// A control's value, or its default if it isn't connected or isn't finite,
// clamped to its range.
float control(const float* const* ports, uint32_t index) {
    const PortSpec& spec = kPorts[index];
    const float* port = ports[index];
    const float value = port && std::isfinite(*port) ? *port : spec.def;
    return std::fmin(spec.max, std::fmax(spec.min, value));
}

int integer(const float* const* ports, uint32_t index) { return static_cast<int>(std::lround(control(ports, index))); }

struct Plugin {
    float* audio[3]{};             // in, out_l, out_r
    const float* ports[port_count]{};
    double rate = 0;
    Voice voices[kVoices];
};

// Voice n's settings from the controls, as of now.
void apply(Plugin& self, uint32_t n) {
    const double step = step_samples(kStepBeats[integer(self.ports, euclidelay::step)],
                                     control(self.ports, bpm), self.rate);
    const int steps = integer(self.ports, voice_port(n, euclidelay::steps));
    const uint32_t mask = pattern(steps, integer(self.ports, voice_port(n, hits)),
                                  integer(self.ports, voice_port(n, shift)));
    self.voices[n].set(make_taps(mask, steps, step), control(self.ports, voice_port(n, feedback)),
                       tone_pole(control(self.ports, tone), self.rate));
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    if (!(rate > 0)) return nullptr;
    auto* self = new (std::nothrow) Plugin;
    if (!self) return nullptr;
    self->rate = rate;
    // All the memory Euclidelay will ever use, now: never in run().
    for (Voice& v : self->voices)
        if (!v.allocate(kBufferSeconds, rate)) {
            delete self;
            return nullptr;
        }
    return self;
}

void connect_port(LV2_Handle handle, uint32_t port, void* data) {
    auto& self = *static_cast<Plugin*>(handle);
    if (port <= out_r) self.audio[port] = static_cast<float*>(data);
    else if (port < port_count) self.ports[port] = static_cast<const float*>(data);
}

void activate(LV2_Handle handle) {
    auto& self = *static_cast<Plugin*>(handle);
    for (uint32_t n = 0; n < kVoices; ++n) {
        self.voices[n].clear();
        apply(self, n);
    }
}

// Milestone 1: voice 1 only, its settings applied at the start of each slice
// with no crossfade or smoothing yet. Everything is per sample and the voice
// carries on across calls, so Ingen's slices (see docs/design.md) give the
// same output as one run() per period. Input and outputs may be the same
// buffer.
void run(LV2_Handle handle, uint32_t frames) {
    auto& self = *static_cast<Plugin*>(handle);
    const float* input = self.audio[in];
    float* left = self.audio[out_l];
    float* right = self.audio[out_r];
    if (!input || !left || !right) return;
    const float dry_gain = control(self.ports, dry);
    const bool on = control(self.ports, voice_port(0, voice)) >= 0.5f;
    Voice& voice1 = self.voices[0];
    apply(self, 0);
    // Constant power: a centred voice is -3 dB on each side.
    const double angle = (control(self.ports, voice_port(0, pan)) + 1.0) * M_PI / 4.0;
    const float gain = control(self.ports, echoes) * control(self.ports, voice_port(0, level));
    const float gain_l = static_cast<float>(gain * std::cos(angle));
    const float gain_r = static_cast<float>(gain * std::sin(angle));
    for (uint32_t i = 0; i < frames; ++i) {
        const float x = input[i];
        const float v = on ? voice1.process(x) : 0.0f;
        left[i] = dry_gain * x + gain_l * v;
        right[i] = dry_gain * x + gain_r * v;
    }
}

void cleanup(LV2_Handle handle) { delete static_cast<Plugin*>(handle); }

const LV2_Descriptor descriptor = {kURI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr};
} // namespace
} // namespace euclidelay

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &euclidelay::descriptor : nullptr;
}
