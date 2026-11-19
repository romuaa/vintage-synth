#pragma once

// The chip's output stage (docs/architecture.md 8.4.4, 8.15.7, 8.19.1; docs/decisions.md "B-Side S3a"):
// the voices' DC, the mixer, the 4-bit volume, the board filters and the reference's output scale.
// One per chip. JUCE-free, allocates nothing, no locks, no exceptions.
//
// SIGNAL FLOW (render units in, the reference's output units out):
//   bus level      = audio + kEnvDc x (sum of the bus's envelope converter values) + voices x kVoiceDc
//                    (filterBusLevel for the filter's input; the direct bus inside process)
//   mixer sum S    = kMixerDc + direct bus level + filter output (Filter::process of the filter bus level)
//   mixer          = Y0 + V (volume) x kScale x N (G x S)
//   board filters  = one-pole high-pass 1.196 Hz, one-pole low-pass 15.4 kHz (8.15.7), at the internal rate
// The output is in the reference's units (8.15.7: its board-filter output x 3/2 (A) or 5/2 (B) / 32768, so
// 1.0 is its 16-bit full scale); our scale constant kScale is fitted to that directly.
//
// - Voice DC (8.4.4): every voice adds kVoiceDc wherever it is routed (filter or direct; none when
//   oscillator 3 is off and not routed, 8.4.3 row 14) and kEnvDc times its envelope converter value: Model A's
//   waveforms sit above their zero (a pulse at full envelope swings +0.88 / -0.12 around it in render units),
//   Model B's are centred (kEnvDc 0). kMixerDc is the mixer's own DC.
// - The mixer N is a static curve (Model A clearly compressive and asymmetric, a quartic limited where it
//   turns; Model B a mild cubic): the second harmonic on the triangle (-34 / -40 dB, decisions S2b-2 row 5)
//   comes from here. G is the mixer's gain, which falls with the number of inputs connected to it (direct
//   voices and selected filter outputs; Model A about 8 % per input, Model B 0.4 %).
// - volumeDc off (chip.volume_dc, 8.4.4): the static DC (mixer DC, the voices' kVoiceDc on both paths, Y0)
//   is subtracted after the mixer curve at the curve's own operating point, so volume writes do not step the
//   output and the curve's distortion is unchanged; the envelope part (kEnvDc) belongs to the waveform and
//   stays. On: as the reference.
// - V is the volume register's level: Model A a fitted compressive table, Model B k / 15.

#include "bside/chip/ChipTypes.h"

#include <cmath>
#include <cstdint>

namespace vs::bside::chip
{
class OutputStage
{
public:
    OutputStage() noexcept;

    /** Model, internal rate (Hz, > 0), volume DC on or off. Keeps the state. */
    void prepare (Model model, double internalRate, bool volumeDc) noexcept;
    /** Board-filter state to zero (the reference's power-on). */
    void reset() noexcept;

    void setVolumeDc (bool on) noexcept;
    /** Register 0x17 (bits 0..2: oscillators 1..3 through the filter; the resonance nibble is ignored here). */
    void setRouting (std::uint8_t resonanceRouting) noexcept;
    /** Register 0x18 (bit 7 oscillator 3 off, bits 4..6 the mode, bits 0..3 the volume). */
    void setModeVolume (std::uint8_t modeVolume) noexcept;
    /** What the filter makes of the filter bus's static DC (Filter::staticOutput (staticFilterBusDc ())),
        needed with volumeDc off. The owner sets it when the routing, the mode or the resonance changes. */
    void setFilterStaticDc (double filterOutputDc) noexcept;

    /** Voices routed to the filter and voices on the direct path (oscillator 3 off taken into account). */
    int filterVoices() const noexcept { return nFilter; }
    int directVoices() const noexcept { return nDirect; }
    /** The filter's input: the filter bus's audio (render units) and the sum of its voices' envelope
        converter values (RenderTables envelope[], 0..1 each). */
    float filterBusLevel (float audio, float envelopeSum) const noexcept
    {
        return static_cast<float> (static_cast<double> (audio) + envDc * static_cast<double> (envelopeSum) + filterBusDc);
    }
    /** The filter bus's static DC (no audio, envelopes 0). */
    double staticFilterBusDc() const noexcept { return filterBusDc; }

    /** One internal sample: the filter's output (render units) and the direct bus (audio and envelope sum).
        Returns the output in the reference's units, after the board filters. */
    float process (float filterOut, float directAudio, float directEnvelopeSum) noexcept
    {
        const double s = static_cast<double> (filterOut) + static_cast<double> (directAudio)
                         + envDc * static_cast<double> (directEnvelopeSum) + staticDirect;
        const double mix = offset + level * (curve (gain * s) - curveDc);
        // board filters: one-pole high-pass, then the trapezoidal one-pole low-pass
        const double hp = hpA * (hpY + mix - hpX);
        hpX = mix;
        hpY = flush (hp);
        const double v = (hpY - lpS) * lpG;
        const double y = v + lpS;
        lpS = flush (y + v);
        return static_cast<float> (y);
    }

    /** The mixer alone, before the board filters (tests): the same as process without the filters. */
    double mixerOutput (double filterOut, double directAudio, double directEnvelopeSum) const noexcept
    {
        const double s = filterOut + directAudio + envDc * directEnvelopeSum + staticDirect;
        return offset + level * (curve (gain * s) - curveDc);
    }

    Model model() const noexcept { return mdl; }
    double gainFactor() const noexcept { return gain; }
    double volumeLevel() const noexcept { return level; }

    /** The fitted constants a renderer needs (render units; decisions "B-Side S3a"). */
    static double voiceDc (Model m) noexcept;
    static double envelopeDc (Model m) noexcept;

private:
    static double flush (double v) noexcept { return std::abs (v) < 1e-15 ? 0.0 : v; }
    double curve (double x) const noexcept
    {
        const double c = x < curveMin ? curveMin : (x > curveMax ? curveMax : x);
        return ((((n5 * c + n4) * c + n3) * c + n2) * c + 1.0) * c;
    }
    void update() noexcept;

    Model mdl = Model::B;
    double rate = 96000.0;
    bool volumeDcOn = false;
    std::uint8_t routing = 0, modeVolume = 0;
    double filterStaticDc = 0.0;

    int nFilter = 0, nDirect = 3;
    double envDc = 0.0, filterBusDc = 0.0, staticDirect = 0.0;
    double gain = 1.0, level = 0.0, offset = 0.0, curveDc = 0.0;
    double n2 = 0.0, n3 = 0.0, n4 = 0.0, n5 = 0.0, curveMin = -8.0, curveMax = 8.0;
    double hpA = 1.0, lpG = 0.5;
    double hpX = 0.0, hpY = 0.0, lpS = 0.0;
};
} // namespace vs::bside::chip
