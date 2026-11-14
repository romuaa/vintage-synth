// Help texts (docs/architecture.md 9.3.8, 9.6): the parser rules, and the real file: every one of
// the 172 parameter IDs resolves to exactly one text, no entry names an unknown ID, every text is
// at most two sentences and 170 characters, and the plugin embeds exactly the file in docs/ui/.

#include <catch2/catch_test_macros.hpp>

#include "../integration/IntegrationRig.h"
#include "UiData.h"
#include "common/ui/HelpTexts.h"

#include <fstream>
#include <set>
#include <sstream>

using vs::ui::HelpTexts;

namespace
{
std::string embedded() { return { vs_ui::helptexts_md, static_cast<size_t> (vs_ui::helptexts_mdSize) }; }
} // namespace

TEST_CASE ("Help texts: parser rules (entry lines, <part>. and NN expansion, everything else ignored)", "[ui][help]")
{
    const auto h = HelpTexts::parse ("# Title\n"
                                     "Some prose with - `not.an.entry`: inside.\n"
                                     "    - `indented.code`: ignored\n"
                                     "- `master.volume`: The overall volume.\n"
                                     "- `<part>.vcf.cutoff`: Brightness.\r\n"
                                     "- `seq.stepNN.note`: Pitch of step NN.\n"
                                     "- `broken entry without colon`\n"
                                     "- `<part>.` stands for `lower.`: prose, not an entry\n"
                                     "- `ui.size`: Window size.");
    CHECK (h.countOf ("master.volume") == 1);
    REQUIRE (h.find ("lower.vcf.cutoff") != nullptr);
    CHECK (*h.find ("lower.vcf.cutoff") == "Brightness."); // CR stripped
    CHECK (*h.find ("upper.vcf.cutoff") == "Brightness.");
    CHECK (h.find ("<part>.vcf.cutoff") == nullptr);
    CHECK (*h.find ("seq.step01.note") == "Pitch of step 1.");
    CHECK (*h.find ("seq.step16.note") == "Pitch of step 16.");
    CHECK (h.find ("seq.step17.note") == nullptr);
    CHECK (h.find ("not.an.entry") == nullptr);
    CHECK (h.find ("indented.code") == nullptr);
    CHECK (*h.find ("ui.size") == "Window size."); // last line without a newline
    CHECK (h.entries().size() == 1 + 2 + 16 + 1);

    CHECK (HelpTexts::sentenceCount ("One. Two.") == 2);
    CHECK (HelpTexts::sentenceCount ("About 2.5 Hz here.") == 1);
    CHECK (HelpTexts::sentenceCount ("Is it? Yes!") == 2);
    CHECK (HelpTexts::characterCount ("8' \xe2\x86\x92 1") == 6); // UTF-8 code points: 8 ' space arrow space 1
}

TEST_CASE ("Help texts: the plugin embeds docs/ui/help-texts.md unchanged", "[ui][help]")
{
    std::ifstream in (VS_HELP_TEXTS_FILE, std::ios::binary);
    REQUIRE (in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK (ss.str() == embedded());
}

TEST_CASE ("Help texts: every parameter has exactly one text; every entry names a parameter or a ui.* control; short texts", "[ui][help]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    const auto help = HelpTexts::parse (embedded());

    std::set<std::string> ids;
    int count = 0;
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            ++count;
            const auto id = r->paramID.toStdString();
            ids.insert (id);
            INFO (id);
            CHECK (help.countOf (id) == 1);
        }
    CHECK (count == 172);

    for (const auto& e : help.entries())
    {
        INFO (e.id << " (line " << e.line << "): " << e.text);
        CHECK ((ids.count (e.id) == 1 || e.id.rfind ("ui.", 0) == 0));
        CHECK (HelpTexts::sentenceCount (e.text) >= 1);
        CHECK (HelpTexts::sentenceCount (e.text) <= 2);
        CHECK (HelpTexts::characterCount (e.text) <= 170);
        CHECK (help.countOf (e.id) == 1);
    }
    // The interface items the editor uses exist.
    for (const char* id : { "ui.part", "ui.page", "ui.preset", "ui.output", "ui.size" })
        CHECK (help.countOf (id) == 1);
}
