// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "ports.hpp"
#include <lv2/core/lv2.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

struct Plugin {
    ~Plugin() { for (float* b : buffers) std::free(b); }
    float* audio[3]{};             // in, out_l, out_r
    const float* ports[port_count]{};
    float* buffers[kVoices]{};     // each voice's loop buffer
    size_t size = 0;               // floats per buffer
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    if (!(rate > 0)) return nullptr;
    auto* self = new (std::nothrow) Plugin;
    if (!self) return nullptr;
    // All the memory Euclidelay will ever use, now: never in run().
    self->size = static_cast<size_t>(std::ceil(kBufferSeconds * rate)) + 1;
    for (float*& b : self->buffers)
        if (!(b = static_cast<float*>(std::calloc(self->size, sizeof(float))))) {
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
    for (float* b : self.buffers) std::memset(b, 0, self.size * sizeof(float));
}

// Milestone 0: the dry sound only. Per sample with nothing kept between
// calls, so Ingen's slices (see docs/design.md) give the same output as one
// run() per period. Input and outputs may be the same buffer.
void run(LV2_Handle handle, uint32_t frames) {
    auto& self = *static_cast<Plugin*>(handle);
    const float* input = self.audio[in];
    float* left = self.audio[out_l];
    float* right = self.audio[out_r];
    if (!input || !left || !right) return;
    const float dry_gain = control(self.ports, dry);
    for (uint32_t i = 0; i < frames; ++i) {
        const float x = dry_gain * input[i];
        left[i] = x;
        right[i] = x;
    }
}

void cleanup(LV2_Handle handle) { delete static_cast<Plugin*>(handle); }

const LV2_Descriptor descriptor = {kURI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr};
} // namespace
} // namespace euclidelay

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &euclidelay::descriptor : nullptr;
}
