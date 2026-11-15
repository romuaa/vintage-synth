// B-Side's patch tables in presets and state (docs/architecture.md 8.8), with the real table reader:
// program, tone and bank files, session state, the hand-over to the audio thread inside the ApplyGate
// hold, the file caps (the largest legitimate bank) and hostile files and sessions.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../unit/TableTestUtil.h"
#include "bside/PluginProcessor.h"
#include "bside/presets/BSidePresets.h"
#include "bside/presets/BSideTables.h"
#include "common/presets/StrictJson.h"

#if defined(__APPLE__)
 #include "../../seq_unit/AllocationCounter.h"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace vs::presets;
namespace prog = vs::bside::program;

namespace
{
struct TempDir
{
    juce::File dir;
    TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bs_table_test_" + juce::Uuid().toString())) {}
    ~TempDir() { dir.deleteRecursively(); }
    juce::File operator/ (const juce::String& name) const { return dir.getChildFile (name); }
};

struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    bs::BSideProcessor proc;
    TempDir temp;

    Rig()
    {
        presets().setLocations ({ temp.dir });
        presets().getGate().setTimeouts (0, 0); // nothing runs on another thread unless a test starts it
        REQUIRE (temp.dir.createDirectory());
    }

    PresetManager& presets() { return proc.getPresets(); }
    const Schema& schema() { return proc.getPresets().getSchema(); }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }
    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    juce::MemoryBlock state()
    {
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        return mb;
    }
    std::unique_ptr<juce::XmlElement> stateXml() { auto mb = state(); return juce::AudioProcessor::getXmlFromBinary (mb.getData(), static_cast<int> (mb.getSize())); }
    void restore (const juce::XmlElement& xml)
    {
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (xml, mb);
        proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
    }

    /** The compiled program a part's audio side would use right now (one block's read, no gate involved). */
    prog::Program compiled (int part)
    {
        auto& h = proc.tableHandover (part);
        const prog::Program* p = h.beginAcquire();
        REQUIRE (p != nullptr);
        h.commit();
        return h.inUse();
    }
};

std::string readFileText (const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    return std::string (static_cast<const char*> (mb.getData()), mb.getSize());
}

std::vector<std::string> linesOf (const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
        {
            out.emplace_back (text, start, i - start);
            start = i + 1;
        }
    if (start < text.size())
        out.emplace_back (text, start, text.size() - start);
    return out;
}

prog::Program compileText (const std::string& text)
{
    const auto r = prog::readTable (text);
    REQUIRE (r.ok);
    return r.program;
}

Program programWith (const Schema& schema, const std::string& lower, const std::string& upper, const juce::String& name = "Tabby")
{
    auto p = makeInitProgram (schema);
    p.meta.name = name;
    p.data = { linesOf (lower), linesOf (upper) };
    return p;
}

/** The canonical text of a table text. */
std::string canonical (const std::string& text)
{
    const auto r = prog::readTable (text);
    REQUIRE (r.ok);
    std::string out;
    REQUIRE (prog::writeTable (r.program, out));
    return out;
}

double secondsOf (const std::function<void()>& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}

/** Bytes requested from the allocator on this thread while it lives (macOS only; zero elsewhere or under ASan). */
#if defined(VS_SEQ_COUNT_ALLOCATIONS) && VS_SEQ_COUNT_ALLOCATIONS
std::atomic<std::uint64_t> gBytes { 0 };
std::atomic<pthread_t> gThread { nullptr };
void countBytes (std::uint32_t type, std::uintptr_t, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t, std::uint32_t)
{
    constexpr std::uint32_t kAllocate = 2, kDeallocate = 4;
    if ((type & kAllocate) != 0 && pthread_equal (pthread_self(), gThread.load()) != 0)
        gBytes.fetch_add ((type & kDeallocate) != 0 ? a3 : a2, std::memory_order_relaxed); // realloc: (old, new size)
}
struct ByteCounter
{
    ByteCounter() { gBytes = 0; gThread = pthread_self(); malloc_logger = countBytes; }
    ~ByteCounter() { malloc_logger = nullptr; gThread = nullptr; }
    std::uint64_t bytes() const { return gBytes.load(); }
    static constexpr bool available = true;
};
#else
struct ByteCounter
{
    std::uint64_t bytes() const { return 0; }
    static constexpr bool available = false;
};
#endif

const std::string kTableA = bst::kSnare;
const std::string kTableB = bst::kArpeggio;
const std::string kTableC = bst::kFilterBass;
} // namespace

TEST_CASE ("B-Side tables: the instrument has them, with limits that match the table format", "[bside][tables][presets]")
{
    const auto inst = bs::bsideInstrument();
    CHECK (inst.hasTables());
    CHECK (inst.isValid());
    CHECK (inst.maxTableLines == prog::kMaxLines);
    CHECK (inst.maxTableBytes == prog::kMaxTextBytes);
    CHECK (inst.fileCap() == 12u * 1024u * 1024u);
    CHECK (inst.maxJsonValues == 160000);
    CHECK (bs::validateTableText (linesOf (kTableA)).isEmpty());
    CHECK (bs::validateTableText ({ "not a table" }).contains ("line 1"));
    // The canonical form is stable and itself a table.
    for (const auto& text : { kTableA, kTableB, kTableC })
    {
        const auto lines = bs::canonicalTableLines (linesOf (text));
        REQUIRE_FALSE (lines.empty());
        CHECK (bs::validateTableText (lines).isEmpty());
        CHECK (bs::canonicalTableLines (lines) == lines);
        CHECK (joinTableLines (lines) == canonical (text));
    }
    CHECK (bs::canonicalTableLines ({ "garbage" }).empty());
}

TEST_CASE ("B-Side tables: applying a program publishes the compiled tables of both parts", "[bside][tables][presets]")
{
    Rig rig;
    auto& pm = rig.presets();
    CHECK (rig.compiled (0) == prog::emptyProgram());
    CHECK (rig.compiled (1) == prog::emptyProgram());

    pm.applyProgram (programWith (rig.schema(), kTableA, kTableB), 3, false);
    CHECK (rig.compiled (0) == compileText (kTableA));
    CHECK (rig.compiled (1) == compileText (kTableB));
    CHECK (pm.currentTable (0) == linesOf (kTableA)); // held verbatim: comments and spacing kept
    CHECK (pm.captureProgram().data[1] == linesOf (kTableB));
    CHECK (pm.tableError (0).isEmpty());

    // Applying a program without tables (Init) clears both.
    pm.initialise();
    CHECK (rig.compiled (0) == prog::emptyProgram());
    CHECK (rig.compiled (1) == prog::emptyProgram());
    CHECK (pm.currentTable (0).empty());

    // A tone puts its table into the chosen part only.
    pm.applyProgram (programWith (rig.schema(), kTableA, kTableB), 3, false);
    Tone tone = toneOf (rig.schema(), makeInitProgram (rig.schema()), 0);
    tone.data = linesOf (kTableC);
    pm.applyTone (tone, 0);
    CHECK (rig.compiled (0) == compileText (kTableC));
    CHECK (rig.compiled (1) == compileText (kTableB));

    // setTable: validated, and a table is program content.
    CHECK (pm.setTable (1, linesOf (kTableA)).isEmpty());
    CHECK (rig.compiled (1) == compileText (kTableA));
    CHECK (pm.isModified());
    const auto error = pm.setTable (1, { "bside-table 1", "[wave1]", "frobnicate" });
    CHECK (error.startsWith ("Upper table: line 3, column 1"));
    CHECK (rig.compiled (1) == compileText (kTableA)); // unchanged
}

