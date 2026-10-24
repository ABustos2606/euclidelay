// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// The four voices and the mix: the pan law at -1, 0 and 1; Dry and Echoes;
// every one of the 16 on/off combinations of the voices gives the sum of
// exactly the voices that are on, and all off gives the dry sound only;
// Enabled off lets the loops ring out with the dry sound at unity, and back on
// lets the input in again; a held full-scale sine with all voices at 16 hits
// stays under 0 dBFS through the clipper; a NaN input clears the voices and
// the output stays finite.
#include "engine_run.hpp"
#include <cstdio>

using namespace euclidelay;
using namespace euclidelay::test;
namespace {
constexpr uint32_t length = 40 * kStep;

std::vector<float> impulse(uint32_t at, float level, uint32_t frames = length) {
    std::vector<float> x(frames, 0.0f);
    x[at] = level;
    return x;
}

void near(double got, double want, double tolerance, const std::string& what) {
    require(std::fabs(got - want) <= tolerance,
            what + ": expected " + std::to_string(want) + ", got " + std::to_string(got));
}

void finite(const Output& o, const std::string& what) {
    for (size_t i = 0; i < o.left.size(); ++i)
        require(std::isfinite(o.left[i]) && std::isfinite(o.right[i]), what + ": non-finite output at " + std::to_string(i));
}

void pan_law() {
    // Voice 1 alone at its defaults (8 steps, 3 hits: x.x..x..): the first
    // echo is 2 steps after the note, at full level.
    for (const float pan : {-1.0f, 0.0f, 1.0f}) {
        Settings s = quiet();
        s.voice[0].on = true;
        s.voice[0].pan = pan;
        const Output o = play(impulse(0, 0.25f), s);
        const std::string what = "pan " + std::to_string(pan);
        const double angle = (pan + 1) * M_PI / 4;
        near(o.left[2 * kStep], 0.25 * std::cos(angle), 1e-7, what + ", left");
        near(o.right[2 * kStep], 0.25 * std::sin(angle), 1e-7, what + ", right");
    }
    // The ends are one side only, the centre -3 dB on each.
    Settings s = quiet();
    s.voice[0].on = true;
    s.voice[0].pan = 0;
    const Output o = play(impulse(0, 0.25f), s);
    near(o.left[2 * kStep], 0.25 / std::sqrt(2.0), 1e-7, "a centred voice is -3 dB on the left");
    near(o.right[2 * kStep], 0.25 / std::sqrt(2.0), 1e-7, "a centred voice is -3 dB on the right");
}

void dry_and_echoes() {
    // Below the clipper's knee the mix is linear: Dry 0.3 and Echoes 0.5
    // give 0.3 * x plus half the echoes of Dry 0, Echoes 1.
    std::vector<float> x = noise(length, 0.02f);
    x[100] = 0.25f;
    Settings s = default_settings();
    s.step = 0;
    s.bpm = 300;
    s.dry = 0;
    s.echoes = 1;
    const Output wet = play(x, s);
    s.dry = 0.3f;
    s.echoes = 0.5f;
    const Output mixed = play(x, s);
    double loudest = 0;
    for (uint32_t i = 0; i < length; ++i) {
        loudest = std::fmax(loudest, std::fmax(std::fabs(wet.left[i]), std::fabs(wet.right[i])));
        near(mixed.left[i], 0.3 * x[i] + 0.5 * wet.left[i], 1e-6, "Dry 0.3, Echoes 0.5, left at " + std::to_string(i));
        near(mixed.right[i], 0.3 * x[i] + 0.5 * wet.right[i], 1e-6, "Dry 0.3, Echoes 0.5, right at " + std::to_string(i));
    }
    require(loudest > 0.05 && loudest < kClipKnee, "the echoes are there and stay below the clipper's knee");
}

// Every voice on its own, then every combination: the sum, and all off is
// the dry sound alone.
void combinations() {
    std::vector<float> x = noise(length, 0.02f, 777);
    x[50] = 0.1f;
    Settings s = quiet();
    s.dry = 0.5f;
    const float pans[kVoices] = {-0.6f, 0.6f, -0.2f, 0.2f};
    for (uint32_t n = 0; n < kVoices; ++n) s.voice[n].pan = pans[n];
    Output alone[kVoices];
    for (uint32_t n = 0; n < kVoices; ++n) {
        Settings one = s;
        one.dry = 0;
        one.voice[n].on = true;
        alone[n] = play(x, one);
    }
    for (uint32_t mask = 0; mask < 16; ++mask) {
        Settings some = s;
        for (uint32_t n = 0; n < kVoices; ++n) some.voice[n].on = mask & (1u << n);
        const Output o = play(x, some);
        const std::string what = "voices " + std::to_string(mask & 1) + std::to_string(mask >> 1 & 1) +
                                 std::to_string(mask >> 2 & 1) + std::to_string(mask >> 3 & 1);
        for (uint32_t i = 0; i < length; ++i) {
            double l = 0.5 * x[i], r = 0.5 * x[i];
            for (uint32_t n = 0; n < kVoices; ++n)
                if (mask & (1u << n)) l += alone[n].left[i], r += alone[n].right[i];
            if (mask == 0) {
                require(o.left[i] == 0.5f * x[i] && o.right[i] == 0.5f * x[i], what + ": dry only at " + std::to_string(i));
                continue;
            }
            require(std::fabs(l - 0.5 * x[i]) < kClipKnee, what + ": below the clipper's knee");
            near(o.left[i], l, 1e-6, what + ", left at " + std::to_string(i));
            near(o.right[i], r, 1e-6, what + ", right at " + std::to_string(i));
        }
    }
    for (uint32_t n = 0; n < kVoices; ++n) {
        double loudest = 0;
        for (float v : alone[n].left) loudest = std::fmax(loudest, std::fabs(v));
        require(loudest > 0.01, "voice " + std::to_string(n + 1) + " plays on its own");
    }
}

// Enabled off: the input stops entering the loops (over 10 ms), the dry sound
// is at unity whatever Dry is, and what was in the loops rings out. Back on:
// the input enters again and Dry applies.
void enabled_port() {
    const uint32_t off = 4 * kStep, fade = 480, on = 32 * kStep;
    const auto at = [&](uint32_t frame) {
        Settings s = default_settings();
        s.step = 0;
        s.bpm = 300;
        s.tone = 1;
        s.dry = 0.3f;
        s.enabled = frame < off || frame >= on;
        return s;
    };
    const std::vector<float> note = impulse(0, 0.25f);
    const Output a = play(note, at);
    // The same, with noise while off: it reaches the outputs at unity and
    // nothing else.
    std::vector<float> noisy = note;
    const std::vector<float> n = noise(length, 0.1f);
    for (uint32_t i = off + fade; i < on; ++i) noisy[i] = n[i];
    const Output b = play(noisy, at);
    double trails = 0;
    for (uint32_t i = off + fade; i < on; ++i) {
        near(b.left[i] - a.left[i], n[i], 1e-6, "Enabled off: dry at unity and no input into the loops, left at " + std::to_string(i));
        near(b.right[i] - a.right[i], n[i], 1e-6, "Enabled off: dry at unity and no input into the loops, right at " + std::to_string(i));
        trails = std::fmax(trails, std::fabs(a.left[i]) + std::fabs(a.right[i]));
    }
    require(trails > 0.01, "Enabled off: the echoes ring out");
    // Back on: a second note gets its own echoes and Dry again.
    std::vector<float> two = note;
    const uint32_t second = on + 1000;
    two[second] = 0.25f;
    const Output c = play(two, at);
    near(c.left[second] - a.left[second], 0.3 * 0.25, 1e-6, "Enabled on again: Dry applies");
    // Voices 1 (x.x..x..) and 2 (x.x..) both echo 2 steps later.
    const double angle1 = (kPorts[voice_port(0, pan)].def + 1) * M_PI / 4;
    const double angle2 = (kPorts[voice_port(1, pan)].def + 1) * M_PI / 4;
    near(c.left[second + 2 * kStep] - a.left[second + 2 * kStep], 0.8 * 0.25 * (std::cos(angle1) + std::cos(angle2)), 1e-6,
         "Enabled on again: the input enters the loops");
}

void clipper() {
    require(soft_clip(0.49f) == 0.49f && soft_clip(-0.5f) == -0.5f, "the clipper is transparent below its knee");
    float last = 0;
    for (float s = 0; s < 40; s += 0.001f) {
        const float c = soft_clip(s);
        require(c >= last && c <= kClipCeiling && soft_clip(-s) == -c, "the clipper rises, stays below its ceiling and is symmetric");
        require(c - last <= 0.0011f, "the clipper has no step");
        last = c;
    }
    require(soft_clip(INFINITY) == kClipCeiling, "the clipper holds an infinite sum at full scale");
    // All four voices at 16 hits, Feedback 0.95, on a held full-scale sine.
    Settings s = quiet();
    for (VoiceSettings& v : s.voice) {
        v.on = true;
        v.steps = v.hits = 16;
        v.feedback = 0.95f;
    }
    const Output o = play(sine(length, 1.0f, 200), s);
    double loudest = 0;
    // std::max, not std::fmax: Debian 12's GCC 12 on arm64 crashes vectorising this loop.
    for (uint32_t i = 0; i < length; ++i)
        loudest = std::max(loudest, double(std::max(std::fabs(o.left[i]), std::fabs(o.right[i]))));
    require(loudest < 1.0 && loudest > 0.9, "a full-scale chord through every voice stays under 0 dBFS (" +
                                                std::to_string(loudest) + ")");
}

void non_finite() {
    // A NaN in the input: every voice is emptied, the outputs stay finite and
    // nothing plays until a new note arrives.
    Settings s = default_settings();
    s.step = 0;
    s.bpm = 300;
    s.tone = 1;
    for (VoiceSettings& v : s.voice) v.on = true;
    std::vector<float> x = impulse(0, 0.25f);
    const uint32_t bad = 5000, later = 20000;
    x[bad] = NAN;
    x[later] = 0.25f;
    const Output o = play(x, s);
    finite(o, "a NaN input");
    for (uint32_t i = bad; i < later; ++i)
        require(o.left[i] == 0 && o.right[i] == 0, "a NaN input empties the voices, at " + std::to_string(i));
    require(o.left[later + 2 * kStep] != 0, "a new note after a NaN plays its echoes");
    x[bad] = INFINITY;
    finite(play(x, s), "an infinite input");
    // An input so loud that the loops overflow.
    for (VoiceSettings& v : s.voice) {
        v.steps = v.hits = 16;
        v.feedback = 0.95f;
    }
    finite(play(std::vector<float>(length, 1e38f), s), "an input of 1e38");
}
}

int main() {
    try {
        pan_law();
        dry_and_echoes();
        combinations();
        enabled_port();
        clipper();
        non_finite();
        std::puts("PASS test_engine: pan law; Dry and Echoes; the 16 voice combinations; Enabled trails and dry at "
                  "unity; the clipper under 0 dBFS; non-finite input");
    } catch (const std::exception& e) { std::printf("FAIL test_engine: %s\n", e.what()); return 1; }
}
