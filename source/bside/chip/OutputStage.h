#pragma once

// The chip's output stage (docs/architecture.md 8.4.4, 8.15.7, 8.19.1; docs/decisions.md "B-Side S3a"): the
// voices' shaping and DC, the mixer, the 4-bit volume, the board filters and the reference's output scale. One
// per chip. JUCE-free, allocates nothing, no locks, no exceptions.
//
// SIGNAL FLOW (render units in, the reference's output units out):
//   voice          v = e f (w), Model A's voice bend (render::bend), folded into the renderer since S3b-2
//                  (Chip::renderBendVoices; the waveform is bent before the envelope multiplies it). Model B: v = e w.
//   bus level      = sum of v + kEnvDc x (sum of the bus's envelope converter values) + voices x kVoiceDc
//                    (filterBusLevel for the filter's input; the direct bus inside process)
//   mixer sum S    = kMixerDc + direct bus level + filter output (Filter::process of the filter bus level)
//   mixer          = Y0 + V (volume) x kScale x N (G x S)
//   board filters  = one-pole high-pass 1.196 Hz, one-pole low-pass 15.4 kHz (8.15.7), at the internal rate
// process () runs this at the internal rate as S3a built it (tests, the bench's S3a cases). The chain (ChipVoice, S3b-2)
// takes the pieces instead (directSumWith, gainFactor, mixerCurve, postScale / postOffset / postGain, board) and puts
// its anti-alias filters between them (HalfBand.h; ChipVoice.h "ALIAS").
// The output is in the reference's units (8.15.7: its board-filter output x 3/2 (A) or 5/2 (B) / 32768).
//
// - Voice DC (8.4.4): every voice adds kVoiceDc wherever it is routed (none when oscillator 3 is off and not
//   routed) and kEnvDc times its envelope converter value; kMixerDc is the mixer's own DC.
// - N is a static curve, G the mixer's gain, which falls with the number of
//   inputs connected (direct voices, selected filter outputs).
// - volumeDc off (chip.volume_dc, 8.4.4): no DC reaches the volume. The static DC (mixer DC, kVoiceDc on both
//   paths, Y0) is subtracted at the curve's operating point (no step at a routing write), and the volume moves
//   behind the board high-pass, which removes what remains (the envelopes' DC, what the curves make of the
//   audio): a volume write with held voices then changes the output's DC by less than -60 dBFS once the
//   high-pass has settled (tested). On: as the reference, the volume before the board filters.
// - N: Model B a cubic (limited to +-8, never reached); Model A a quintic fitted on [-3.06, 3.55] (the gated traces'
//   range), continued smoothly beyond its joins at -3.08 and 3.80 (S3b-2 review, 8.19.4): past a join N approaches a
//   bound exponentially, N = L + a exp (b (x - j)) below, U - a exp (-b (x - j)) above, a and b chosen so that value,
//   slope and curvature are continuous at the join (a = s^2 / |k|, b = |k| / s, s and k the quintic's slope and
//   curvature there): monotonic, bounded, twice continuously differentiable. (The first build clamped N flat at the
//   quintic's turning points, -3.297 and 4.081: a jump in curvature that folded at twice the rate.) Between the joins N
//   is the quintic unchanged, to the bit.
// - V is the volume register's level: Model A a fitted compressive table, Model B k / 15.
// - INPUT GUARD: a non-finite or absurd mixer value is replaced by 0 before the board filters, off the per-sample
//   dependency chain of their states.
//
// SHARED (8.5, filter.topology Shared; 8.19.3): one Filter per part; its input is the static DC of one chip's filter
// bus (staticFilterBusDc of the most recent note) plus, over every chip, filterBusAc (the audio and the envelopes' DC
// of that chip's filter bus). Each of the n chips feeding it gets sharedFilterShare: the filter's static output of
// that one chip's static DC in full, plus 1/n of the rest; setFilterStaticDc gets the static output in full. Then
// (tested, "the Shared topology ..." in FilterOutputTests.cpp):
// - n = 1 is Per Note exactly (through ChipVoice bit for bit, the static output taken per sample: ChipVoice.h);
// - the claim below is for IDENTICAL voices only: with different notes per chip, each chip's mixer curve sees its share
//   of the summed filter output instead of its own filter's output, and Model B's Shared differs from Per Note by
//   -38.8 dB (n = 2) to -31.8 dB (n = 16) rms re Per Note's audio (all voices to the filter; mixed routings -32 to
//   -30 dB; measured 2026-11-23, figures and the pinning test in ChipVoice.h "TOPOLOGY");
// - Model B (its filter is linear): n chips with identical held routed voices give the output of n Per Note chips,
//   n = 1 .. 16: with volume DC on the DC step of a 0 -> 15 volume write within 0.05 dB, with it off the output's
//   mean within 0.05 dB (or 1e-6), and the audio's rms within 1 %; tested with a 110 Hz triangle (cutoff 900, all
//   three modes, full and half envelope) and, where the band- and high-pass carry the signal, a 220 Hz sawtooth
//   through the high-pass at cutoff 64 and through the band-pass at cutoff 300 (resonance 8, volume DC on and off);
// - Model A distorts the summed signal (8.5): the envelopes' DC and the audio of all n chips meet its static output
//   functions together (they saturate), so with identical held routed voices (a 110 Hz triangle, cutoff 900, all three
//   modes) at full envelope and n = 2 / 4 / 8 / 16 the volume step's DC differs from n Per Note chips' by -0.1 / +0.6 /
//   +1.9 / +2.7 dB and the audio's rms is 1.01 / 0.72 / 0.37 / 0.20 of theirs; at half envelope the DC step differs by
//   at most 0.8 dB. A documented property, pinned by that test (+-0.3 dB, +-0.05); it is not a claim that Model A
//   Shared matches Per Note.