TEST_CASE ("B-Side tables: program, tone and bank files round trip and re-save byte-identical", "[bside][tables][presets][files]")
{
    Rig rig;
    auto& pm = rig.presets();
    pm.applyProgram (programWith (rig.schema(), kTableA, kTableB, "Round Trip"), 0, false);

    const auto file = rig.temp / "a.bsprog";
    REQUIRE (pm.saveProgramFile (file).isEmpty());
    const auto text = readFileText (file);
    CHECK (text.find ("\"tables\": {") != std::string::npos);
    CHECK (text.find ("\"bside-table 1\"") != std::string::npos);
    CHECK (text.find ("# gate off: the release rings out") != std::string::npos); // the user's comments are kept

    pm.initialise();
    REQUIRE (pm.loadProgramFile (file).isEmpty());
    CHECK (rig.compiled (0) == compileText (kTableA));
    CHECK (rig.compiled (1) == compileText (kTableB));
    const auto file2 = rig.temp / "a2.bsprog";
    REQUIRE (pm.saveProgramFile (file2).isEmpty());
    CHECK (readFileText (file2) == text);

    // Tone: saved from Upper, loaded into Lower.
    const auto toneFile = rig.temp / "t.bstone";
    REQUIRE (pm.saveToneFile (toneFile, 1).isEmpty());
    CHECK (readFileText (toneFile).find ("\"table\": [") != std::string::npos);
    REQUIRE (pm.loadToneFile (toneFile, 0).isEmpty());
    CHECK (rig.compiled (0) == compileText (kTableB));
    CHECK (rig.compiled (1) == compileText (kTableB));

    // Bank: store the program into slot 9 of a user bank, reload the bank, select the slot.
    const auto bankFile = rig.temp / "b.bsbank";
    REQUIRE (pm.saveBankFile (bankFile, "Tables").isEmpty());
    pm.applyProgram (programWith (rig.schema(), kTableC, kTableA, "Slot Nine"), 9, true);
    REQUIRE (pm.storeInBank (9).isEmpty());
    const auto bankText = readFileText (bankFile);
    pm.initialise();
    REQUIRE (pm.loadBankFile (bankFile).isEmpty()); // selects slot 0: no tables
    CHECK (rig.compiled (0) == prog::emptyProgram());
    REQUIRE (pm.selectProgram (9));
    CHECK (rig.compiled (0) == compileText (kTableC));
    CHECK (rig.compiled (1) == compileText (kTableA));
    REQUIRE (pm.saveBankFile (rig.temp / "b2.bsbank", "Tables").isEmpty());
    CHECK (readFileText (rig.temp / "b2.bsbank") == bankText);

    // The shipped factory files (no "tables" key) still load: no table.
    CHECK (rig.presets().isFactoryBank() == false); // we are on a user bank now; the factory is built in
    const auto schema = rig.schema();
    juce::StringArray errors;
    const auto factory = bs::makeBSideFactoryBank (schema, &errors);
    CHECK (errors.isEmpty());
    CHECK (factory.programs[0].data.size() == 2);
    CHECK (factory.programs[0].data[0].empty());
}

TEST_CASE ("B-Side tables: a bad table rejects the whole file, named by part and program; nothing changes", "[bside][tables][presets][files]")
{
    Rig rig;
    auto& pm = rig.presets();
    pm.applyProgram (programWith (rig.schema(), kTableA, kTableB, "Keep"), 1, false);
    const auto before = pm.captureProgram();
    const auto generationBefore = rig.proc.tableHandover (0).publishes();

    auto programJson = [&] (const std::string& lowerLine) {
        auto p = programWith (rig.schema(), kTableA, kTableB, "Broken One");
        p.data[0] = { "bside-table 1", "[wave1]", lowerLine };
        return toJson (rig.schema(), p);
    };
    const auto bad = rig.temp / "bad.bsprog";
    REQUIRE (bad.replaceWithText (juce::String (programJson ("frobnicate 3"))));
    const auto error = pm.loadProgramFile (bad);
    INFO (error);
    CHECK (error.contains ("Lower table of \"Broken One\": line 3, column 1"));
    CHECK (error.contains ("bad.bsprog"));
    CHECK (pm.currentTable (0) == before.data[0]);
    CHECK (pm.currentName() == "Keep");
    CHECK (rig.proc.tableHandover (0).publishes() == generationBefore);
    CHECK (rig.compiled (0) == compileText (kTableA));

    // Upper, in a tone file, and in a bank (the program number is part of the message).
    auto p2 = programWith (rig.schema(), kTableA, kTableB, "Broken Two");
    p2.data[1] = { "bside-table 1", "[nothing]" };
    const auto t = toneOf (rig.schema(), p2, 1);
    REQUIRE ((rig.temp / "bad.bstone").replaceWithText (juce::String (toJson (rig.schema(), t))));
    CHECK (pm.loadToneFile (rig.temp / "bad.bstone", 0).contains ("Table of \"Broken Two\": line 2"));
    auto bank = makeInitBank (rig.schema());
    bank.programs[6] = p2;
    REQUIRE ((rig.temp / "bad.bsbank").replaceWithText (juce::String (toJson (rig.schema(), bank))));
    const auto bankError = pm.loadBankFile (rig.temp / "bad.bsbank");
    INFO (bankError);
    CHECK (bankError.contains ("Program 7: Upper table of \"Broken Two\": line 2"));
    CHECK (pm.currentTable (0) == before.data[0]);
    CHECK (rig.compiled (1) == compileText (kTableB));
    CHECK (rig.proc.tableHandover (0).publishes() == generationBefore);
}

