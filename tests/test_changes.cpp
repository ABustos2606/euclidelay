// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// Changes while playing. The block-wise voice gives the same output, sample
// for sample, as the plain per-sample voice kept here, under random changes
// of every setting and random run lengths. Each crossfaded control (Steps,
// Hits, Shift, Step, BPM) and each gliding one (Level, Pan, Feedback, Tone,
// Dry, Echoes) changed during a held sine makes no sample-to-sample jump
// above -40 dB of the input, where the same change at once would; a knob
// dragged through 16 values costs at most two fades; a tempo change fades
// every voice that is on, once.
#include "engine_run.hpp"
#include <cstdio>
#include <cstring>

using namespace euclidelay;
using namespace euclidelay::test;
namespace {
int samples(double seconds, int at_least = 1) {
    const int n = static_cast<int>(std::lround(seconds * rate));
    return n < at_least ? at_least : n;
}

// One voice, one sample at a time, as docs/implementation-plan.md section
// 3.2 writes it: the reference for Voice::run. `fade_len` is the crossfade
// in samples; 1 makes every change of taps take effect at once.
class RefVoice {
public:
    RefVoice(std::size_t size, int fade_len) : buffer_(size, 0.0f), fade_len_(fade_len) {
        fade_ = samples(kSwitchSeconds);
        glide_ = samples(kGlideSeconds);
        clear();
    }
    void clear() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_ = 0;
        tone_ = old_tone_ = 0;
        on_ = true;
        fresh_ = size();
        old_age_ = 0;
        in_ramp_ = out_ramp_ = fade_;
        old_ramp_ = 0;
        fade_left_ = 0;
        pending_set_ = false;
    }
    void stop() { on_ = false; out_ramp_ = old_ramp_ = 0; }
    void switch_to(bool on) {
        if (on == on_) return;
        on_ = on;
        if (!on) return;
        old_ramp_ = out_ramp_;
        old_age_ = out_ramp_ > 0 ? fresh_ : 0;
        out_ramp_ = fade_;
        old_tone_ = tone_;
        fresh_ = 0;
        in_ramp_ = 0;
        tone_ = 0;
    }
    bool active() const { return on_ || out_ramp_ > 0 || old_ramp_ > 0; }
    void start(const Taps& t, float fb) {
        cur_ = next_ = t;
        pending_set_ = false;
        fade_left_ = 0;
        feedback_.reset(fb);
    }
    void set(const Taps& t, float fb) {
        feedback_.set(fb, glide_);
        const Taps& newest = pending_set_ ? pending_ : next_;
        if (t == newest) return;
        if (fade_left_ == 0) { next_ = t; fade_left_ = fade_len_; }
        else if (t == next_) pending_set_ = false;
        else { pending_ = t; pending_set_ = true; }
    }
    const std::vector<float>& buffer() const { return buffer_; }

