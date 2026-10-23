// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "voice.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace euclidelay {
namespace {
int samples(double seconds, double rate) {
    const int n = static_cast<int>(std::lround(seconds * rate));
    return n < 1 ? 1 : n;
}

long longest(const Taps& t) { return t.count ? t.delay[t.count - 1] : 0; }
} // namespace

float tone_pole(float tone, double rate) {
    constexpr double darkest = 1500.0;  // Hz at Tone 0 (tune)
    const double t = tone < 0 ? 0 : tone > 1 ? 1 : tone;
    const double pole = std::exp(-2.0 * M_PI * darkest / rate);
    // The square root keeps most of the knob's travel in the darker half
    // (tune): Tone 0.7 is about 6 kHz at 48 kHz.
    return static_cast<float>(pole * std::sqrt(1.0 - t));
}

Voice::~Voice() { std::free(buffer_); }

bool Voice::allocate(double seconds, double rate) {
    std::free(buffer_);
    size_ = static_cast<std::size_t>(std::ceil(seconds * rate)) + 1;
    buffer_ = static_cast<float*>(std::calloc(size_, sizeof(float)));
    if (!buffer_) size_ = 0;
    fade_ = samples(kSwitchSeconds, rate);
    inv_fade_ = 1.0f / static_cast<float>(fade_);
    fade_len_ = samples(kFadeSeconds, rate);
    inv_fade_len_ = 1.0f / static_cast<float>(fade_len_);
    glide_ = samples(kGlideSeconds, rate);
    fades_ = 0;
    clear();
    return buffer_;
}

void Voice::clear() {
    if (buffer_) std::memset(buffer_, 0, size_ * sizeof(float));
    write_ = 0;
    tone_ = old_tone_ = 0;
    on_ = true;
    fresh_ = static_cast<long>(size_);  // all of it, zeroed, is this life's
    old_age_ = 0;
    in_ramp_ = out_ramp_ = fade_;
    old_ramp_ = 0;
    fade_left_ = 0;
    pending_set_ = false;
}

void Voice::switch_to(bool on) {
    if (on == on_) return;
    on_ = on;
    if (!on) return;  // its output fades out from here
    // Coming on: whatever is still fading out becomes the old life.
    old_ramp_ = out_ramp_;
    old_age_ = out_ramp_ > 0 ? fresh_ : 0;
    out_ramp_ = fade_;
    old_tone_ = tone_;
    fresh_ = 0;
    in_ramp_ = 0;
    tone_ = 0;
}

void Voice::start(const Taps& taps, float feedback) {
    cur_ = next_ = taps;
    pending_set_ = false;
    fade_left_ = 0;
    feedback_.reset(feedback);
}

void Voice::set(const Taps& taps, float feedback) {
    feedback_.set(feedback, glide_);
    if (taps == this->taps()) return;
    if (fade_left_ == 0) {
        next_ = taps;  // cur_ is next_ until now
        fade_left_ = fade_len_;
        ++fades_;
    } else if (taps == next_) {
        pending_set_ = false;  // back to what is fading in
    } else {
        pending_ = taps;
        pending_set_ = true;
    }
}

void Voice::run(const float* x, const float* pole, float* v, int n) {
    int done = 0;
    while (done < n) {
        const bool fading = fade_left_ > 0;
        const int m = fading && n - done > fade_left_ ? fade_left_ : n - done;
        if (!part(x + done, pole + done, v + done, m)) {
            for (int i = done + m; i < n; ++i) v[i] = 0.0f;
            return;
        }
        done += m;
        if (fading && fade_left_ == 0) {
            // The fade is over: the newest taps, if they are newer still,
            // fade in next.
            cur_ = next_;
            if (pending_set_) {
                pending_set_ = false;
                if (pending_ != cur_) {
                    next_ = pending_;
                    fade_left_ = fade_len_;
                    ++fades_;
                }
            }
        }
    }
}

void Voice::add_run(float* sum, long w0, long d, int n) const {
    const long size = static_cast<long>(size_);
    long s = w0 - d;
    if (s < 0) s += size;
    const int first = size - s < n ? static_cast<int>(size - s) : n;
    const float* from = buffer_ + s;
    for (int i = 0; i < first; ++i) sum[i] += from[i];
    for (int i = first; i < n; ++i) sum[i] += buffer_[i - first];
}