TEST_CASE ("B-Side tables: line, byte and shape limits of one table", "[bside][tables][presets][security]")
{
    Rig rig;
    const auto& schema = rig.schema();
    auto parse = [&] (const std::vector<std::string>& lines) {
        auto p = makeInitProgram (schema);
        p.data = { lines, {} };
        // Written without the validator's help: the model's lines go into the file as given.
        return parseProgram (schema, toJson (schema, p));
    };
    // 1024 lines are the most; 1025 are refused before the reader sees them.
    std::vector<std::string> many (1024, "# c");
    many[0] = "bside-table 1";
    CHECK (parse (many).ok);
    many.push_back ("# one too many");
    const auto r1 = parse (many);
    CHECK_FALSE (r1.ok);
    CHECK (r1.error.contains ("more than 1024 lines"));

    // A line over 200 bytes: the table reader's own limit, reported with line and column.
    const auto r2 = parse ({ "bside-table 1", "# " + std::string (199, 'x') });
    CHECK_FALSE (r2.ok);
    CHECK (r2.error.contains ("line 2"));
    CHECK (parse ({ "bside-table 1", "# " + std::string (198, 'x') }).ok);

    // 32 768 bytes of text with line ends; one byte more is refused before the reader.
    std::vector<std::string> big { "bside-table 1" };
    size_t bytes = 14;
    while (bytes + 201 <= prog::kMaxTextBytes)
    {
        big.push_back ("#" + std::string (198, 'y'));
        bytes += 200;
    }
    big.push_back ("#" + std::string (prog::kMaxTextBytes - bytes - 2, 'z'));
    bytes = joinTableLines (big).size();
    REQUIRE (bytes == prog::kMaxTextBytes);
    CHECK (parse (big).ok);
    big.back() += "z";
    const auto r3 = parse (big);
    CHECK_FALSE (r3.ok);
    CHECK (r3.error.contains ("longer than 32768 bytes"));

    // Embedded control characters, NUL and invalid UTF-8 are the reader's rejections, never crashes.
    for (const std::string& line : { std::string ("# a\x01"), std::string ("# \xff\xfe"), std::string ("# \xc2\x85"), std::string ("# a\tb\x7f") })
        CHECK_FALSE (parse ({ "bside-table 1", line }).ok);
    CHECK_FALSE (parseProgram (schema, "{\"format\":1,\"instrument\":\"b-side\",\"kind\":\"program\",\"tables\":{\"lower\":[\"bside-table 1\",\"# a\\u0000b\"]}}").ok);
    CHECK_FALSE (parseProgram (schema, "{\"format\":1,\"instrument\":\"b-side\",\"kind\":\"program\",\"tables\":{\"lower\":[\"bside-table 1\\n# x\"]}}").ok);
}