    float process(float x, float p) {
        const long age = fresh_;
        x *= gain(in_ramp_);
        const bool fading = fade_left_ > 0;
        const float a = fading ? static_cast<float>(fade_len_ - fade_left_) * (1.0f / static_cast<float>(fade_len_)) : 0.0f;
        const float fb = feedback_.next();
        const float back_cur = cur_.loop <= age ? at(cur_.loop) : 0.0f;
        const float back_next = next_.loop <= age ? at(next_.loop) : 0.0f;
        const float back = back_cur + a * (back_next - back_cur);
        tone_ = back + p * (tone_ - back);
        if (std::fabs(tone_) < 1e-20f) tone_ = 0.0f;
        float looped = fb * tone_;
        float y = x + looped;
        if (!std::isfinite(y)) {
            y = looped = 0.0f;
            empty();
        }
        buffer_[write_] = y;
        const long lo = fresh_, hi = lo + old_age_;
        float now_cur = cur_.zero ? looped : 0.0f, now_next = next_.zero ? looped : 0.0f;
        float old_cur = 0, old_next = 0;
        const bool old = old_ramp_ > 0;
        if (old) {
            const float oc = cur_.loop > lo && cur_.loop <= hi ? at(cur_.loop) : 0.0f;
            const float on = next_.loop > lo && next_.loop <= hi ? at(next_.loop) : 0.0f;
            const float old_back = oc + a * (on - oc);
            old_tone_ = old_back + p * (old_tone_ - old_back);
            old_cur = cur_.zero ? fb * old_tone_ : 0.0f;
            old_next = next_.zero ? fb * old_tone_ : 0.0f;
        }
        for (int k = 0; k < cur_.count; ++k) {
            const long d = cur_.delay[k];
            if (d <= lo) now_cur += at(d);
            else if (old && d <= hi) old_cur += at(d);
        }
        for (int k = 0; k < next_.count; ++k) {
            const long d = next_.delay[k];
            if (d <= lo) now_next += at(d);
            else if (old && d <= hi) old_next += at(d);
        }
        float v = gain(out_ramp_) * (now_cur + a * (now_next - now_cur));
        if (old) v += gain(old_ramp_) * (old_cur + a * (old_next - old_cur));
        if (!std::isfinite(v)) {
            v = 0.0f;
            empty();
        }
        if (++write_ == size()) write_ = 0;
        if (fresh_ < size()) ++fresh_;
        if (in_ramp_ < fade_) ++in_ramp_;
        if (!on_ && out_ramp_ > 0) --out_ramp_;
        if (old_ramp_ > 0) --old_ramp_;
        if (fading && --fade_left_ == 0) {
            cur_ = next_;
            if (pending_set_) {
                pending_set_ = false;
                if (pending_ != cur_) { next_ = pending_; fade_left_ = fade_len_; }
            }
        }
        return v;
    }

private:
    long size() const { return static_cast<long>(buffer_.size()); }
    float at(long d) const { return buffer_[write_ - d < 0 ? write_ - d + size() : write_ - d]; }
    float gain(int ramp) const { return ramp >= fade_ ? 1.0f : static_cast<float>(ramp) * (1.0f / static_cast<float>(fade_)); }
    void empty() { fresh_ = 0; old_ramp_ = 0; in_ramp_ = 0; tone_ = 0.0f; }

    std::vector<float> buffer_;
    long write_ = 0;
    float tone_ = 0, old_tone_ = 0;
    Glide feedback_;
    int glide_ = 1;
    Taps cur_, next_, pending_;
    bool pending_set_ = false;
    int fade_left_ = 0, fade_len_ = 1;
    bool on_ = true;
    long fresh_ = 0, old_age_ = 0;
    int fade_ = 1, in_ramp_ = 1, out_ramp_ = 1, old_ramp_ = 0;
};

// Random settings of one voice: short loops mostly, so that changes of
// loop length, fades and lives overlap a lot.
struct Pick {
    int steps, hits, shift, step;
    float bpm, feedback;
    bool on;
};

Pick pick(uint32_t& rng) {
    Pick p;
    p.steps = 1 + xorshift(rng) % 16;
    p.hits = xorshift(rng) % 17;
    p.shift = xorshift(rng) % 16;
    p.step = xorshift(rng) % 3;
    p.bpm = 200 + xorshift(rng) % 101;
    p.feedback = (xorshift(rng) % 96) / 100.0f;
    p.on = xorshift(rng) % 5 != 0;
    return p;
}

// Voice::run in random run lengths against RefVoice sample by sample, with
// settings changed (or the voice switched) at random moments, as the engine
// applies them.
void blocks_match_samples() {
    Voice block;
    require(block.allocate(kBufferSeconds, rate), "allocate");
    RefVoice ref(block.size(), samples(kFadeSeconds));
    uint32_t rng = 2026;
    const long length = 40 * static_cast<long>(rate);
    std::vector<float> x(length);
    uint32_t seed = 99;
    for (long t = 0; t < length; ++t) {
        // Notes with silences between them.
        const long phase = t % 30000;
        x[t] = phase < 12000 ? (static_cast<float>(xorshift(seed) % 20001) / 10000.0f - 1.0f) * 0.2f : 0.0f;
    }
    Glide pole;
    pole.reset(tone_pole(0.7f, rate));
    float v[kChunk], poles[kChunk];
    long t = 0, next_change = 0;
    uint64_t compared = 0;
    while (t < length) {
        if (t >= next_change) {
            const Pick p = pick(rng);
            const Taps taps = make_taps(pattern(p.steps, p.hits, p.shift), p.steps,
                                        step_samples(kStepBeats[p.step], p.bpm, rate));
            const bool was = block.active();
            require(was == ref.active(), "block and per-sample voices agree on being active");
            block.switch_to(p.on);
            ref.switch_to(p.on);
            if (block.active()) {
                if (was) {
                    block.set(taps, p.feedback);
                    ref.set(taps, p.feedback);
                } else {
                    block.start(taps, p.feedback);
                    ref.start(taps, p.feedback);
                }
            }
            pole.set(tone_pole((xorshift(rng) % 101) / 100.0f, rate), samples(kGlideSeconds));
            // Mostly soon, so changes land inside fades; sometimes later.
            next_change = t + (xorshift(rng) % 4 ? 1 + xorshift(rng) % 2000 : 1 + xorshift(rng) % 40000);
        }
        const int n = static_cast<int>(std::min<long>({1 + static_cast<long>(xorshift(rng) % kChunk), length - t,
                                                       next_change - t}));
        for (int i = 0; i < n; ++i) poles[i] = pole.next();
        if (block.active()) block.run(x.data() + t, poles, v, n);
        else std::fill(v, v + n, 0.0f);
        for (int i = 0; i < n; ++i) {
            const float want = ref.active() ? ref.process(x[t + i], poles[i]) : 0.0f;
            require(v[i] == want, "block-wise output differs from per-sample at " + std::to_string(t + i) + ": " +
                                      std::to_string(v[i]) + " against " + std::to_string(want));
            if (want != 0) ++compared;
        }
        t += n;
    }
    require(compared > static_cast<uint64_t>(length) / 4, "the comparison heard echoes");
    require(std::memcmp(block.data(), ref.buffer().data(), block.size() * sizeof(float)) == 0,
            "block-wise and per-sample buffers are the same");
    require(block.fades() > 100, "the comparison went through many crossfades");
}

