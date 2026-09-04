#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Rate, pitch and volume for AcuVoice.
//
// avcore parses the engine's own \spd= \pit= \vol= tags but never acts on them -- every
// value renders byte-identical audio and the numbers come back out of the dequeue
// functions instead. The 1999 SAPI4 engine did the prosody itself, in AcuEng.dll, using
// Aspeed.dll and its own pitch converter. This does the same job, in the same place, with
// an algorithm that does not access-violate: WSOLA for the time scale, cubic resampling
// for the pitch, and a scale with a soft knee for the volume.

namespace AcuVoice {
namespace dsp {

// 8-bit G.711 mu-law to 16-bit linear. The engine's whole output is in this format.
void ulaw_to_pcm16(const unsigned char* ulaw, size_t count, std::vector<int16_t>& pcm);

[[nodiscard]] int16_t ulaw_to_pcm16(unsigned char u) noexcept;

// The prosody one utterance needs, already reduced to plain factors.
struct params {
    // Output duration as a multiple of the engine's own. 2.0 is half speed, 0.5 double.
    double duration = 1.0;
    // Output pitch as a multiple of the engine's own. 1.0 leaves it alone.
    double pitch = 1.0;
    // Linear amplitude. 1.0 leaves it alone; above 1.0 a soft knee keeps it from
    // clipping into buzz.
    double gain = 1.0;

    [[nodiscard]] bool is_identity() const noexcept
    {
        return duration == 1.0 && pitch == 1.0 && gain == 1.0;
    }
};

// Turns one utterance of engine audio into what SAPI is given: OUTPUT_SAMPLE_RATE,
// 16-bit, mono, with `p` applied. `in` is at ENGINE_SAMPLE_RATE.
//
// Resampling and pitch are the same operation, so they are done in one interpolation
// pass: the sample rate doubles and the pitch moves by p.pitch at the same time. WSOLA
// runs first, at the engine's rate, stretching by duration*pitch so that the resampling
// leaves the duration where it was asked to be.
void render(const std::vector<int16_t>& in, const params& p, std::vector<int16_t>& out);

// WSOLA time scaling: `out` is `factor` times as long as `in`, at the same pitch.
// factor > 1 slows down, < 1 speeds up. Exactly 1.0 copies.
void time_scale(const std::vector<int16_t>& in, double factor, std::vector<int16_t>& out);

// Cubic resample. `out[i]` is `in` read at `i * step`, so step > 1 raises the pitch and
// shortens the result.
void resample(const std::vector<int16_t>& in, double step, std::vector<int16_t>& out);

void apply_gain(std::vector<int16_t>& pcm, double gain);

}  // namespace dsp
}  // namespace AcuVoice