TEST_CASE ("B-Side tables: the largest legitimate bank fits the caps", "[bside][tables][presets][files]")
{
    Rig rig;
    const auto& schema = rig.schema();
    const auto inst = schema.instrument();
    const auto jsonBytesOfTable = [&] (const std::vector<std::string>& lines) {
        Tone t = toneOf (schema, makeInitProgram (schema), 0);
        t.data = lines;
        return toJson (schema, t).size();
    };

    // (0) The worst byte, found by code: every byte value that the table reader accepts in a line, and
    // every code point (a stride over the multi-byte ones), with the bytes the file writer needs for it.
    double worstRatio = 0.0;
    int worstByte = -1;
    const size_t base = jsonBytesOfTable ({ "bside-table 1", "# " });
    for (int byte = 1; byte < 256; ++byte)
    {
        const std::string line = std::string ("# ") + static_cast<char> (byte);
        if (! bs::validateTableText ({ "bside-table 1", line }).isEmpty())
            continue;
        const double ratio = static_cast<double> (jsonBytesOfTable ({ "bside-table 1", line }) - base);
        if (ratio > worstRatio)
        {
            worstRatio = ratio;
            worstByte = byte;
        }
    }
    int multiByteChecked = 0;
    for (juce::juce_wchar cp = 0x80; cp <= 0x10FFFF; cp += (cp < 0x3000 ? 1 : 61))
    {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        const auto utf8 = juce::String::charToString (cp).toStdString();
        if (! bs::validateTableText ({ "bside-table 1", "# " + utf8 }).isEmpty())
            continue;
        ++multiByteChecked;
        const double ratio = static_cast<double> (jsonBytesOfTable ({ "bside-table 1", "# " + utf8 }) - base) / static_cast<double> (utf8.size());
        CHECK (ratio <= 1.0); // written as they are
    }
    std::printf ("worst byte of a table line in a file: 0x%02x, %.0f bytes written for 1; %d multi-byte characters checked, at most 1 byte per byte\n",
                 worstByte, worstRatio, multiByteChecked);
    CHECK (worstRatio == Catch::Approx (2.0));
    CHECK ((worstByte == '"' || worstByte == '\\' || worstByte == '\t'));
    // Every byte that costs the worst ratio, by code: a tab, a quotation mark, a backslash (not a long unicode escape).
    for (const int byte : { '\t', '"', '\\' })
        CHECK (jsonBytesOfTable ({ "bside-table 1", std::string ("# ") + static_cast<char> (byte) }) - base == 2);

    // The bytes one line costs besides its text (indent, quotes, comma, line end), measured in a bank: write
    // the bank with 1000 empty lines in each of its 128 tables, and with no table.
    const auto bankWith = [&] (const std::vector<std::string>& lines) {
        Bank bank = makeInitBank (schema);
        for (auto& p : bank.programs)
            p.data = { lines, lines };
        return toJson (schema, bank).size();
    };
    const double lineOverhead = static_cast<double> (bankWith (std::vector<std::string> (1000, "")) - bankWith ({})) / (128.0 * 1000.0);
    // Worst case of a table: the most lines and the most bytes the reader accepts, every byte the worst one.
    // JSON bytes = lines x overhead + worst ratio x (bytes - lines); with 1024 lines and 32 768 bytes:
    const double worstTable = 1024.0 * lineOverhead + worstRatio * (32768.0 - 1024.0);

    // Lines that are nothing but the worst byte: a table line may be white space (tabs) and its header may be
    // followed by white space; with quotation marks the lines must be comments, which costs one byte in 31.
    const auto makeWorstLines = [&] (char worstChar) {
        const std::string prefix = worstChar == '\t' ? "" : "#";
        std::vector<std::string> lines { std::string ("bside-table 1") + (worstChar == '\t' ? "" : " #") };
        lines[0] += std::string (31 - lines[0].size(), worstChar);
        while (lines.size() < 1024)
            lines.push_back (prefix + std::string (31 - prefix.size(), worstChar));
        lines.back() += std::string (prog::kMaxTextBytes - joinTableLines (lines).size(), worstChar);
        REQUIRE (joinTableLines (lines).size() == prog::kMaxTextBytes);
        REQUIRE (lines.size() == 1024);
        INFO (bs::validateTableText (lines));
        REQUIRE (bs::validateTableText (lines).isEmpty());
        return lines;
    };
    // Tabs (the case in which the old escape cost 6 bytes for 1) and quotation marks.
    const auto tabs = makeWorstLines ('\t');
    const auto quotes = makeWorstLines ('"');

    size_t worstBank = 0;
    for (const auto& worst : { tabs, quotes })
    {
        Bank bank = makeInitBank (schema);
        const auto longName = juce::String::repeatedString (juce::String::fromUTF8 ("\xf0\x9f\x8e\xb9"), kMaxAuthorChars);
        for (auto& p : bank.programs)
        {
            p.meta.name = longName;
            p.meta.category = longName;
            p.meta.author = longName;
            p.data = { worst, worst };
            for (int i = 0; i < schema.numParams(); ++i)
            {
                const auto& s = schema.spec (i);
                constexpr float tiny = 1.17549435e-38f;
                float v = s.lo + (s.hi - s.lo) * 0.123456789f;
                if (s.discrete)
                    v = s.hi;
                else if (s.lo <= -tiny)
                    v = -tiny;
                p.values[static_cast<size_t> (i)] = v;
            }
        }
        const auto text = toJson (schema, bank);
        worstBank = std::max (worstBank, text.size());
        std::printf ("largest legitimate bank (%s lines): %zu bytes; cap %zu\n", worst == tabs ? "tab" : "quotation mark", text.size(), inst.fileCap());

        json::Limits limits;
        limits.maxInputBytes = inst.fileCap();
        limits.maxValues = 1 << 24;
        limits.maxElements = 4096;
        json::Document doc;
        std::string why;
        REQUIRE (doc.parse (text.data(), text.size(), limits, why));
        std::printf ("  json values in that bank: %zu (cap %d)\n", doc.numValues(), inst.maxJsonValues);
        CHECK (doc.numValues() <= static_cast<size_t> (inst.maxJsonValues) * 90 / 100);

        Parsed<Bank> back;
        const auto seconds = secondsOf ([&] { back = parseBank (schema, text); });
        std::printf ("  parse + validate of 128 maximal tables: %.3f s\n", seconds);
        REQUIRE (back.ok);
        CHECK (back.value.programs[63].data[1] == worst);
        CHECK (toJson (schema, back.value) == text);
        CHECK (seconds < 5.0);

        // A save of it is accepted, and what was saved loads again: a writer never makes what its reader refuses.
        const auto file = rig.temp / "max.bsbank";
        REQUIRE (file.replaceWithData (text.data(), text.size()));
        REQUIRE (rig.presets().loadBankFile (file).isEmpty());
        const auto saved = rig.temp / "max2.bsbank";
        REQUIRE (rig.presets().saveBankFile (saved).isEmpty());
        CHECK (readFileText (saved) == text);
        REQUIRE (rig.presets().loadBankFile (saved).isEmpty());
    }
    const double tableShare = 128.0 * worstTable;
    std::printf ("worst case by formula: %.0f bytes per table x 128 = %.0f; measured worst bank %zu bytes; cap %zu (%.0f %% headroom)\n", worstTable,
                 tableShare, worstBank, inst.fileCap(),
                 100.0 * (static_cast<double> (inst.fileCap()) / static_cast<double> (worstBank) - 1.0));
    CHECK (static_cast<double> (worstBank) <= tableShare + 500000.0); // the tables plus well under 0.5 MB of parameters and names
    CHECK (static_cast<double> (worstBank) >= tableShare);            // and the constructed bank really is the extreme
    CHECK (static_cast<double> (inst.fileCap()) >= static_cast<double>(worstBank) * 1.15); // at least 15 % to spare

    // (2) Large canonical tables (the table format's own bounds are tested in tests/bside/unit): 496 of the 512 commands.
    std::string t;
    for (const char* lane : { "wave1", "wave2", "wave3" })
        t += std::string ("[") + lane + "]\n" + bst::repeatLine ("wave tri+saw+pulse+gate -96, adsr f f f f, pitch -96.00 ticks 255", 21)
             + bst::repeatLine ("pitch +1 ticks 3", 43);
    for (const char* lane : { "pulse1", "pulse2", "pulse3" })
        t += std::string ("[") + lane + "]\n" + bst::repeatLine ("set 100", 40);
    t += "[filter]\n" + bst::repeatLine ("res 3", 64);
    INFO (bs::validateTableText (linesOf ("bside-table 1\n" + t)));
    const auto canon = bs::canonicalTableLines (linesOf ("bside-table 1\n" + t));
    REQUIRE_FALSE (canon.empty());
    CHECK (joinTableLines (canon).size() <= prog::kMaxTextBytes);
    Bank canonicalBank = makeInitBank (schema);
    for (auto& p : canonicalBank.programs)
        p.data = { canon, canon };
    const auto canonicalText = toJson (schema, canonicalBank);
    std::printf ("  64 programs with two large tables (%zu bytes of text each): %zu bytes\n", joinTableLines (canon).size(), canonicalText.size());
    CHECK (canonicalText.size() < inst.fileCap() / 2);
    CHECK (parseBank (schema, canonicalText).ok);
}

