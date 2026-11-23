#pragma once

// The chip's output stage (docs/architecture.md 8.4.4, 8.15.7, 8.19.1; docs/decisions.md "B-Side S3a"): the
// voices' shaping and DC, the mixer, the 4-bit volume, the board filters and the reference's output scale. One
// per chip. JUCE-free, allocates nothing, no locks, no exceptions.
//
// SIGNAL FLOW (render units in, the reference's output units out):
//   voice          v = shapeVoice (u, e): u + al u^2 / e + be u^3 / e^2 (= e f (u / e)), per voice, by the renderer
//                  (Model A: the waveform is bent before the envelope multiplies it; the bend's relative strength
//                  does not depend on the envelope level, measured). Model B: v = u.
//   bus level      = sum of v + kEnvDc x (sum of the bus's envelope converter values) + voices x kVoiceDc
//                    (filterBusLevel for the filter's input; the direct bus inside process)
//   mixer sum S    = kMixerDc + direct bus level + filter output (Filter::process of the filter bus level)
//   mixer          = Y0 + V (volume) x kScale x N (G x S)
//   board filters  = one-pole high-pass 1.196 Hz, one-pole low-pass 15.4 kHz (8.15.7), at the internal rate
// The output is in the reference's units (8.15.7: its board-filter output x 3/2 (A) or 5/2 (B) / 32768).
//
// - Voice DC (8.4.4): every voice adds kVoiceDc wherever it is routed (none when oscillator 3 is off and not
//   routed) and kEnvDc times its envelope converter value; kMixerDc is the mixer's own DC.
// - N is a static curve (a quintic limited where it turns), G the mixer's gain, which falls with the number of
//   inputs connected (direct voices, selected filter outputs).
// - volumeDc off (chip.volume_dc, 8.4.4): no DC reaches the volume. The static DC (mixer DC, kVoiceDc on both
//   paths, Y0) is subtracted at the curve's operating point (no step at a routing write), and the volume moves
//   behind the board high-pass, which removes what remains (the envelopes' DC, what the curves make of the
//   audio): a volume write with held voices then changes the output's DC by less than -60 dBFS once the
//   high-pass has settled (tested). On: as the reference, the volume before the board filters.
// - V is the volume register's level: Model A a fitted compressive table, Model B k / 15.
// - INPUT GUARD: a non-finite or absurd mixer value is replaced by 0 before the board filters, off the per-sample
//   dependency chain of their states.
//
// SHARED (8.5, filter.topology Shared; 8.19.3): one Filter per part; its input is the static DC of one chip's filter
// bus (staticFilterBusDc of the most recent note) plus, over every chip, filterBusAc (the audio and the envelopes' DC
// of that chip's filter bus). Each of the n chips feeding it gets sharedFilterShare: the filter's static output of
// that one chip's static DC in full, plus 1/n of the rest; setFilterStaticDc gets the static output in full. Then
// (tested, "the Shared topology ..." in FilterOutputTests.cpp):
// - n = 1 is Per Note exactly;
// - Model B (its filter linear but for the band/high second-order term, Filter.h, which in Shared acts on the summed
//   band/high signal): n chips with identical held routed voices (a 110 Hz triangle, cutoff 900, all three modes,
//   full and half envelope) give the output DC of n Per Note chips (a 0 -> 15 volume step with volume DC on: within
//   0.05 dB for n = 1 .. 16) and their audio (rms within 1 %);
// - Model A distorts the summed signal (8.5): the envelopes' DC and the audio of all n chips meet its static output
//   functions together (they saturate), so with identical held routed voices (a 110 Hz triangle, cutoff 900, all three
//   modes) at full envelope and n = 2 / 4 / 8 / 16 the volume step's DC differs from n Per Note chips' by -0.1 / +0.6 /
//   +1.9 / +2.7 dB and the audio's rms is 1.01 / 0.72 / 0.37 / 0.20 of theirs; at half envelope the DC step differs by
//   at most 0.8 dB. A documented property, pinned by that test (+-0.3 dB, +-0.05); it is not a claim that Model A
//   Shared matches Per Note.

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

    /** One voice's contribution to its bus (render units): the renderer's voice audio u (its waveform after the
        converter, minus the converter's centre, times its envelope value e) bent as the model's voice bends it. */
    static float shapeVoice (Model m, float u, float e) noexcept
    {
        if (m == Model::B || ! (e > 1e-4f))
            return u;
        const double w = static_cast<double> (u) / static_cast<double> (e);
        return static_cast<float> (static_cast<double> (e) * (w + w * w * (kShapeA2 + w * kShapeA3)));
    }

    /** Voices routed to the filter and voices on the direct path (oscillator 3 off taken into account). */
    int filterVoices() const noexcept { return nFilter; }
    int directVoices() const noexcept { return nDirect; }
    /** The filter's input: the filter bus's (shaped) audio and the sum of its voices' envelope converter values
        (RenderTables envelope[], 0..1 each). */
    float filterBusLevel (float audio, float envelopeSum) const noexcept
    {
        return static_cast<float> (static_cast<double> (audio) + envDc * static_cast<double> (envelopeSum) + filterBusDc);
    }
    /** Shared topology: the part of filterBusLevel that is summed over chips (no voice DC; see "SHARED"). */
    float filterBusAc (float audio, float envelopeSum) const noexcept
    {
        return static_cast<float> (static_cast<double> (audio) + envDc * static_cast<double> (envelopeSum));
    }
    /** The filter bus's static DC (no audio, envelopes 0). */
    double staticFilterBusDc() const noexcept { return filterBusDc; }
    /** Shared topology (see "SHARED"): what one of `chips` chips' stages gets of the shared filter's output, given the
        filter's static output for one chip's static DC (Filter::staticOutput (staticFilterBusDc ())). */
    static float sharedFilterShare (float sharedOut, double filterStaticOut, int chips) noexcept
    {
        const double n = chips > 1 ? static_cast<double> (chips) : 1.0;
        return static_cast<float> ((static_cast<double> (sharedOut) - filterStaticOut) / n + filterStaticOut);
    }

    /** One internal sample: the filter's output (render units) and the direct bus (shaped audio and envelope sum).
        Returns the output in the reference's units, after the board filters. */
    float process (float filterOut, float directAudio, float directEnvelopeSum) noexcept
    {
        const double s = static_cast<double> (filterOut) + static_cast<double> (directAudio)
                         + envDc * static_cast<double> (directEnvelopeSum) + staticDirect;
        // volume DC on: the volume before the board filters, as the chip; off: the volume after the board high-pass,
        // which has removed every DC by then (see "volumeDc off" above)
        const double mix = volumeDcOn ? offset + level * curve (gain * s) : curve (gain * s) - curveDc;
        // board filters: one-pole high-pass, then the trapezoidal one-pole low-pass (state-space form)
        const double in = std::abs (mix) < 1e9 ? mix : 0.0; // a NaN or absurd input does not enter the filters
        hpY = flush (hpA * hpY + hpA * (in - hpX));
        hpX = in;
        const double lpIn0 = volumeDcOn ? hpY : level * hpY;
        const double y = lpG * lpIn0 + lpKeep * lpS;
        lpS = flush (lpIn * lpIn0 + lpNext * lpS);
        return static_cast<float> (y);
    }

    /** The mixer alone, before the board filters (tests; with volume DC off the volume is not applied). */
    double mixerOutput (double filterOut, double directAudio, double directEnvelopeSum) const noexcept
    {
        const double s = filterOut + directAudio + envDc * directEnvelopeSum + staticDirect;
        return volumeDcOn ? offset + level * curve (gain * s) : curve (gain * s) - curveDc;
    }

    Model model() const noexcept { return mdl; }
    double gainFactor() const noexcept { return gain; }
    double volumeLevel() const noexcept { return level; }

    /** The fitted constants a renderer needs (render units; decisions "B-Side S3a"). */
    static double voiceDc (Model m) noexcept;
    static double envelopeDc (Model m) noexcept;
    /** Model A's voice bend f (w) = w + kShapeA2 w^2 + kShapeA3 w^3 (fitted; decisions "B-Side S3a"). */
    static constexpr double kShapeA2 = -0.175104, kShapeA3 = -0.097759;

private:
    /** A filter state: 0 below 1e-15 (no subnormal numbers). */
    static double flush (double v) noexcept { return std::abs (v) < 1e-15 ? 0.0 : v; }
    double curve (double x) const noexcept
    {
        const double c = x < curveMin ? curveMin : (x > curveMax ? curveMax : x); // NaN passes; the guard stops it
        if (! quintic)
            return ((n3 * c + n2) * c + 1.0) * c;
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
    bool quintic = false;
    double n2 = 0.0, n3 = 0.0, n4 = 0.0, n5 = 0.0, curveMin = -8.0, curveMax = 8.0;
    double hpA = 1.0, lpG = 0.5, lpKeep = 0.5, lpIn = 1.0, lpNext = 0.0;
    double hpX = 0.0, hpY = 0.0, lpS = 0.0;
};
} // namespace vs::bside::chip