#include "bside/chip/ChipTypes.h"

#include <array>
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
    /** Board-filter state settled on the constant mixer value `mix` (board (mix, post) then returns 0, the high-pass's
        steady output): a chain that starts from idle starts settled on its static level (docs/architecture.md 8.21.1). */
    void primeBoard (double mix) noexcept
    {
        hpX = std::abs (mix) < 1e9 ? mix : 0.0;
        hpY = 0.0;
        lpS = 0.0;
    }

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

    // ---- the chain in double precision (ChipVoice, docs/architecture.md 8.19.4; process () is these three in one) ----

    /** The voices on each bus as the renderer counts them (Chip::BusBlock: fractional only across a routing write).
        From the first call on they replace the counts setRouting and setModeVolume derive from the register bits. */
    void setVoices (double filterVoices, double directVoices) noexcept
    {
        if (! externalCounts || filterVoices < cntF || filterVoices > cntF || directVoices < cntD || directVoices > cntD)
        {
            externalCounts = true;
            cntF = filterVoices;
            cntD = directVoices;
            update();
        }
    }
    /** The filter's input (filterBusLevel in double): the bus's audio, its envelope sum and its voices' DC. */
    double filterInput (double audio, double envelopeSum) const noexcept { return audio + envDc * envelopeSum + filterBusDc; }
    /** Shared topology: the part of filterInput summed over chips (filterBusAc in double). */
    double filterInputAc (double audio, double envelopeSum) const noexcept { return audio + envDc * envelopeSum; }
    /** sharedFilterShare in double; one chip gets the filter's output itself (bit for bit Per Note). */
    static double sharedShare (double sharedOut, double filterStaticOut, int chips) noexcept
    {
        if (chips <= 1)
            return sharedOut;
        return (sharedOut - filterStaticOut) / static_cast<double> (chips) + filterStaticOut;
    }
    /** What multiplies the board high-pass's output: 1 with volume DC on (the volume is in the mixer), the volume
        level with it off (the volume behind the high-pass). */
    double postGain() const noexcept { return volumeDcOn ? 1.0 : level; }

    // ---- the mixer in pieces (the chain, S3b-2): mixerOutput (fo, a, e) = postScale () mixerCurve (gainFactor () s)
    //      + postOffset (), s = fo + directSumWith (a, e, directVoices) ----

    /** The direct bus's part of the mixer sum for a voice count (the stage's own count not used): its audio, its
        envelopes' DC, the mixer's DC and the voices' DC. */
    double directSumWith (double audio, double envelopeSum, double directVoiceCount) const noexcept
    {
        return audio + envDc * envelopeSum + (mixerDcConst + voiceDcConst * directVoiceCount);
    }
    /** The static curve N (limited) of the mixer, on gain x sum. */
    double mixerCurve (double x) const noexcept { return curve (x); }
    /** mixerCurve and board, inlined into their caller whatever their size (ChipVoice's block path; ChipTypes.h). */
    BS_CHIP_INLINE double mixerCurveInline (double x) const noexcept { return curveInline (x); }
    BS_CHIP_INLINE double boardInline (double mix, double post) noexcept
    {
        const double in = std::abs (mix) < 1e9 ? mix : 0.0; // a NaN or absurd input does not enter the filters
        hpY = flush (hpA * hpY + hpA * (in - hpX));
        hpX = in;
        const double lpIn0 = post * hpY;
        const double y = lpG * lpIn0 + lpKeep * lpS;
        lpS = flush (lpIn * lpIn0 + lpNext * lpS);
        return y;
    }
    /** Where N leaves its polynomial (Model A: the joins of its smooth continuation; Model B: its limits): {lower, upper}. */
    std::array<double, 2> mixerLimits() const noexcept { return { curveMin, curveMax }; }
    /** Test aid: N's polynomial coefficients {n2, n3, n4, n5} (N = x + n2 x^2 + ... + n5 x^5 between the limits). */
    std::array<double, 4> mixerCoefficients() const noexcept { return { n2, n3, n4, n5 }; }
    /** What multiplies the curve's output (the volume level with volume DC on, else 1) and what is added to it (Y0 with
        volume DC on, minus the curve at the static operating point with it off). */
    double postScale() const noexcept { return volumeDcOn ? level : 1.0; }
    double postOffset() const noexcept { return volumeDcOn ? offset : -curveDc; }
    /** The board filters over a mixer value (mixerOutput) and a post gain (postGain): one internal sample of the output
        in the reference's units. A chain blending configurations across a write passes their blends. */
    double board (double mix, double post) noexcept { return boardInline (mix, post); }

    Model model() const noexcept { return mdl; }
    double gainFactor() const noexcept { return gain; }
    double volumeLevel() const noexcept { return level; }

    /** The fitted constants a renderer needs (render units; decisions "B-Side S3a"). Model A's voice bend is
        render::bend (RenderTables.h). */
    static double voiceDc (Model m) noexcept;
    static double envelopeDc (Model m) noexcept;