TEST_CASE ("B-Side tables: the user's text is kept exactly, through apply, capture, files and the session", "[bside][tables][presets][state]")
{
    // Comments, odd spacing, tabs, blank lines, a trailing comment and unusual label names.
    const std::string odd = "# my snare, take 3\n"
                            "bside-table 1   # header with a comment\n"
                            "\n"
                            "[wave1]\t\t# lane\n"
                            "start:\n"
                            "\t\tpitch   +0,gate on     ticks   2   # tab indent\n"
                            "     jump   start\n"
                            "\n"
                            "[pulse1]\n"
                            "   set 2048#no space before the comment\n";
    REQUIRE (prog::readTable (odd).ok);
    Rig a, b;
    auto& pm = a.presets();
    pm.applyProgram (programWith (a.schema(), odd, kTableB, "Odd"), 5, false);
    CHECK (pm.currentTable (0) == linesOf (odd));
    CHECK (joinTableLines (pm.currentTable (0)) == odd);
    CHECK (a.compiled (0) == compileText (odd));
    CHECK (pm.captureProgram().data[0] == linesOf (odd));
    CHECK (pm.captureTone (0).data == linesOf (odd));

    // Files: program, tone, bank. Saved text contains the user's lines; load gives them back; re-save is byte-identical.
    const auto file = a.temp / "odd.bsprog";
    REQUIRE (pm.saveProgramFile (file).isEmpty());
    const auto fileText = readFileText (file);
    CHECK (fileText.find ("# my snare, take 3") != std::string::npos);
    CHECK (fileText.find ("\\t\\tpitch   +0,gate on     ticks   2   # tab indent") != std::string::npos);
    CHECK (fileText.find ("\\u0009") == std::string::npos); // tabs are written with the short escape
    pm.initialise();
    REQUIRE (pm.loadProgramFile (file).isEmpty());
    CHECK (joinTableLines (pm.currentTable (0)) == odd);
    REQUIRE (pm.saveProgramFile (a.temp / "odd2.bsprog").isEmpty());
    CHECK (readFileText (a.temp / "odd2.bsprog") == fileText);
    const auto toneFile = a.temp / "odd.bstone";
    REQUIRE (pm.saveToneFile (toneFile, 0).isEmpty());
    REQUIRE (pm.loadToneFile (toneFile, 1).isEmpty());
    CHECK (joinTableLines (pm.currentTable (1)) == odd);
    const auto bankFile = a.temp / "odd.bsbank";
    REQUIRE (pm.saveBankFile (bankFile, "Odd bank").isEmpty());
    REQUIRE (pm.storeInBank (7).isEmpty());
    pm.initialise();
    REQUIRE (pm.loadBankFile (bankFile).isEmpty());
    REQUIRE (pm.selectProgram (7));
    CHECK (joinTableLines (pm.currentTable (0)) == odd);

    // Session: the text attribute is the user's text, and restoring it gives the same text back, byte for byte.
    const auto saved = a.state();
    const auto xml = a.stateXml();
    CHECK (xml->getChildByName ("Tables")->getStringAttribute ("lower").toStdString() == odd);
    b.proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    CHECK (joinTableLines (b.presets().currentTable (0)) == odd);
    CHECK (b.compiled (0) == compileText (odd));
    const auto again = b.state();
    CHECK (again.getSize() == saved.getSize());
    CHECK (std::memcmp (again.getData(), saved.getData(), saved.getSize()) == 0);

    // The modified marker looks at the canonical form: comments and spacing are not a change of sound.
    pm.applyProgram (programWith (a.schema(), odd, kTableB, "Odd"), 5, false);
    REQUIRE_FALSE (pm.isModified());
    CHECK (pm.setTable (0, linesOf (canonical (odd))).isEmpty());
    CHECK_FALSE (pm.isModified());
    CHECK (pm.currentTable (0) == linesOf (canonical (odd))); // the text the user gave is the text kept
    CHECK (pm.setTable (0, linesOf (kTableC)).isEmpty());
    CHECK (pm.isModified());
}

TEST_CASE ("B-Side tables: error messages are valid UTF-8, cut between characters", "[bside][tables][presets]")
{
    // Eight errors, each quoting a long word of multi-byte characters: far over 1024 bytes in all.
    for (const char* unit : { "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x8e\xb9" })
        for (int lead = 0; lead < 4; ++lead)
        {
            std::vector<std::string> lines { "bside-table 1", "[wave1]" };
            for (int i = 0; i < 12; ++i)
            {
                std::string word (static_cast<size_t> (lead), 'x');
                for (int k = 0; k < 60; ++k)
                    word += unit;
                lines.push_back ("    " + word);
            }
            const auto message = bs::validateTableText (lines);
            INFO (unit << " lead " << lead << " message of " << message.getNumBytesAsUTF8() << " bytes");
            CHECK (message.isNotEmpty());
            CHECK (message.getNumBytesAsUTF8() <= 1024);
            CHECK (juce::CharPointer_UTF8::isValidString (message.toRawUTF8(), 4096));
        }
    // And through the common function, with a message that is exactly 1024 bytes long, and one byte more.
    auto inst = bs::bsideInstrument();
    for (const size_t extra : { size_t (0), size_t (1), size_t (2), size_t (3) })
    {
        inst.validateTable = [=] (const std::vector<std::string>&) { return juce::String (std::string (1023 - 1 + extra, 'x')) + juce::String::fromUTF8 ("\xf0\x9f\x8e\xb9"); };
        const auto message = validateTableLines (inst, { "x" });
        CHECK (message.getNumBytesAsUTF8() <= 1024);
        CHECK (juce::CharPointer_UTF8::isValidString (message.toRawUTF8(), 4096));
    }
}

