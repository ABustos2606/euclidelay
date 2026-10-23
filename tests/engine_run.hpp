// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// What test_engine and test_voices share: one engine, run in blocks with
// settings that may change between blocks, and test signals.
#pragma once
#include "../src/engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace euclidelay::test {

constexpr double rate = 48000;
// 1/32 at 300 BPM: 1200 samples a step (kStep).
constexpr uint32_t kStep = 1200;

inline void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Output {
    std::vector<float> left, right;
};

// One engine for the whole test, 25 MB: activate empties it.
inline Engine& engine() {
    static Engine e;
    static const bool ok = e.allocate(rate);
    require(ok, "allocate");
    return e;
}

// `x` through the engine, activated with at(0), in blocks of `block` frames;
// at(frame) gives the settings for the block starting at `frame`.
inline Output play(const std::vector<float>& x, const std::function<Settings(uint32_t)>& at, uint32_t block = 64) {
    Engine& e = engine();
    const uint32_t length = static_cast<uint32_t>(x.size());
    Output out{std::vector<float>(length, NAN), std::vector<float>(length, NAN)};
    e.activate(at(0));
    for (uint32_t start = 0; start < length; start += block) {
        const uint32_t count = std::min(block, length - start);
        e.run(x.data() + start, out.left.data() + start, out.right.data() + start, count, at(start));
    }
    return out;
}

inline Output play(const std::vector<float>& x, const Settings& s) {
    return play(x, [&](uint32_t) { return s; });
}

// The defaults at 1/32 and 300 BPM, Tone 1 (impulses stay whole), Dry 0,
// Echoes 1 and every voice off.
inline Settings quiet() {
    Settings s = default_settings();
    s.step = 0;
    s.bpm = 300;
    s.tone = 1;
    s.dry = 0;
    s.echoes = 1;
    for (VoiceSettings& v : s.voice) v.on = false;
    return s;
}

inline uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline std::vector<float> noise(uint32_t length, float amplitude, uint32_t seed = 12345) {
    std::vector<float> x(length);
    for (float& v : x) v = (static_cast<float>(xorshift(seed) % 20001) / 10000.0f - 1.0f) * amplitude;
    return x;
}

inline std::vector<float> sine(uint32_t length, float amplitude, double hz) {
    std::vector<float> x(length);
    for (uint32_t i = 0; i < length; ++i) x[i] = static_cast<float>(amplitude * std::sin(2 * M_PI * hz * i / rate));
    return x;
}

// The largest second difference of `y` in [from, to): a click is a sudden
// change of slope, which a smooth signal or a fade doesn't have.
inline double worst_kink(const std::vector<float>& y, uint32_t from, uint32_t to) {
    double worst = 0;
    for (uint32_t i = std::max<uint32_t>(from, 2); i < to && i < y.size(); ++i)
        worst = std::fmax(worst, std::fabs(double(y[i]) - 2.0 * y[i - 1] + y[i - 2]));
    return worst;
}

} // namespace euclidelay::test
