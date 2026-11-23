#pragma once

// Model A's cutoff law as the S3a form had it (commit 44bc753, source/bside/chip/Filter.cpp), kept for the tests that
// pin Filter's corners up to and including register value 479.0 to it bit for bit (docs/architecture.md 8.19.3, owner
// decision O9). The functions are copied verbatim into their own translation unit (S3aCutoffLaw.cpp), with the same
// function boundaries, so that the compiler rounds them as it rounded the S3a build. Test code only.

namespace vs::bside::test::s3a
{
/** The S3a Filter::cutoffCurveHz for Model A: register value (clamped 0 .. 2047), variation (clamped 0 .. 1). */
double cutoffCurveHzA (double registerValue, double variationValue) noexcept;
/** The corner (Hz, resonance 0) S3a's processWithCutoff computed for a register value at a stored variation. */
double cornerWithCutoffHzA (float registerValue, float variation) noexcept;
} // namespace vs::bside::test::s3a