TEST_CASE ("B-Side tables: files over the caps, and hostile files, are refused with bounded time and memory", "[bside][tables][presets][files][security]")
{
    Rig rig;
    const auto& schema = rig.schema();
    auto& pm = rig.presets();
    const auto cap = schema.instrument().fileCap();
    const auto before = pm.captureProgram();

    // A file one byte over the cap is refused by the reader before any parsing (and before being read whole).
    {
        const auto file = rig.temp / "huge.bsbank";
        REQUIRE (file.getParentDirectory().createDirectory());
        {
            juce::FileOutputStream out (file);
            REQUIRE (out.openedOk());
            const std::string chunk (1 << 20, ' ');
            for (size_t written = 0; written <= cap; written += chunk.size())
                out.write (chunk.data(), chunk.size());
        }
        CHECK (file.getSize() > static_cast<juce::int64> (cap));
        ByteCounter counter;
        const auto seconds = secondsOf ([&] { CHECK (pm.loadBankFile (file).isNotEmpty()); });
        CHECK (seconds < 2.0);
        if (ByteCounter::available)
            CHECK (counter.bytes() < 4u * 1024u * 1024u); // nothing like the cap's worth was read or allocated
    }

    auto header = [] (const char* kind) { return std::string ("{\"format\":1,\"instrument\":\"b-side\",\"kind\":\"") + kind + "\","; };
    struct Hostile
    {
        const char* what;
        std::string text;
        double maxSeconds;
        std::uint64_t maxBytes;
    };
    std::vector<Hostile> files;

    // Many programs, each with two tables of 1024 empty lines: over the JSON value cap long before the end.
    {
        std::string lines = "[";
        for (int i = 0; i < 1024; ++i)
            lines += i ? ",\"\"" : "\"\"";
        lines += "]";
        std::string programs;
        for (int i = 0; i < 200; ++i)
            programs += std::string (i ? "," : "") + "{\"tables\":{\"lower\":" + lines + ",\"upper\":" + lines + "}}";
        files.push_back ({ "200 programs of 2 x 1024 empty lines", header ("bank") + "\"programs\":[" + programs + "]}", 2.0, 80u << 20 });
    }
    // One table with a million empty lines; a table list of a million nested lists; a 10 MB single line.
    {
        std::string s = "[";
        for (int i = 0; i < 1000000; ++i)
            s += i ? ",\"\"" : "\"\"";
        files.push_back ({ "a million empty lines", header ("program") + "\"tables\":{\"lower\":" + s + "]}}", 2.0, 40u << 20 });
        files.push_back ({ "a million nested lists", header ("program") + "\"tables\":{\"lower\":" + std::string (1000000, '[') + "}}", 2.0, 40u << 20 });
        files.push_back ({ "a 10 MB line", header ("program") + "\"tables\":{\"lower\":[\"" + std::string (10u << 20, 'x') + "\"]}}", 2.0, 60u << 20 });
    }
    // Wrong types, duplicate keys, deep nesting, non-UTF-8, embedded NUL.
    files.push_back ({ "tables is a list", header ("program") + "\"tables\":[\"bside-table 1\"]}", 0.5, 8u << 20 });
    files.push_back ({ "lower is text", header ("program") + "\"tables\":{\"lower\":\"bside-table 1\"}}", 0.5, 8u << 20 });
    files.push_back ({ "lines are numbers", header ("program") + "\"tables\":{\"lower\":[1,2,3]}}", 0.5, 8u << 20 });
    files.push_back ({ "duplicate lower", header ("program") + "\"tables\":{\"lower\":[],\"lower\":[]}}", 0.5, 8u << 20 });
    files.push_back ({ "duplicate tables", header ("program") + "\"tables\":{},\"tables\":{}}", 0.5, 8u << 20 });
    files.push_back ({ "deep nesting", header ("program") + "\"tables\":{\"lower\":" + std::string (200, '[') + std::string (200, ']') + "}}", 0.5, 8u << 20 });
    files.push_back ({ "non-UTF-8 line", header ("program") + "\"tables\":{\"lower\":[\"bside-table 1\",\"# \xff\xfe\"]}}", 0.5, 8u << 20 });
    files.push_back ({ "embedded NUL", [&] { auto s = header ("program") + "\"tables\":{\"lower\":[\"bside-table 1\",\"# a"; s += '\0'; return s + "b\"]}}"; }(), 0.5, 8u << 20 });
    files.push_back ({ "line over 200 bytes", header ("program") + "\"tables\":{\"lower\":[\"bside-table 1\",\"# " + std::string (300, 'x') + "\"]}}", 0.5, 8u << 20 });
    files.push_back ({ "a table that is only comments", header ("program") + "\"tables\":{\"lower\":[\"# no header\"]}}", 0.5, 8u << 20 });
    // The reader's worst input for its own cost: 1024 lines of a long pathological row.
    {
        std::string s = "\"bside-table 1\",\"[wave1]\"";
        for (int i = 0; i < 1022; ++i)
            s += ",\"    gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on\"";
        files.push_back ({ "1024 rows of 13 commands", header ("program") + "\"tables\":{\"lower\":[" + s + "]}}", 1.0, 16u << 20 });
    }

    for (const auto& f : files)
    {
        INFO (f.what);
        const auto file = rig.temp / "hostile.bsprog";
        REQUIRE (file.replaceWithData (f.text.data(), f.text.size()));
        const bool isBank = f.text.find ("\"kind\":\"bank\"") != std::string::npos;
        juce::String error;
        ByteCounter counter;
        const auto seconds = secondsOf ([&] { error = isBank ? pm.loadBankFile (file) : pm.loadProgramFile (file); });
        CHECK (error.isNotEmpty());
        CHECK (seconds < f.maxSeconds);
        if (ByteCounter::available)
        {
            INFO ("allocated " << counter.bytes() << " bytes");
            CHECK (counter.bytes() < f.maxBytes);
        }
    }
    // After all of that: nothing changed.
    const auto after = pm.captureProgram();
    CHECK (after.values == before.values);
    CHECK (after.data == before.data);
    CHECK (rig.compiled (0) == prog::emptyProgram());
}

TEST_CASE ("B-Side tables: the session carries them verbatim and restores them without file I/O", "[bside][tables][state]")
{
    Rig a, b;
    a.presets().applyProgram (programWith (a.schema(), kTableA, kTableB), 2, false);
    a.set ("key.split_point", 33.0f);
    const auto xml = a.stateXml();
    REQUIRE (xml != nullptr);
    const auto* tables = xml->getChildByName ("Tables");
    REQUIRE (tables != nullptr);
    CHECK (tables->getStringAttribute ("lower").toStdString() == kTableA);
    CHECK (tables->getStringAttribute ("upper").toStdString() == kTableB);
    CHECK (xml->getNumChildElements() > 0);
    int tablesChildren = 0;
    for (auto* c : xml->getChildIterator())
        tablesChildren += c->hasTagName ("Tables") ? 1 : 0;
    CHECK (tablesChildren == 1);

    // Restore into another instance: tables compiled and published, state saved again is the same bytes.
    const auto mb = a.state();
    b.proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
    CHECK (b.compiled (0) == compileText (kTableA));
    CHECK (b.compiled (1) == compileText (kTableB));
    CHECK (b.presets().currentTable (0) == linesOf (kTableA));
    CHECK (b.presets().tableError (0).isEmpty());
    const auto again = b.state();
    CHECK (again.getSize() == mb.getSize());
    CHECK (std::memcmp (again.getData(), mb.getData(), mb.getSize()) == 0);

    // A state without tables (older session) restores to no tables, no error.
    auto stripped = a.stateXml();
    while (auto* c = stripped->getChildByName ("Tables"))
        stripped->removeChildElement (c, true);
    b.restore (*stripped);
    CHECK (b.compiled (0) == prog::emptyProgram());
    CHECK (b.presets().currentTable (0).empty());
    CHECK (b.presets().tableError (0).isEmpty());
    const auto stateXmlAfter = b.stateXml();
    CHECK (stateXmlAfter->getChildByName ("Tables")->getStringAttribute ("lower").isEmpty());
}