constexpr float amplitude = 0.05f;
constexpr double bound = 0.01 * amplitude;  // -40 dB of the input (tune)
constexpr uint32_t length = 60 * kStep;
constexpr uint32_t change = 30 * kStep + 136;  // near a peak of the 87 Hz sine

std::vector<float> held() {
    std::vector<float> x = sine(length, amplitude, 87);  // no whole number of periods in a step
    // Faded in smoothly, so its own start isn't a click in its echoes.
    for (uint32_t i = 0; i < kStep; ++i) x[i] *= static_cast<float>(0.5 - 0.5 * std::cos(M_PI * i / kStep));
    return x;
}

// All four voices on at 1/32, 300 BPM, Tone 0.7, Dry 1.
Settings four() {
    Settings s = quiet();
    s.tone = 0.7f;
    s.dry = 1;
    s.echoes = 0.8f;
    for (VoiceSettings& v : s.voice) v.on = true;
    return s;
}

double kink(const Output& o) { return std::fmax(worst_kink(o.left, 0, length), worst_kink(o.right, 0, length)); }

// A held sine through four voices; `edit` changes the settings at `change`.
// No click, and the echoes changed.
void smooth(const std::string& what, const std::function<void(Settings&)>& edit) {
    const std::vector<float> x = held();
    const Settings before = four();
    Settings after = before;
    edit(after);
    const Output o = play(x, [&](uint32_t f) { return f < change ? before : after; }, 8);
    const Output same = play(x, [&](uint32_t) { return before; }, 8);
    double moved = 0;
    // std::max, not std::fmax: Debian 12's GCC 12 on arm64 crashes vectorising this loop.
    for (uint32_t i = change; i < length; ++i) moved = std::max(moved, double(std::fabs(o.left[i] - same.left[i])));
    require(moved > 0.001, what + ": the change is heard (" + std::to_string(moved) + ")");
    const double k = kink(o);
    std::printf("  %-26s worst second difference %.2e (bound %.1e)\n", what.c_str(), k, bound);
    require(k < bound, what + ": a click, " + std::to_string(k) + " against " + std::to_string(bound));
}

// The same change of taps at once, through the per-sample voice with no
// fade, clicks: the bound can tell.
void bound_catches_a_cut() {
    const std::vector<float> x = held();
    Voice probe;
    require(probe.allocate(kBufferSeconds, rate), "allocate");
    RefVoice cut(probe.size(), 1);
    const double step = step_samples(kStepBeats[0], 300, rate);
    cut.start(make_taps(pattern(8, 3, 0), 8, step), 0.5f);
    std::vector<float> v(length);
    const float pole = tone_pole(0.7f, rate);
    for (uint32_t i = 0; i < length; ++i) {
        if (i == change) cut.set(make_taps(pattern(8, 3, 3), 8, step), 0.5f);
        v[i] = cut.process(x[i], pole);
    }
    require(worst_kink(v, 0, length) > bound, "the bound catches a change of Shift at once");
}

