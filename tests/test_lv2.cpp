// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// The built plugin, run as Ingen runs it: each period split into slices at
// random points, the audio pointers moved to each slice. With fixed controls
// the output must match the same input run unsliced, sample for sample, as
// other hosts run it. Milestone 0 also checks the output is Dry * In on both
// sides.
//   test_lv2 build/euclidelay.lv2/euclidelay.so
#include "../src/ports.hpp"
#include <lv2/core/lv2.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace euclidelay;
namespace {
void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
constexpr uint32_t block = 128, periods = 200, length = block * periods;
using Controls = std::array<float, port_count>;

Controls defaults() {
    Controls c{};
    for (uint32_t i = 0; i < port_count; ++i) c[i] = kPorts[i].def;
    return c;
}

uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// A guitar-like level of noise with a few full-scale and silent stretches.
std::vector<float> input() {
    std::vector<float> x(length);
    uint32_t s = 12345;
    for (uint32_t i = 0; i < length; ++i) x[i] = (float(xorshift(s) % 20001) / 10000.0f - 1.0f) * 0.3f;
    std::fill(x.begin() + 3000, x.begin() + 3100, 1.0f);
    std::fill(x.begin() + 5000, x.begin() + 9000, 0.0f);
    return x;
}

struct Output { std::vector<float> left, right; };

// Runs `x` through a fresh instance. sliced: each period is cut at random
// points, as Ingen does on a control change. in_place: Out L is the input
// buffer itself.
Output play(const LV2_Descriptor* d, const std::vector<float>& x, Controls controls, bool sliced, bool in_place) {
    LV2_Handle h = d->instantiate(d, 48000, ".", nullptr);
    require(h, "instantiate at 48 kHz");
    for (uint32_t i = 0; i < port_count; ++i)
        if (kPorts[i].kind == Kind::control_in) d->connect_port(h, i, &controls[i]);
    d->activate(h);
    Output out{std::vector<float>(length, NAN), std::vector<float>(length, NAN)};
    std::vector<float> buffer = x;  // in-place: the host's single buffer
    uint32_t rng = 0x9E3779B9;
    for (uint32_t start = 0; start < length; start += block) {
        for (uint32_t offset = 0; offset < block;) {
            const uint32_t count = sliced ? std::min(1 + xorshift(rng) % 40, block - offset) : block;
            const uint32_t f = start + offset;
            if (in_place) {
                d->connect_port(h, in, buffer.data() + f);
                d->connect_port(h, out_l, buffer.data() + f);
            } else {
                d->connect_port(h, in, const_cast<float*>(x.data()) + f);
                d->connect_port(h, out_l, out.left.data() + f);
            }
            d->connect_port(h, out_r, out.right.data() + f);
            d->run(h, count);
            offset += count;
        }
    }
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);
    if (in_place) out.left = buffer;
    return out;
}

void same(const Output& a, const Output& b, const std::string& what) {
    for (uint32_t i = 0; i < length; ++i)
        require(a.left[i] == b.left[i] && a.right[i] == b.right[i], what + ": differs at frame " + std::to_string(i));
}

void dry_only(const Output& o, const std::vector<float>& x, float dry_gain, const std::string& what) {
    for (uint32_t i = 0; i < length; ++i)
        require(o.left[i] == dry_gain * x[i] && o.right[i] == dry_gain * x[i],
                what + ": Dry * In at frame " + std::to_string(i) + ", got " + std::to_string(o.left[i]));
}
}

int main(int argc, char** argv) {
    try {
        require(argc > 1, "usage: test_lv2 PLUGIN.so");
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(library, dlerror() ? dlerror() : "dlopen");
        using Get = const LV2_Descriptor* (*)(uint32_t);
        const auto get = reinterpret_cast<Get>(dlsym(library, "lv2_descriptor"));
        require(get && get(0) && !get(1), "one descriptor");
        const LV2_Descriptor* d = get(0);
        require(std::strcmp(d->URI, kURI) == 0, "URI");
        require(!d->instantiate(d, 0, ".", nullptr), "instantiate refuses a sample rate of 0");
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            LV2_Handle h = d->instantiate(d, rate, ".", nullptr);
            require(h, "instantiate at " + std::to_string(int(rate)) + " Hz");
            d->cleanup(h);  // never activated or run: cleanup alone must free it
        }

        const std::vector<float> x = input();
        Controls controls = defaults();
        const Output reference = play(d, x, controls, false, false);
        dry_only(reference, x, 1.0f, "defaults, unsliced");
        same(play(d, x, controls, true, false), reference, "defaults, sliced as Ingen runs it");
        same(play(d, x, controls, true, true), reference, "defaults, sliced, in place");

        controls[dry] = 0.7f;
        controls[voice_port(2, voice)] = 1;
        controls[voice_port(3, voice)] = 1;
        controls[bpm] = 300;
        controls[step] = 0;
        const Output changed = play(d, x, controls, false, false);
        dry_only(changed, x, 0.7f, "Dry 0.7, unsliced");
        same(play(d, x, controls, true, false), changed, "Dry 0.7, sliced as Ingen runs it");

        controls[dry] = NAN;
        dry_only(play(d, x, controls, true, false), x, 1.0f, "a non-finite Dry counts as its default");
        controls[dry] = 5;
        dry_only(play(d, x, controls, true, false), x, 1.0f, "Dry above its range is clamped");

        std::puts("PASS test_lv2: one descriptor and URI; sliced as Ingen runs it, unsliced and in place give the same output, Dry * In");
        dlclose(library);
    } catch (const std::exception& e) { std::printf("FAIL test_lv2: %s\n", e.what()); return 1; }
}