TEST_CASE ("B-Side tables: a malformed table in a session gives that part an empty table and an error", "[bside][tables][state][security]")
{
    Rig a, b;
    a.presets().applyProgram (programWith (a.schema(), kTableA, kTableB), 0, false);
    b.presets().applyProgram (programWith (b.schema(), kTableC, kTableC), 0, false);

    auto xml = a.stateXml();
    auto* tables = xml->getChildByName ("Tables");
    REQUIRE (tables != nullptr);
    tables->setAttribute ("lower", juce::String ("bside-table 1\n[wave1]\nfrobnicate\n"));
    b.restore (*xml);
    CHECK (b.compiled (0) == prog::emptyProgram()); // the bad part: empty
    CHECK (b.presets().currentTable (0).empty());
    CHECK (b.presets().tableError (0).startsWith ("The session's lower table: line 3, column 1"));
    CHECK (b.compiled (1) == compileText (kTableB));  // the good part still arrives
    CHECK (b.presets().tableError (1).isEmpty());
    // The parameters of the session were restored all the same.
    CHECK (b.get ("key.split_point") == Catch::Approx (a.get ("key.split_point")));
    // A state saved now has no bad table in it, and loads without an error.
    const auto saved = b.state();
    Rig c;
    c.proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    CHECK (c.presets().tableError (0).isEmpty());
    CHECK (c.compiled (1) == compileText (kTableB));
    // Applying a program with a good table clears the error.
    b.presets().applyProgram (programWith (b.schema(), kTableA, kTableA), 0, false);
    CHECK (b.presets().tableError (0).isEmpty());

    // Hostile properties: wrong types, huge text, too many lines, control characters, invalid UTF-8
    // (XML cannot carry NUL or most controls, so those come as other characters), non-canonical but valid text.
    const std::vector<std::pair<const char*, juce::String>> bad {
        { "empty header", "[wave1]\n" },
        { "too many lines", juce::String::repeatedString ("# c\n", 1025) },
        { "too many bytes", "bside-table 1\n" + juce::String::repeatedString (juce::String::repeatedString ("#", 199) + "\n", 170) },
        { "a line over 200 bytes", "bside-table 1\n# " + juce::String::repeatedString ("x", 300) + "\n" },
        { "a carriage return", "bside-table 1\r\n[wave1]\r\n" },
        { "control character", juce::String::fromUTF8 ("bside-table 1\n# a\x01\n") },
        { "C1 control", juce::String::fromUTF8 ("bside-table 1\n# a\xc2\x85\n") },
        // Kept small on purpose: JUCE's own XML decoder (not ours, and run by the host's blob handling before
        // our code) takes 9 s for 1 000 000 escaped line ends; what is measured below is our part only.
        { "50 000 empty lines", juce::String::repeatedString ("\n", 50000) },
        { "400 KB of text", juce::String::repeatedString ("0123456789012345678901234567890123456789\n", 10000) },
    };
    for (const auto& [what, text] : bad)
    {
        INFO (what);
        auto hostile = a.stateXml();
        hostile->getChildByName ("Tables")->setAttribute ("upper", text);
        juce::MemoryBlock block;
        const auto encode = secondsOf ([&] { juce::AudioProcessor::copyXmlToBinary (*hostile, block); });
        std::unique_ptr<juce::XmlElement> parsed;
        const auto decode = secondsOf ([&] { parsed = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize())); });
        juce::ValueTree tree;
        const auto toTree = secondsOf ([&] { tree = juce::ValueTree::fromXml (*parsed); });
        (void) toTree;
        const auto seconds = secondsOf ([&] { b.proc.setStateInformation (block.getData(), static_cast<int> (block.getSize())); });
        std::printf ("  %s: XML decode %.3f s + ours %.3f s (%zu bytes of state; encode %.3f s)\n", what, decode, seconds - decode, block.getSize(), encode);
        CHECK (seconds - decode < 0.5); // our part: everything after the host's XML decoding
        CHECK (b.compiled (1) == prog::emptyProgram());
        CHECK (b.presets().tableError (1).isNotEmpty());
        CHECK (b.compiled (0) == compileText (kTableA));
    }
    // (A property that is not text cannot come through XML, which turns every attribute into text; that
    // branch is reached through a ValueTree in tests/preset_unit/PresetTableManagerTests.cpp.)
}

TEST_CASE ("B-Side tables: duplicate Tables and Preset children of a hostile session are all removed", "[bside][tables][state][security]")
{
    Rig a, b;
    a.presets().applyProgram (programWith (a.schema(), kTableA, kTableB), 2, false);
    auto xml = a.stateXml();
    const auto* tables = xml->getChildByName ("Tables");
    const auto* info = xml->getChildByName ("Preset");
    REQUIRE (tables != nullptr);
    REQUIRE (info != nullptr);

    // The first of each is used; every later one is dropped, so a saved state never carries them on.
    for (int i = 0; i < 2000; ++i)
    {
        auto* extraTables = new juce::XmlElement ("Tables");
        extraTables->setAttribute ("lower", juce::String ("bside-table 1\n[wave1]\npitch +") + juce::String (i % 90) + "\n");
        extraTables->setAttribute ("upper", "junk");
        xml->addChildElement (extraTables);
        auto* extraInfo = new juce::XmlElement ("Preset");
        extraInfo->setAttribute ("name", "Impostor " + juce::String (i));
        xml->addChildElement (extraInfo);
    }
    // Duplicates also before the original (the document's first Tables child wins).
    const auto seconds = secondsOf ([&] { b.restore (*xml); });
    CHECK (seconds < 1.0);
    CHECK (b.compiled (0) == compileText (kTableA));
    CHECK (b.compiled (1) == compileText (kTableB));
    CHECK (b.presets().currentName() == "Tabby"); // the first Preset child: the real one of `a`
    const auto saved = b.stateXml();
    int tablesChildren = 0, infoChildren = 0;
    for (auto* c : saved->getChildIterator())
    {
        tablesChildren += c->hasTagName ("Tables") ? 1 : 0;
        infoChildren += c->hasTagName ("Preset") ? 1 : 0;
    }
    CHECK (tablesChildren == 1);
    CHECK (infoChildren == 1);

    // When the first one is the hostile one, it is the one used (garbage in, an error out, no crash).
    auto hostile = a.stateXml();
    auto* first = new juce::XmlElement ("Tables");
    first->setAttribute ("lower", "nonsense");
    hostile->prependChildElement (first);
    b.restore (*hostile);
    CHECK (b.presets().currentTable (0).empty());
    CHECK (b.presets().tableError (0).isNotEmpty());
    CHECK (b.compiled (1) == prog::emptyProgram()); // its missing "upper" means no table
}