void crossfaded_and_gliding() {
    smooth("Steps 8 to 5", [](Settings& s) { s.voice[0].steps = 5; });
    smooth("Hits 3 to 5", [](Settings& s) { s.voice[0].hits = 5; });
    smooth("Shift 0 to 3", [](Settings& s) { s.voice[0].shift = 3; });
    smooth("Step 1/32 to 1/16", [](Settings& s) { s.step = 2; });
    smooth("BPM 300 to 170", [](Settings& s) { s.bpm = 170; });
    smooth("Level 1 to 0", [](Settings& s) { s.voice[1].level = 0; });
    smooth("Pan 0.6 to -1", [](Settings& s) { s.voice[1].pan = -1; });
    smooth("Feedback 0.5 to 0.95", [](Settings& s) { s.voice[0].feedback = 0.95f; });
    smooth("Tone 0.7 to 0", [](Settings& s) { s.tone = 0; });
    smooth("Dry 1 to 0", [](Settings& s) { s.dry = 0; });
    smooth("Echoes 0.8 to 0", [](Settings& s) { s.echoes = 0; });
    smooth("everything at once", [](Settings& s) {
        s.bpm = 123;
        s.step = 4;
        s.tone = 0.2f;
        s.dry = 0.4f;
        for (VoiceSettings& v : s.voice) {
            v.steps = 16 - v.steps;
            v.hits = v.hits + 3;
            v.shift = 2;
            v.feedback = 0.9f;
            v.level = 0.5f;
            v.pan = -v.pan;
        }
    });
    bound_catches_a_cut();
}

// Hits dragged from 1 to 16, one value a block of 64: the first change
// fades, the rest wait, and the newest fades in when that fade ends.
void dragged_knob() {
    const std::vector<float> x = held();
    const uint32_t from = 10 * kStep;
    const auto at = [&](uint32_t f) {
        Settings s = four();
        s.voice[0].steps = 16;  // so that Hits up to 16 all differ
        s.voice[0].hits = f < from ? 1 : std::min<int>(16, 2 + static_cast<int>((f - from) / 64));
        return s;
    };
    const uint64_t before = engine().voice(0).fades();
    const Output o = play(x, at, 64);
    const uint64_t fades = engine().voice(0).fades() - before;
    require(fades >= 1 && fades <= 2, "16 values dragged within a fade cost " + std::to_string(fades) + " fades");
    require(engine().voice(0).taps() == make_taps(pattern(16, 16, 0), 16, kStep), "the newest value is playing");
    require(kink(o) < bound, "dragging Hits clicks");
    // Dragged slowly, a value every 2000 samples, each value fades on its own.
    const auto slow = [&](uint32_t f) {
        Settings s = four();
        s.voice[0].steps = 16;  // so that Hits up to 16 all differ
        s.voice[0].hits = f < from ? 1 : std::min<int>(16, 2 + static_cast<int>((f - from) / 2000));
        return s;
    };
    const uint64_t start = engine().voice(0).fades();
    play(x, slow, 64);
    require(engine().voice(0).fades() - start == 15, "15 slow changes are 15 fades");
}

// A tempo change fades each voice that is on, once; the one that is off
// doesn't fade.
void tempo_fades_every_voice() {
    const std::vector<float> x = held();
    Settings before = four();
    before.voice[3].on = false;
    Settings after = before;
    after.bpm = 240;
    uint64_t start[kVoices];
    for (uint32_t n = 0; n < kVoices; ++n) start[n] = engine().voice(n).fades();
    const Output o = play(x, [&](uint32_t f) { return f < change ? before : after; }, 128);
    for (uint32_t n = 0; n < 3; ++n)
        require(engine().voice(n).fades() - start[n] == 1, "a tempo change fades voice " + std::to_string(n + 1) + " once");
    require(engine().voice(3).fades() == start[3], "a voice that is off doesn't fade");
    require(kink(o) < bound, "a tempo change clicks");
}
}

int main() {
    try {
        blocks_match_samples();
        crossfaded_and_gliding();
        dragged_knob();
        tempo_fades_every_voice();
        std::puts("PASS test_changes: block-wise voice matches per-sample; no clicks changing any control; a dragged "
                  "knob costs at most two fades; a tempo change fades every voice that is on");
    } catch (const std::exception& e) { std::printf("FAIL test_changes: %s\n", e.what()); return 1; }
}