private:
    /** A filter state: 0 below 1e-15 (no subnormal numbers). */
    static double flush (double v) noexcept { return std::abs (v) < 1e-15 ? 0.0 : v; }
    double curve (double x) const noexcept { return curveInline (x); }
    BS_CHIP_INLINE double curveInline (double x) const noexcept
    {
        if (! quintic)
        {
            const double c = x < curveMin ? curveMin : (x > curveMax ? curveMax : x); // NaN passes; the guard stops it
            return ((n3 * c + n2) * c + 1.0) * c;
        }
        // Model A: the quintic between its joins, the smooth continuation beyond them (see "N" above; NaN passes)
        if (x < curveMin)
            return loBound + loA * std::exp (loB * (x - curveMin));
        if (x > curveMax)
            return hiBound - hiA * std::exp (hiB * (curveMax - x));
        return ((((n5 * x + n4) * x + n3) * x + n2) * x + 1.0) * x;
    }
    void update() noexcept;

    Model mdl = Model::B;
    double rate = 96000.0;
    bool volumeDcOn = false;
    std::uint8_t routing = 0, modeVolume = 0;
    double filterStaticDc = 0.0;

    int nFilter = 0, nDirect = 3;
    bool externalCounts = false; // setVoices has been called: cntF, cntD come from the renderer
    double cntF = 0.0, cntD = 3.0;
    double envDc = 0.0, filterBusDc = 0.0, staticDirect = 0.0, mixerDcConst = 0.0, voiceDcConst = 0.0;
    double gain = 1.0, level = 0.0, offset = 0.0, curveDc = 0.0;
    bool quintic = false;
    double n2 = 0.0, n3 = 0.0, n4 = 0.0, n5 = 0.0, curveMin = -8.0, curveMax = 8.0;
    double loBound = 0.0, loA = 0.0, loB = 0.0, hiBound = 0.0, hiA = 0.0, hiB = 0.0; // Model A's continuation
    double hpA = 1.0, lpG = 0.5, lpKeep = 0.5, lpIn = 1.0, lpNext = 0.0;
    double hpX = 0.0, hpY = 0.0, lpS = 0.0;
};
} // namespace vs::bside::chip