TEST_CASE ("B-Side tables: parameters and tables of an apply are one consistent set for the audio thread", "[bside][tables][threads][handover]")
{
    // The reader does what a block does (presets/ApplyGate.h, program/ProgramHandover.h): read the
    // parameters and the table inside one seqlock window, and use them only if the window held. The
    // writer applies programs whose split point and table carry the same number. A block that used
    // a new table with an old split point (or the reverse) is the failure.
    Rig rig;
    auto& pm = rig.presets();
    auto& gate = pm.getGate();
    auto* splitPoint = rig.proc.getState().getRawParameterValue ("key.split_point");
    auto& handover = rig.proc.tableHandover (0);

    auto tableFor = [] (int tag) { return std::string ("bside-table 1\n[pulse1]\n    set ") + std::to_string (tag * 20) + "\n"; };
    std::vector<Program> programs;
    for (int tag = 1; tag <= 100; ++tag)
    {
        auto p = programWith (rig.schema(), tableFor (tag), tableFor (101 - tag), "P" + juce::String (tag));
        p.values[static_cast<size_t> (rig.schema().indexOf ("key.split_point"))] = static_cast<float> (tag);
        programs.push_back (std::move (p));
    }

    std::atomic<bool> stop { false };
    std::atomic<long long> used { 0 }, held { 0 }, inconsistent { 0 }, failedClaims { 0 }, changes { 0 };
    std::thread audio ([&] {
        int lastTag = -1;
        while (! stop.load (std::memory_order_relaxed))
        {
            gate.blockStarted();
            const auto token = gate.beginRead();
            if ((token & 1u) != 0u)
            {
                ++held;
                continue;
            }
            const float split = splitPoint->load (std::memory_order_relaxed);
            const prog::Program* p = handover.beginAcquire();
            if (p == nullptr)
            {
                ++failedClaims;
                continue;
            }
            if (! gate.endRead (token))
            {
                handover.abort();
                ++held;
                continue;
            }
            handover.commit();
            ++used;
            // The set this block uses: the parameter it copied and the table it holds for the whole block.
            const prog::Program& table = *p;
            if (table.numCmds == 0) // the initial empty table, before the first apply
                continue;
            const int tag = static_cast<int> (table.cmds[0].a) / 20;
            std::this_thread::yield(); // the "block" goes on reading the table while the writer works
            const bool whole = table.cmds[0].op == static_cast<std::uint8_t> (prog::Op::pulseSet) && table.numCmds == 1
                               && static_cast<int> (table.cmds[0].a) == tag * 20;
            if (! whole || tag < 1 || tag > 100 || static_cast<int> (split + 0.5f) != tag)
                ++inconsistent;
            if (tag != lastTag)
            {
                ++changes;
                lastTag = tag;
            }
        }
    });

    const auto start = std::chrono::steady_clock::now();
    int applied = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds (2000))
    {
        pm.applyProgram (programs[static_cast<size_t> (applied % 100)], 5, false, false);
        ++applied;
        if ((applied & 7) == 0)
            std::this_thread::yield();
    }
    stop = true;
    audio.join();
    INFO ("applied " << applied << ", blocks using a set " << used.load() << ", held " << held.load() << ", failed claims " << failedClaims.load()
                     << ", table changes seen " << changes.load());
    CHECK (inconsistent.load() == 0);
    CHECK (applied > 500);
    CHECK (used.load() > 1000);
    CHECK (changes.load() > 100);
    CHECK (gate.holdDepth() == 0);
}

TEST_CASE ("B-Side tables: two parts in one seqlock window commit both tables or neither", "[bside][tables][threads][handover]")
{
    // The protocol of program/ProgramHandover.h for two parts: claim both, then decide once. Each program
    // has the same tag in the split point, the Lower table and the Upper table (the Upper one mirrored), so a
    // block that kept one part's old table, or mixed in an old parameter, is seen.
    Rig rig;
    auto& pm = rig.presets();
    auto& gate = pm.getGate();
    auto* splitPoint = rig.proc.getState().getRawParameterValue ("key.split_point");
    auto& lower = rig.proc.tableHandover (0);
    auto& upper = rig.proc.tableHandover (1);

    auto tableFor = [] (int tag) { return std::string ("bside-table 1\n[pulse1]\n    set ") + std::to_string (tag * 20) + "\n"; };
    std::vector<Program> programs;
    for (int tag = 1; tag <= 100; ++tag)
    {
        auto p = programWith (rig.schema(), tableFor (tag), tableFor (101 - tag), "P" + juce::String (tag));
        p.values[static_cast<size_t> (rig.schema().indexOf ("key.split_point"))] = static_cast<float> (tag);
        programs.push_back (std::move (p));
    }

    std::atomic<bool> stop { false };
    std::atomic<long long> used { 0 }, aborted { 0 }, inconsistent { 0 }, bothClaimed { 0 };
    std::thread audio ([&] {
        while (! stop.load (std::memory_order_relaxed))
        {
            gate.blockStarted();
            const auto token = gate.beginRead();
            if ((token & 1u) != 0u)
                continue;
            const float split = splitPoint->load (std::memory_order_relaxed);
            const prog::Program* l = lower.beginAcquire();
            const prog::Program* u = upper.beginAcquire();
            const bool ok = l != nullptr && u != nullptr && gate.endRead (token);
            if (ok)
            {
                lower.commit();
                upper.commit();
            }
            else
            {
                if (l != nullptr)
                    lower.abort();
                if (u != nullptr)
                    upper.abort();
                ++aborted;
                continue;
            }
            ++used;
            ++bothClaimed;
            std::this_thread::yield();
            const prog::Program& lt = lower.inUse();
            const prog::Program& ut = upper.inUse();
            if (lt.numCmds == 0 && ut.numCmds == 0) // before the first apply
                continue;
            const int tagL = static_cast<int> (lt.cmds[0].a) / 20, tagU = static_cast<int> (ut.cmds[0].a) / 20;
            if (lt.numCmds != 1 || ut.numCmds != 1 || tagL < 1 || tagL > 100 || tagU != 101 - tagL || static_cast<int> (split + 0.5f) != tagL)
                ++inconsistent;
        }
    });
    const auto start = std::chrono::steady_clock::now();
    int applied = 0;
    // At least 1.5 s; then on until the abort path has run at least once (on a loaded machine or under a
    // sanitizer the two threads may not collide within 1.5 s), 20 s at most.
    const auto elapsed = [&] { return std::chrono::steady_clock::now() - start; };
    while (elapsed() < std::chrono::milliseconds (1500) || (aborted.load() == 0 && elapsed() < std::chrono::seconds (20)))
    {
        pm.applyProgram (programs[static_cast<size_t> (applied % 100)], 5, false, false);
        ++applied;
        if ((applied & 7) == 0)
            std::this_thread::yield();
    }
    stop = true;
    audio.join();
    INFO ("applied " << applied << ", blocks using a set " << used.load() << ", aborted " << aborted.load());
    CHECK (inconsistent.load() == 0);
    CHECK (applied > 500);
    CHECK (used.load() > 1000);
    CHECK (aborted.load() > 0); // the abort path of both parts really ran
}

TEST_CASE ("B-Side tables: applying programs and restoring states with tables while the audio thread renders", "[bside][tables][threads][realtime]")
{
    Rig rig;
    rig.proc.setNonRealtime (false);
    rig.proc.setPlayConfigDetails (0, 2, 48000.0, 256);
    rig.proc.prepareToPlay (48000.0, 256);
    rig.presets().getGate().setTimeouts (250, 40);

    std::atomic<bool> stop { false };
    std::atomic<bool> finite { true };
    std::atomic<long long> blocks { 0 };
    std::thread audio ([&] {
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        while (! stop.load())
        {
            buffer.clear();
            rig.proc.processBlock (buffer, midi);
            midi.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    if (! std::isfinite (buffer.getSample (ch, i)))
                        finite = false;
            ++blocks;
        }
    });
    const auto start = std::chrono::steady_clock::now();
    int n = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds (1500))
    {
        rig.presets().applyProgram (programWith (rig.schema(), (n & 1) != 0 ? kTableA : kTableB, (n & 2) != 0 ? kTableC : kTableA), 1, false);
        const auto mb = rig.state();
        rig.proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
        ++n;
    }
    stop = true;
    audio.join();
    CHECK (finite.load());
    CHECK (blocks.load() > 10);
    CHECK (n > 5);
}
