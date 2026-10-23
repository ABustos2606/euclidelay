// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// One voice on its own: an impulse in gives impulses exactly at D_k (pass 0,
// without step 0) and at m * L + D_k (pass m) at Feedback^m, each within
// m / 2 + 1 samples of the ideal time, and nothing anywhere else. Feedback 0
// plays one pass, Tone 1 leaves every impulse intact, Tone 0 darkens each pass
// more, Hits 0 is silent.
#include "../src/voice.hpp"
#include "../src/ports.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace euclidelay;
namespace {
constexpr double rate = 48000;
// 1/8 triplet at 97 BPM: 9896.9 samples, so every time is rounded.
const double S = step_samples(1.0 / 3, 97, rate);

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

// An impulse at sample 0 through a fresh voice, `frames` samples of output.
std::vector<float> impulse(Voice& voice, const Taps& taps, float feedback, float tone, long frames) {
    voice.clear();
    voice.start(taps, feedback);
    const std::vector<float> pole(kChunk, tone_pole(tone, rate));
    std::vector<float> x(kChunk, 0.0f), out(frames);
    x[0] = 1.0f;
    for (long t = 0; t < frames; t += kChunk) {
        const int n = static_cast<int>(std::min<long>(kChunk, frames - t));
        voice.run(x.data(), pole.data(), out.data() + t, n);
        x[0] = 0.0f;
    }
    return out;
}

void check_echoes(Voice& voice, int steps, int hits, int shift, float feedback, int passes) {
    const std::string name = std::to_string(steps) + "/" + std::to_string(hits) + "/" + std::to_string(shift) +
                             ", Feedback " + std::to_string(feedback);
    const uint32_t mask = pattern(steps, hits, shift);
    const Taps taps = make_taps(mask, steps, S);
    const long frames = static_cast<long>(passes) * taps.loop;  // up to the next pass
    const std::vector<float> out = impulse(voice, taps, feedback, 1.0f, frames);

    std::map<long, double> expected;  // sample -> level
    for (int m = 0; m < passes; ++m)
        for (int k = 0, tap = 0; k < steps; ++k) {
            if (!(mask & (1u << k))) continue;
            const long d = k ? taps.delay[tap++] : 0;
            if (m == 0 && k == 0) continue;  // pass 0's step 0 is the note itself
            const long t = static_cast<long>(m) * taps.loop + d;
            const double ideal = (static_cast<double>(m) * steps + k) * S;
            require(std::fabs(t - ideal) <= m / 2.0 + 1, name + ": pass " + std::to_string(m) + " hit " +
                                                             std::to_string(k) + " is too far from its ideal time");
            expected[t] = std::pow(feedback, m);
        }
    for (long t = 0; t < frames; ++t) {
        const auto e = expected.find(t);
        const double want = e == expected.end() ? 0.0 : e->second;
        require(std::fabs(out[t] - want) <= 1e-6 * (want + 1e-3),
                name + ": at sample " + std::to_string(t) + " expected " + std::to_string(want) + ", got " +
                    std::to_string(out[t]));
    }
}
}

int main() {
    try {
        Voice voice;
        require(voice.allocate(kBufferSeconds, rate), "allocate");
        require(tone_pole(1.0f, rate) == 0.0f, "Tone 1 leaves the loop untouched");

        check_echoes(voice, 7, 4, 0, 0.5f, 4);
        check_echoes(voice, 4, 3, 0, 0.7f, 4);
        check_echoes(voice, 1, 1, 0, 0.9f, 6);
        check_echoes(voice, 16, 16, 0, 0.5f, 3);
        check_echoes(voice, 5, 2, 1, 0.5f, 4);   // the note on a rest
        check_echoes(voice, 16, 5, 3, 0.95f, 3);
        check_echoes(voice, 7, 4, 0, 0.0f, 3);   // Feedback 0: pass 0 only
        check_echoes(voice, 4, 0, 0, 0.9f, 3);   // Hits 0: silent

        // Longest loop: 16 steps of 1/4 at 30 BPM fits the buffer.
        {
            const Taps longest = make_taps(pattern(16, 1, 15), 16, step_samples(1, 30, rate));
            require(longest.loop == 32 * 48000 && static_cast<std::size_t>(longest.loop) < voice.size(),
                    "the longest loop fits the buffer");
            const std::vector<float> out = impulse(voice, longest, 0.5f, 1.0f, 2 * longest.loop + 1);
            require(out[longest.delay[0]] == 1.0f && out[longest.loop + longest.delay[0]] == 0.5f,
                    "the longest loop: Shift 15 echoes at 15 steps, then a loop later");
        }

        // Tone 0, Steps 1 Hits 1: every pass is filtered once more. The
        // filter keeps the level of low frequencies (each pass sums to
        // Feedback^m) but each pass is smeared more: its peak falls further
        // below its sum.
        {
            const float fb = 0.9f;
            const Taps taps = make_taps(pattern(1, 1, 0), 1, S);
            const std::vector<float> out = impulse(voice, taps, fb, 0.0f, 6L * taps.loop);
            double last_ratio = 1.01;
            for (int m = 1; m < 6; ++m) {
                double sum = 0, peak = 0;
                for (long t = static_cast<long>(m) * taps.loop; t < (m + 1L) * taps.loop; ++t) {
                    sum += out[t];
                    peak = std::fmax(peak, out[t]);
                }
                require(std::fabs(sum - std::pow(fb, m)) < 1e-4, "Tone 0: pass " + std::to_string(m) + " keeps its level");
                const double ratio = peak / sum;
                require(ratio < last_ratio * 0.95, "Tone 0: pass " + std::to_string(m) + " is darker than the last");
                last_ratio = ratio;
            }
            require(std::fabs(out[0]) == 0 && out[taps.loop - 1] == 0, "Tone 0: pass 0 is the note itself, not played");
        }

        std::puts("PASS test_voice: impulses at D_k and m * L + D_k at Feedback^m; Feedback 0; Tone 1 and 0; Hits 0");
    } catch (const std::exception& e) { std::printf("FAIL test_voice: %s\n", e.what()); return 1; }
}
