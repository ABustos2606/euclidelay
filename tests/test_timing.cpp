// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Ten simulated minutes of the built plugin at 48 kHz in 128-frame periods,
// with every control changed at random every few periods (each change
// slicing its period, as Ingen does): the output is always finite and below
// what Dry and the clipper allow, and the time of each period is measured.
// The 99.9th percentile must stay under 25% of the period on the build
// machine; the worst period is printed too (desktop scheduling, not a Pi
// xrun result). Sanitizer builds only check the output.
//   test_timing build/euclidelay.lv2/euclidelay.so
#include "../src/ports.hpp"
#include <lv2/core/lv2.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SANITIZED 1
#endif
#endif

using namespace euclidelay;
namespace {
void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

constexpr double rate = 48000;
constexpr uint32_t block = 128;
constexpr uint64_t periods = static_cast<uint64_t>(600 * rate / block);  // 10 minutes

float random_in(uint32_t& rng, uint32_t port) {
    const PortSpec& p = kPorts[port];
    return p.min + (p.max - p.min) * static_cast<float>(xorshift(rng) % 10001) / 10000.0f;
}

// A new value for a few controls; voices are mostly on, where it costs most.
void change(uint32_t& rng, std::array<float, port_count>& c) {
    const uint32_t count = 1 + xorshift(rng) % 6;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t port = enabled + xorshift(rng) % (port_count - enabled);
        float value = random_in(rng, port);
        const bool toggle = port == enabled || (port >= first_voice && (port - first_voice) % kVoicePorts == voice);
        if (toggle) value = xorshift(rng) % 8 ? 1.0f : 0.0f;
        else if (port == step || (port >= first_voice && (port - first_voice) % kVoicePorts <= shift)) value = std::round(value);
        c[port] = value;
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc > 1, "usage: test_timing PLUGIN.so");
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(library, dlerror() ? dlerror() : "dlopen");
        using Get = const LV2_Descriptor* (*)(uint32_t);
        const auto get = reinterpret_cast<Get>(dlsym(library, "lv2_descriptor"));
        require(get && get(0), "descriptor");
        const LV2_Descriptor* d = get(0);
        LV2_Handle h = d->instantiate(d, rate, ".", nullptr);
        require(h, "instantiate");

        std::array<float, port_count> controls{};
        for (uint32_t i = 0; i < port_count; ++i) controls[i] = kPorts[i].def;
        for (uint32_t n = 0; n < kVoices; ++n) controls[voice_port(n, voice)] = 1;
        for (uint32_t i = 0; i < port_count; ++i)
            if (kPorts[i].kind == Kind::control_in) d->connect_port(h, i, &controls[i]);
        std::vector<float> input(block), left(block), right(block);
        d->activate(h);

        uint32_t rng = 31337, noise = 777;
        uint64_t frame = 0, next_change = 0;
        std::vector<double> times;
        times.reserve(periods);
        for (uint64_t p = 0; p < periods; ++p) {
            // Plucked notes: noise decaying over half a second, twice a second,
            // now and then at full scale.
            for (uint32_t i = 0; i < block; ++i, ++frame) {
                const double t = static_cast<double>(frame % 24000) / rate;
                const float level = (frame / 24000) % 17 == 0 ? 1.0f : 0.3f;
                input[i] = level * static_cast<float>(std::exp(-6 * t)) *
                        (static_cast<float>(xorshift(noise) % 20001) / 10000.0f - 1.0f);
            }
            // A control change slices the period where it lands.
            uint32_t cut = block;
            if (p >= next_change) {
                cut = xorshift(rng) % block;
                next_change = p + 1 + xorshift(rng) % 8;
            }
            const auto begin = std::chrono::steady_clock::now();
            uint32_t at = 0;
            if (cut < block) {
                if (cut > 0) {
                    d->connect_port(h, in, input.data());
                    d->connect_port(h, out_l, left.data());
                    d->connect_port(h, out_r, right.data());
                    d->run(h, cut);
                }
                change(rng, controls);
                at = cut;
            }
            d->connect_port(h, in, input.data() + at);
            d->connect_port(h, out_l, left.data() + at);
            d->connect_port(h, out_r, right.data() + at);
            d->run(h, block - at);
            times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
            for (uint32_t i = 0; i < block; ++i) {
                // Dry at most 1 (unity while off) plus Echoes at most 1 times
                // the clipper's ceiling.
                const float most = std::fabs(input[i]) + 0.98f + 1e-6f;
                require(std::isfinite(left[i]) && std::isfinite(right[i]) && std::fabs(left[i]) <= most &&
                            std::fabs(right[i]) <= most,
                        "output out of bounds at frame " + std::to_string(frame - block + i));
            }
        }
        d->cleanup(h);
        dlclose(library);

        std::sort(times.begin(), times.end());
        const double period_us = block / rate * 1e6, budget = 0.25 * period_us;
        const double p999 = times[times.size() * 999 / 1000], worst = times.back();
        std::printf("  %llu periods of %u frames: median %.1f us, p99.9 %.1f us, worst %.1f us; budget %.0f us "
                    "(25%% of %.0f us)\n",
                    static_cast<unsigned long long>(periods), block, times[times.size() / 2], p999, worst, budget,
                    period_us);
#ifndef SANITIZED
        require(p999 < budget, "p99.9 over budget");
#endif
        std::puts("PASS test_timing: 10 simulated minutes of random control changes, finite and bounded output");
    } catch (const std::exception& e) { std::printf("FAIL test_timing: %s\n", e.what()); return 1; }
}
