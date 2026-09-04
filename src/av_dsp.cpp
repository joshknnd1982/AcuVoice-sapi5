#include "av_dsp.h"
#include "av_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AcuVoice {
namespace dsp {

namespace {

// G.711 mu-law expansion. Built once; the table costs 512 bytes and saves a branchy
// decode on every one of the eight thousand samples a second the engine produces.
struct ulaw_table {
    int16_t value[256];
    ulaw_table() noexcept
    {
        static const int segment[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
        for (int i = 0; i < 256; ++i) {
            const int u = ~i & 0xFF;
            const int exponent = (u >> 4) & 0x07;
            const int mantissa = u & 0x0F;
            const int magnitude = segment[exponent] + (mantissa << (exponent + 3));
            value[i] = static_cast<int16_t>((u & 0x80) ? -magnitude : magnitude);
        }
    }
};
const ulaw_table g_ulaw;

// WSOLA, sized for 8 kHz speech. A 30 ms frame is long enough to hold a pitch period of
// the lowest male fundamental this engine produces and short enough that a plosive does
// not smear across the overlap.
constexpr int FRAME = 240;              // 30 ms at 8 kHz
constexpr int OVERLAP = FRAME / 2;      // 15 ms Hann cross-fade
constexpr int SEARCH = 96;              // +/- 12 ms of similarity search

[[nodiscard]] inline int16_t clamp16(double v) noexcept
{
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return static_cast<int16_t>(std::lround(v));
}

// Cross-correlation between the tail the output already has and a candidate frame, used
// to pick the frame whose start lines up with the phase the output is in.
[[nodiscard]] double similarity(const int16_t* a, const int16_t* b, int n) noexcept
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return sum;
}

}  // namespace

int16_t ulaw_to_pcm16(unsigned char u) noexcept
{
    return g_ulaw.value[u];
}

void ulaw_to_pcm16(const unsigned char* ulaw, size_t count, std::vector<int16_t>& pcm)
{
    pcm.resize(count);
    for (size_t i = 0; i < count; ++i) {
        pcm[i] = g_ulaw.value[ulaw[i]];
    }
}

void time_scale(const std::vector<int16_t>& in, double factor, std::vector<int16_t>& out)
{
    const int n = static_cast<int>(in.size());
    if (factor <= 0.0 || n == 0) {
        out.clear();
        return;
    }
    if (std::fabs(factor - 1.0) < 1e-6) {
        out = in;
        return;
    }
    // Too short to cross-fade: resampling it would change the pitch, so it is passed
    // through. At 30 ms this is a fragment of a single phoneme.
    if (n < FRAME + SEARCH * 2) {
        out = in;
        return;
    }

    // Hann cross-fade, built once per call. The two halves always sum to one, so a
    // stationary vowel comes through the overlap at its original amplitude.
    static std::vector<float> fade_in, fade_out;
    if (fade_in.empty()) {
        fade_in.resize(OVERLAP);
        fade_out.resize(OVERLAP);
        for (int i = 0; i < OVERLAP; ++i) {
            const double w = 0.5 - 0.5 * std::cos(3.14159265358979 * i / (OVERLAP - 1));
            fade_in[i] = static_cast<float>(w);
            fade_out[i] = static_cast<float>(1.0 - w);
        }
    }

    const int synthesis_hop = FRAME - OVERLAP;
    const double analysis_hop = synthesis_hop / factor;

    out.clear();
    out.reserve(static_cast<size_t>(n * factor) + FRAME);

    // The first frame is copied whole; there is nothing yet to cross-fade against.
    out.insert(out.end(), in.begin(), in.begin() + FRAME);

    double next = analysis_hop;
    while (true) {
        const int ideal = static_cast<int>(std::lround(next));
        if (ideal + FRAME >= n) {
            break;
        }

        // Look around the ideal read position for the frame that continues the phase the
        // output already ends on. This is what keeps a stretched vowel from warbling.
        const int low = std::max(0, ideal - SEARCH);
        const int high = std::min(n - FRAME - 1, ideal + SEARCH);
        const int16_t* tail = out.data() + out.size() - OVERLAP;

        int best = ideal;
        double best_score = -1e300;
        for (int candidate = low; candidate <= high; ++candidate) {
            const double score = similarity(tail, in.data() + candidate, OVERLAP);
            if (score > best_score) {
                best_score = score;
                best = candidate;
            }
        }

        // Cross-fade the chosen frame's head over the output's tail, then append the
        // rest of it.
        const int16_t* frame = in.data() + best;
        const size_t base = out.size() - OVERLAP;
        for (int i = 0; i < OVERLAP; ++i) {
            out[base + i] = clamp16(out[base + i] * static_cast<double>(fade_out[i]) +
                                    frame[i] * static_cast<double>(fade_in[i]));
        }
        out.insert(out.end(), frame + OVERLAP, frame + FRAME);

        next += analysis_hop;
    }
}

void resample(const std::vector<int16_t>& in, double step, std::vector<int16_t>& out)
{
    const int n = static_cast<int>(in.size());
    if (n == 0 || step <= 0.0) {
        out.clear();
        return;
    }
    const size_t count = static_cast<size_t>(n / step);
    out.resize(count);

    // Catmull-Rom. Cheap, and its passband is flat enough that speech resampled by less
    // than an octave keeps its formants where they belong.
    const auto at = [&](int i) -> double {
        return in[static_cast<size_t>(std::clamp(i, 0, n - 1))];
    };
    double pos = 0.0;
    for (size_t i = 0; i < count; ++i, pos += step) {
        const int k = static_cast<int>(pos);
        const double t = pos - k;
        const double p0 = at(k - 1), p1 = at(k), p2 = at(k + 1), p3 = at(k + 2);
        const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
        const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
        const double c = -0.5 * p0 + 0.5 * p2;
        out[i] = clamp16(((a * t + b) * t + c) * t + p1);
    }
}

void apply_gain(std::vector<int16_t>& pcm, double gain)
{
    if (std::fabs(gain - 1.0) < 1e-6) {
        return;
    }
    if (gain <= 0.0) {
        std::fill(pcm.begin(), pcm.end(), static_cast<int16_t>(0));
        return;
    }
    // Below unity this is a plain scale. Above it, a tanh knee that only bends the loud
    // samples: hard clipping an amplified vowel turns it into a buzz, which is far worse
    // than the couple of dB of headroom the knee costs.
    constexpr double KNEE = 24000.0;
    for (auto& s : pcm) {
        double v = s * gain;
        if (v > KNEE || v < -KNEE) {
            const double sign = (v < 0) ? -1.0 : 1.0;
            const double over = (std::fabs(v) - KNEE) / (32767.0 - KNEE);
            v = sign * (KNEE + (32767.0 - KNEE) * std::tanh(over));
        }
        s = clamp16(v);
    }
}

void render(const std::vector<int16_t>& in, const params& p, std::vector<int16_t>& out)
{
    constexpr double UPSAMPLE =
        static_cast<double>(ENGINE_SAMPLE_RATE) / OUTPUT_SAMPLE_RATE;   // 0.5

    if (in.empty()) {
        out.clear();
        return;
    }

    // Resampling changes duration as well as pitch, so the time scale has to absorb
    // that: stretching by duration*pitch first leaves exactly `duration` behind once the
    // resampler has divided it by `pitch` again.
    const double stretch = p.duration * p.pitch;

    std::vector<int16_t> stretched;
    time_scale(in, stretch, stretched);

    // One interpolation pass does both the 8 kHz -> 16 kHz rate change and the pitch
    // move. Doing them separately would resample twice for no gain in quality.
    resample(stretched, p.pitch * UPSAMPLE, out);

    apply_gain(out, p.gain);
}

}  // namespace dsp
}  // namespace AcuVoice
