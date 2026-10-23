// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "engine.hpp"
#include "ports.hpp"
#include <lv2/core/lv2.h>
#include <cstdint>
#include <new>

namespace euclidelay {
namespace {
struct Plugin {
    float* audio[3]{};             // in, out_l, out_r
    const float* ports[port_count]{};
    Engine engine;
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    if (!(rate > 0)) return nullptr;
    auto* self = new (std::nothrow) Plugin;
    if (!self) return nullptr;
    // All the memory Euclidelay will ever use, now: never in run().
    if (!self->engine.allocate(rate)) {
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
    self.engine.activate(read_settings(self.ports));
}

// Controls are read once, at the start of each slice (see docs/design.md,
// "Host limits"); the engine carries on across calls.
void run(LV2_Handle handle, uint32_t frames) {
    auto& self = *static_cast<Plugin*>(handle);
    const float* input = self.audio[in];
    float* left = self.audio[out_l];
    float* right = self.audio[out_r];
    if (!input || !left || !right) return;
    self.engine.run(input, left, right, frames, read_settings(self.ports));
}

void cleanup(LV2_Handle handle) { delete static_cast<Plugin*>(handle); }

const LV2_Descriptor descriptor = {kURI, instantiate, connect_port, activate, run, nullptr, cleanup, nullptr};
} // namespace
} // namespace euclidelay

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &euclidelay::descriptor : nullptr;
}
