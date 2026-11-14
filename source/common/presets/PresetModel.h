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
//   Bank     instrument().bankSize() programs and a name.
//
// What differs between instruments lives in Instrument (the file identity, bank size, slot naming,
// file extensions): nothing instrument-specific is a constant of this generic layer.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace vs::presets
{
inline constexpr int kFormat = 1; // the file format version written and accepted
inline constexpr int kMaxNameChars = 40;
inline constexpr int kMaxCategoryChars = 24;
inline constexpr int kMaxAuthorChars = 40;
/** Largest preset file read. The largest legitimate file is a full bank with every value at its
    longest text and every name at its longest UTF-8 form: 386,487 bytes (0.37 MB) for this instrument's 169
    program parameters (measured by the test "The largest legitimate bank fits the file cap").
    Anything larger is not a preset of ours. */
inline constexpr size_t kMaxFileBytes = 1024u * 1024u;
inline constexpr int kMaxJsonDepth = 12; // our files nest 4 levels deep

/** What identifies an instrument's preset files and shapes its banks. */
struct Instrument
{
    juce::String id;                // written into every file as "instrument"; a file of another instrument is rejected
    int groups = 8, slotsPerGroup = 8; // a bank is groups x slotsPerGroup programs, named A1 ... (letter, then 1-9)
    juce::String programExtension, toneExtension, bankExtension; // with the dot: ".vsprog"

    // ---- Patch tables (docs/architecture.md 8.8): optional text attached to a tone ----
    // An instrument that has none (Vintage) leaves every field below at its default: its files and
    // its saved state are then exactly what they were before tables existed, and a "tables" or
    // "table" key in a file of such an instrument is an unknown key and is ignored.
    /** Checks one table (its lines, without line ends). Returns an empty string when the table is
        acceptable, else the message. Called only for a non-empty table that already passed the
        shape checks below, on the message thread or a host's state thread, never on the audio thread.
        Must not throw (an exception is caught and turns into an error). Setting it turns tables on. */
    std::function<juce::String (const std::vector<std::string>&)> validateTable;
    /** Optional: the canonical lines of a table that validateTable accepted. Tables are stored and saved
        VERBATIM (the user's comments and spacing are kept); the canonical form is used only to compare two
        tables (the modified marker, equality). Without it only identical lines count as equal. */
    std::function<std::vector<std::string> (const std::vector<std::string>&)> canonicaliseTable;
    int maxTableLines = 0;       // lines per table, checked before validateTable (and before anything is copied)
    size_t maxTableBytes = 0;    // the lines with one line end each, summed, checked before validateTable
    size_t maxFileBytes = 0;     // largest preset file read; 0 = kMaxFileBytes
    int maxJsonValues = 0;       // values in one preset file's JSON tree; 0 = the reader's default

    bool hasTables() const noexcept { return static_cast<bool> (validateTable); }
    size_t fileCap() const noexcept { return maxFileBytes != 0 ? maxFileBytes : kMaxFileBytes; }

    int bankSize() const noexcept { return groups * slotsPerGroup; }
    bool isValid() const noexcept;
    /** 0 -> "A1"; with 8 per group 63 -> "H8". Out of range is clamped. */
    juce::String positionName (int index) const;
    /** "A1" -> 0 (case-insensitive, from the start of the text); -1 if it does not start with a slot. */
    int slotFromFileName (const juce::String& fileName) const;
};

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
    Schema (std::vector<ParamSpec> allParams, std::vector<juce::String> tonePrefixes, std::vector<juce::String> excludedIds,
            Instrument instrument);

    /** The schema of a processor: every parameter of its tree, in tree order. */
    static Schema fromProcessor (const juce::AudioProcessor& processor, std::vector<juce::String> tonePrefixes,
                                 std::vector<juce::String> excludedIds, Instrument instrument);

    bool isValid() const noexcept { return valid; }
    const Instrument& instrument() const noexcept { return inst; }
    int bankSize() const noexcept { return inst.bankSize(); }
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
    Instrument inst;
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
    std::vector<std::string> data; // the part's table: its lines (empty = no table); only for an instrument with tables
};

struct Program
{
    Meta meta;
    std::vector<float> values; // schema.numParams() values; excluded parameters hold their default
    /** One table per tone (its lines; empty = no table). Empty or shorter than schema.numTones():
        the missing tones have no table. Only for an instrument with tables. */
    std::vector<std::vector<std::string>> data;
};

struct Bank
{
    juce::String name = "Untitled", author;
    std::vector<Program> programs; // schema.bankSize()
};

// ---- Text rules ----
/** Control characters (C0, DEL, C1) and line or paragraph separators. */
bool isControlCharacter (juce::juce_wchar c) noexcept;
/** Invisible format characters that can make text display misleadingly: bidirectional embedding,
    override and isolate controls, marks and joiners, zero-width and invisible characters, the byte
    order mark, interlinear annotation and tag characters. */
bool isFormatCharacter (juce::juce_wchar c) noexcept;
inline bool isHiddenOrControlCharacter (juce::juce_wchar c) noexcept { return isControlCharacter (c) || isFormatCharacter (c); }

/** Control characters become spaces, format characters are removed, the result is trimmed and cut
    to maxChars characters. An empty result stays empty (callers substitute "Untitled" where a name
    is required). */
juce::String sanitiseText (const juce::String& s, int maxChars);
juce::String sanitiseName (const juce::String& s);

// ---- Tables (Instrument::validateTable) ----
/** `s` cut to at most maxBytes bytes of UTF-8, at a character boundary (never inside a character). */
juce::String truncateUtf8 (const juce::String& s, size_t maxBytes);
/** Lines of one table, joined with one LF after each line (the text the instrument's reader gets). */
std::string joinTableLines (const std::vector<std::string>& lines);
/** The shape every table must have before an instrument's validator sees it: at most
    maxTableLines lines, maxTableBytes bytes (a line end counted after each line), and no line
    break or NUL inside a line (so lines and text correspond one to one). Returns "" if it fits. */
juce::String checkTableShape (const Instrument& instrument, const std::vector<std::string>& lines);
/** Shape check, then the instrument's validator (exceptions caught). "" = accepted; an empty table is accepted. */
juce::String validateTableLines (const Instrument& instrument, const std::vector<std::string>& lines);
/** Splits the text of a session property into lines (an LF ends each line; one final LF does not
    start another), refusing more than maxTableBytes bytes or maxTableLines lines before copying
    anything. Returns "" on success. */
juce::String splitTableText (const Instrument& instrument, const juce::String& text, std::vector<std::string>& lines);

// ---- Construction and conversion ----
Program makeInitProgram (const Schema& schema);
Bank makeInitBank (const Schema& schema);
Tone toneOf (const Schema& schema, const Program& program, int tone);
/** Replaces the tone's parameters in `program` (the metadata of the program is kept). */
void setTone (const Schema& schema, Program& program, int tone, const Tone& t);
} // namespace vs::presets
