#pragma once

// Data model of tones, programs and banks (docs/architecture.md 6b.1), generic over an
// instrument's parameter list. Nothing here runs on the audio thread.
//
//   Schema   the parameter list as presets see it: IDs, ranges, defaults, which parameters belong
//            to a tone (one set per part, identified by an ID prefix such as "lower.") and which
//            are left out of programs (master volume). Built from a processor's parameter tree.
//   Tone     the sound of one part: values for the tone parameters, in the schema's tone order.
//   Program  the whole instrument: one value per schema parameter (indexed like the schema), a name,
//            a category and an author.
//   Bank     kBankSize programs and a name.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>
#include <vector>

namespace vs::presets
{
inline constexpr int kFormat = 1;       // the file format version written and accepted
inline constexpr int kBankSize = 64;    // 8 groups of 8 (A1 ... H8)
inline constexpr int kMaxNameChars = 40;
inline constexpr int kMaxCategoryChars = 24;
inline constexpr int kMaxAuthorChars = 40;
inline constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u; // a bank is about 0.5 MB; anything larger is not ours
inline constexpr int kMaxJsonDepth = 12;                    // our files nest 5 levels deep

/** One parameter as presets see it. Values are plain (the unit the parameter's range uses). */
struct ParamSpec
{
    juce::String id;
    float lo = 0.0f, hi = 1.0f, def = 0.0f;
    bool discrete = false; // choice or stepped integer: values are whole steps from lo
};

class Schema
{
public:
    Schema() = default;

    /** @param tonePrefixes  one ID prefix per part, e.g. { "lower.", "upper." }; every prefix must
                             have the same suffixes in the same order (checked: isValid()).
        @param excludedIds   parameters that are not part of a program (e.g. master volume). */
    Schema (std::vector<ParamSpec> allParams, std::vector<juce::String> tonePrefixes, std::vector<juce::String> excludedIds);

    /** The schema of a processor: every parameter of its tree, in tree order. */
    static Schema fromProcessor (const juce::AudioProcessor& processor, std::vector<juce::String> tonePrefixes,
                                 std::vector<juce::String> excludedIds);

    bool isValid() const noexcept { return valid; }
    int numParams() const noexcept { return static_cast<int> (specs.size()); }
    const ParamSpec& spec (int i) const { return specs[static_cast<size_t> (i)]; }
    /** Index of the parameter with this ID, or -1. */
    int indexOf (const juce::String& id) const;
    bool isExcluded (int i) const { return excluded[static_cast<size_t> (i)]; }

    int numTones() const noexcept { return static_cast<int> (tonePrefixes.size()); }
    /** "lower" for the prefix "lower.": also the key of the tone inside a program file. */
    juce::String toneKey (int tone) const { return toneKeys[static_cast<size_t> (tone)]; }
    int toneSize() const noexcept { return static_cast<int> (toneSuffixes.size()); }
    const juce::String& toneSuffix (int k) const { return toneSuffixes[static_cast<size_t> (k)]; }
    /** Schema index of tone parameter k of part `tone`. */
    int toneParam (int tone, int k) const { return toneIndex[static_cast<size_t> (tone)][static_cast<size_t> (k)]; }
    const std::vector<int>& globalParams() const noexcept { return globals; }

    /** A raw number from a file or a host, made valid for parameter i: non-finite -> default,
        clamped to the range, and a discrete parameter rounded to a whole step. */
    float sanitise (int i, double raw) const noexcept;
    std::vector<float> defaults() const;

private:
    std::vector<ParamSpec> specs;
    std::vector<juce::String> tonePrefixes, toneKeys, toneSuffixes;
    std::vector<std::vector<int>> toneIndex; // [tone][k]
    std::vector<int> globals;
    std::vector<bool> excluded;
    bool valid = false;
};

struct Meta
{
    juce::String name = "Untitled", category, author;
};

struct Tone
{
    Meta meta;
    std::vector<float> values; // schema.toneSize() values
};

struct Program
{
    Meta meta;
    std::vector<float> values; // schema.numParams() values; excluded parameters hold their default
};

struct Bank
{
    juce::String name = "Untitled", author;
    std::vector<Program> programs; // kBankSize
};

// ---- Text rules ----
/** Control characters become spaces, the result is trimmed and cut to maxChars characters.
    An empty result stays empty (callers substitute "Untitled" where a name is required). */
juce::String sanitiseText (const juce::String& s, int maxChars);
juce::String sanitiseName (const juce::String& s);

// ---- Construction and conversion ----
Program makeInitProgram (const Schema& schema);
Bank makeInitBank (const Schema& schema);
Tone toneOf (const Schema& schema, const Program& program, int tone);
/** Replaces the tone's parameters in `program` (the metadata of the program is kept). */
void setTone (const Schema& schema, Program& program, int tone, const Tone& t);
} // namespace vs::presets