bool Voice::part(const float* x, const float* pole, float* v, int n) {
    const bool fading = fade_left_ > 0;
    const bool has_old = old_ramp_ > 0;
    const long size = static_cast<long>(size_);
    const long w0 = write_;
    bool reset = false;
    int m = n;

    // y, one sample at a time, and everything per sample the taps need.
    for (int i = 0; i < n; ++i) {
        const long age = fresh_;  // samples written in this life, before this one
        const float xi = x[i] * gain(in_ramp_);
        const float a = fading ? static_cast<float>(fade_len_ - fade_left_) * inv_fade_len_ : 0.0f;
        const float fb = feedback_.next();
        const float p = pole[i];
        const float back_cur = cur_.loop <= age ? buffer_[wrap(write_ - cur_.loop)] : 0.0f;
        float back = back_cur;
        if (fading) {
            const float back_next = next_.loop <= age ? buffer_[wrap(write_ - next_.loop)] : 0.0f;
            back = back_cur + a * (back_next - back_cur);
        }
        // One-pole low-pass: exactly `back` when the pole is 0. Kept out of
        // the denormals as the loop dies away.
        tone_ = back + p * (tone_ - back);
        if (std::fabs(tone_) < 1e-20f) tone_ = 0.0f;
        float looped = fb * tone_;
        float y = xi + looped;
        if (!std::isfinite(y)) {
            // The non-finite guard (docs/design.md, "Host limits"): empty the
            // voice, silent until new input arrives.
            y = looped = 0.0f;
            empty();
            reset = true;
        }
        buffer_[write_] = y;
        age_[i] = fresh_;
        // Step 0 reads y[t] itself, x[t] + looped; without the note, that
        // is `looped`.
        now_cur_[i] = cur_.zero ? looped : 0.0f;
        now_next_[i] = next_.zero ? looped : 0.0f;
        if (has_old) {
            // The last life's step 0 is its own loop read, filtered on.
            const long lo = fresh_, hi = fresh_ + old_age_;
            const float old_cur = cur_.loop > lo && cur_.loop <= hi ? buffer_[wrap(write_ - cur_.loop)] : 0.0f;
            float old_back = old_cur;
            if (fading) {
                const float old_next = next_.loop > lo && next_.loop <= hi ? buffer_[wrap(write_ - next_.loop)] : 0.0f;
                old_back = old_cur + a * (old_next - old_cur);
            }
            old_tone_ = old_back + p * (old_tone_ - old_back);
            const float zero = fb * old_tone_;
            old_cur_[i] = cur_.zero ? zero : 0.0f;
            old_next_[i] = next_.zero ? zero : 0.0f;
            old_gain_[i] = gain(old_ramp_);
        }
        out_gain_[i] = gain(out_ramp_);
        a_[i] = a;
        if (++write_ == size) write_ = 0;
        if (fresh_ < size) ++fresh_;
        if (in_ramp_ < fade_) ++in_ramp_;
        if (!on_ && out_ramp_ > 0) --out_ramp_;
        if (old_ramp_ > 0) --old_ramp_;
        if (fading) --fade_left_;
        if (!active()) {
            m = i + 1;
            break;
        }
    }

    // The taps, each over the whole part.
    const Taps& cur = cur_;
    const Taps& next = next_;
    const long reach = fading && longest(next) > longest(cur) ? longest(next) : longest(cur);
    if (!has_old && !reset && age_[0] >= reach) {
        // Every read is of this life: runs in a row.
        for (int k = 0; k < cur.count; ++k) add_run(now_cur_, w0, cur.delay[k], m);
        if (fading)
            for (int k = 0; k < next.count; ++k) add_run(now_next_, w0, next.delay[k], m);
    } else {
        // A voice that has just come on, or is fading out an old life: each
        // read is of this life, the last, or silence.
        for (int i = 0; i < m; ++i) {
            const long lo = age_[i], hi = has_old ? lo + old_age_ : lo;
            long w = w0 + i;
            if (w >= size) w -= size;
            for (int k = 0; k < cur.count; ++k) {
                const long d = cur.delay[k];
                if (d <= lo) now_cur_[i] += buffer_[wrap(w - d)];
                else if (d <= hi) old_cur_[i] += buffer_[wrap(w - d)];
            }
            if (fading)
                for (int k = 0; k < next.count; ++k) {
                    const long d = next.delay[k];
                    if (d <= lo) now_next_[i] += buffer_[wrap(w - d)];
                    else if (d <= hi) old_next_[i] += buffer_[wrap(w - d)];
                }
        }
    }

    // Old and new taps crossfaded, this life and the last.
    for (int i = 0; i < m; ++i) {
        float now = now_cur_[i];
        if (fading) now = now + a_[i] * (now_next_[i] - now);
        float out = out_gain_[i] * now;
        if (has_old) {
            float before = old_cur_[i];
            if (fading) before = before + a_[i] * (old_next_[i] - before);
            out += old_gain_[i] * before;
        }
        v[i] = out;
    }
    // The non-finite guard on the output: silent from there, and the voice
    // empties itself.
    for (int i = 0; i < m; ++i)
        if (!std::isfinite(v[i])) {
            for (int j = i; j < m; ++j) v[j] = 0.0f;
            empty();
            break;
        }
    for (int i = m; i < n; ++i) v[i] = 0.0f;
    return m == n;
}

} // namespace euclidelay
