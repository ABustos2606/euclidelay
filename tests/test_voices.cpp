// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Switching voices on and off: a voice switched off fades out within 20 ms
// and then reads and writes nothing (its buffer is untouched afterwards);
// switched on again after echoes were in its loop, it plays none of them,
// only what came in after; off and on again within the fade does the same
// once the fade is over, with the old echoes fading rather than cut; and no
// switching makes a click.
#include "engine_run.hpp"
#include <cstdio>
#include <cstring>

using namespace euclidelay;
using namespace euclidelay::test;
namespace {
constexpr uint32_t fade = 960;  // 20 ms
constexpr uint32_t off = 16 * kStep;

// Voice 1 alone at its defaults, switched by `on(frame)`.
std::function<Settings(uint32_t)> voice1(std::function<bool(uint32_t)> on) {
    return [on](uint32_t frame) {
        Settings s = quiet();
        s.voice[0].on = on(frame);
        return s;
    };
}

void fades_out_and_stops() {
    const std::vector<float> x = noise(40 * kStep, 0.1f);
    const auto at = voice1([](uint32_t f) { return f < off; });
    const Output o = play(x, at);
    double before = 0;
    for (uint32_t i = off - kStep; i < off; ++i) before = std::fmax(before, std::fabs(o.left[i]));
    require(before > 0.01, "voice 1 plays before it is switched off");
    require(o.left[off + fade / 2] != 0, "voice 1 fades out rather than stopping at once");
    for (uint32_t i = off + fade; i < x.size(); ++i)
        require(o.left[i] == 0 && o.right[i] == 0, "voice 1 is silent 20 ms after Off, at " + std::to_string(i));
    // Its buffer just after the fade is its buffer at the end: nothing more
    // was written.
    const Voice& v = engine().voice(0);
    const std::vector<float> early(x.begin(), x.begin() + off + fade + 64);
    play(early, at);
    std::vector<float> snapshot(v.data(), v.data() + v.size());
    play(x, at);
    require(!v.active(), "voice 1 is off");
    require(std::memcmp(snapshot.data(), v.data(), v.size() * sizeof(float)) == 0,
            "an Off voice writes nothing into its buffer");
}

// Voice 1 on, off at `off`, on again at `back`, noise all along; against a
// voice that was off until `back` with silence before it. From the end of
// the old echoes' fade, the two are the same: none of the old echoes play.
void comes_back_empty(uint32_t back, const std::string& what) {
    const uint32_t length = back + 30 * kStep;
    const std::vector<float> x = noise(length, 0.1f, 4242);
    const Output a = play(x, voice1([&](uint32_t f) { return f < off || f >= back; }));
    std::vector<float> silent_before = x;
    std::fill(silent_before.begin(), silent_before.begin() + back, 0.0f);
    const Output b = play(silent_before, voice1([&](uint32_t f) { return f >= back; }));
    const uint32_t settled = std::max(back, off + fade);
    for (uint32_t i = settled; i < length; ++i)
        require(a.left[i] == b.left[i] && a.right[i] == b.right[i],
                what + ": plays something from before it came on, at " + std::to_string(i));
    double echoes = 0;
    for (uint32_t i = back; i < length; ++i) echoes = std::fmax(echoes, std::fabs(b.left[i]));
    require(echoes > 0.01, what + ": it plays what came after");
    if (back < off + fade) {
        // Within the fade: the old echoes carry on fading out, not cut.
        double old = 0;
        for (uint32_t i = back; i < settled; ++i) old = std::fmax(old, std::fabs(a.left[i] - b.left[i]));
        require(old > 0.001, what + ": the old echoes fade out");
    }
}

// A held sine through all four voices while they are switched in every
// way: no sample-to-sample jump (second difference) above -40 dB of the
// input, where a cut would be about the size of the echoes themselves.
void no_clicks() {
    const float amplitude = 0.05f;
    const uint32_t length = 80 * kStep;
    std::vector<float> x = sine(length, amplitude, 100);
    // Faded in smoothly, so its own start isn't a click in its echoes.
    for (uint32_t i = 0; i < kStep; ++i) x[i] *= static_cast<float>(0.5 - 0.5 * std::cos(M_PI * i / kStep));
    const auto at = [](uint32_t f) {
        Settings s = quiet();
        s.tone = 0.7f;
        s.voice[0].on = f < 20 * kStep || f >= 20 * kStep + 320;  // off and on within the fade
        s.voice[1].on = f < 30 * kStep || f >= 45 * kStep;         // off and on after a while
        s.voice[2].on = f >= 10 * kStep && f < 60 * kStep;         // on, then off
        s.voice[3].on = f >= 25 * kStep;                          // on
        return s;
    };
    const Output o = play(x, at, 128);
    double loudest = 0;
    for (float v : o.left) loudest = std::fmax(loudest, std::fabs(v));
    require(loudest > 0.05, "the echoes of the held sine are loud enough to hear a click");
    const double bound = 0.01 * amplitude;
    const double kink = std::fmax(worst_kink(o.left, 0, length), worst_kink(o.right, 0, length));
    require(kink < bound, "a click while switching voices: " + std::to_string(kink) + " against " + std::to_string(bound));
    // The same without fades would click: the bound can tell.
    std::vector<float> cut = o.left;
    uint32_t at_peak = 30 * kStep;
    for (uint32_t i = at_peak; i < 40 * kStep; ++i)
        if (std::fabs(cut[i]) > std::fabs(cut[at_peak])) at_peak = i;
    for (uint32_t i = at_peak; i < length; ++i) cut[i] = 0;
    require(worst_kink(cut, 0, length) > bound, "the bound catches a cut");
}
}

int main() {
    try {
        fades_out_and_stops();
        comes_back_empty(off + 10 * kStep, "on again after a while");
        comes_back_empty(off + 320, "off and on again within the fade");
        no_clicks();
        std::puts("PASS test_voices: Off fades within 20 ms and then writes nothing; On starts empty, also within the "
                  "fade; no clicks while switching");
    } catch (const std::exception& e) { std::printf("FAIL test_voices: %s\n", e.what()); return 1; }
}
