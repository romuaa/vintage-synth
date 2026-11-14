#include "vintage/ui/VintageEditor.h"

#include "UiData.h"
#include "vintage/ui/PlaceholderPage.h"
#include "vintage/ui/SoundPage.h"

namespace vs
{
juce::AudioProcessorEditor* VintageSynthProcessor::createEditor()
{
    return new ui::VintageEditor (*this);
}
} // namespace vs

namespace vs::ui
{
namespace
{
using namespace metrics;

std::vector<EditorFrame::Page> vintagePages()
{
    return {
        { kVintagePageNames[0], "Sound", PageIcon::Sound, true, "Sound: the oscillators, the filter and the level of the edited part. Drag the dots on the curve." },
        { kVintagePageNames[1], "Envelopes", PageIcon::Envelopes, true, "Envelopes: how the filter, pitch and loudness move on each note (placeholder until M7b)." },
        { kVintagePageNames[2], "LFO", PageIcon::Lfo, true, "LFO: the slow wobble and what it moves (placeholder until M7b)." },
        { kVintagePageNames[3], "Play", PageIcon::Play, false, "Play: keyboard layout, voices, glide and the mix of both parts (placeholder until M7c)." },
        { kVintagePageNames[4], "Arp + Seq", PageIcon::ArpSeq, false, "Arp + Seq: the arpeggiator and the step sequencer (placeholder until M7d)." },
        { kVintagePageNames[5], "Effects", PageIcon::Effects, false, "Effects: chorus, delay and reverb on both parts (placeholder until M7c)." },
        { kVintagePageNames[6], "Browser", PageIcon::Browser, false, "Browser: programs and banks (until M7e: the preset bar at the top)." },
    };
}

/** The parameter's name without the part prefix ("Lower VCF Cutoff" -> "VCF Cutoff"). */
juce::String shortName (juce::RangedAudioParameter& p)
{
    auto n = p.getName (64);
    for (const char* pre : { "Lower ", "Upper " })
        if (n.startsWith (pre))
            return n.substring (6);
    return n;
}
} // namespace

FontData VintageFontSource::fontData()
{
    return { vs_ui::InterRegular_ttf, static_cast<std::size_t> (vs_ui::InterRegular_ttfSize), vs_ui::InterSemiBold_ttf,
             static_cast<std::size_t> (vs_ui::InterSemiBold_ttfSize) };
}

VintageEditor::VintageEditor (VintageSynthProcessor& p)
    : juce::AudioProcessorEditor (p),
      editorState (p.getEditorState()),
      feed (p.getUiFeed()),
      theme (fonts->regular, fonts->semibold),
      help (HelpTexts::parse (std::string_view (vs_ui::helptexts_md, static_cast<size_t> (vs_ui::helptexts_mdSize)))),
      mirror (p),
      ctx { mirror, theme, registry, help },
      bar (p.getPresets()),
      frame (ctx, vintagePages(), bar),
      vblank (this, [this] { onVBlank(); })
{
    setLookAndFeel (&theme);
    setOpaque (true);
    addAndMakeVisible (frame);
    buildPages();

    frame.onPageChosen = [this] (int page) { selectPage (page, frame.getPart()); };
    frame.onPartChosen = [this] (int part) { selectPage (frame.getPage(), part); };
    frame.onSizeMenu = [this] { showSizeMenu(); };

    keyModeIndex = mirror.indexOf ("key.mode");
    if (keyModeIndex >= 0)
        mirror.subscribe (keyModeIndex, this);

    syncFromEditorState();
    lastRate = feed.internalRate.load (std::memory_order_relaxed);
    for (auto& s : sound)
        s->setSampleRate (lastRate);

    // Size (9.2.4): fixed aspect, 75-200 %, corner resizer and the Size menu.
    setResizable (true, true);
    setResizeLimits (kDesignWidth * 75 / 100, kDesignHeight * 75 / 100, kDesignWidth * 2, kDesignHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (static_cast<double> (kDesignWidth) / static_cast<double> (kDesignHeight));
    juce::Rectangle<int> userArea;
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        userArea = display->userBounds.toNearestInt();
    const int percent = initialSizePercent (editorState.sizePercent.load (std::memory_order_relaxed), userArea);
    appliedSize = editorState.sizePercent.load (std::memory_order_relaxed); // 0: chosen here, not stored
    lastSetSize = { kDesignWidth * percent / 100, kDesignHeight * percent / 100 };
    setSize (lastSetSize.getWidth(), lastSetSize.getHeight());
    constructed = true;

    setWantsKeyboardFocus (true);
    startTimerHz (30); // fallback driver when no vblank arrives (9.1.5)
    tick();
}

VintageEditor::~VintageEditor()
{
    // Everything stops before the members go (9.1.2 rule 5): the tick drivers, open gestures (the
    // pages cancel their controls' gestures, the mirror ends anything left), popovers.
    stopTimer();
    frame.closeOutputPopover();
    for (auto& s : sound)
        s->cancelInteraction();
    for (auto& ph : placeholders)
        ph->cancelInteraction();
    frame.cancelInteraction();
    mirror.unsubscribe (this);
    mirror.endAllGestures();
    setLookAndFeel (nullptr);
}

void VintageEditor::buildPages()
{
    const auto prefix = [] (int part) { return juce::String (part == 0 ? kLowerPrefix : kUpperPrefix); };
    const auto partItems = [&] (const std::vector<juce::String>& suffixes, int part) {
        std::vector<PlaceholderPage::Item> items;
        for (const auto& s : suffixes)
            items.push_back ({ prefix (part) + s, shortName (mirror.parameter (mirror.indexOf (prefix (part) + s))), part });
        return items;
    };
    const auto addPlaceholder = [&] (VintagePage page, int part, const juce::String& title, const juce::String& note,
                                     std::vector<PlaceholderPage::Item> items) {
        registry.setContext (kVintagePageNames[static_cast<size_t> (page)], part);
        placeholders.push_back (std::make_unique<PlaceholderPage> (ctx, title, note, std::move (items)));
        placeholderRefs.push_back ({ page, part, placeholders.back().get() });
        frame.addPageComponent (static_cast<int> (page), part, *placeholders.back());
    };

    for (int part = 0; part < 2; ++part)
    {
        registry.setContext (kVintagePageNames[0], part);
        sound[part] = std::make_unique<SoundPage> (ctx, part);
        sound[part]->onOpenEffects = [this] { selectPage (static_cast<int> (VintagePage::Effects), frame.getPart()); };
        frame.addPageComponent (0, part, *sound[part]);

        const auto partName = juce::String (part == 0 ? "Lower" : "Upper");
        addPlaceholder (VintagePage::Envelopes, part, "Envelopes (" + partName + ")",
                        "Placeholder until M7b: every envelope parameter of this part, with plain sliders.", partItems (envelopeSuffixes(), part));
        addPlaceholder (VintagePage::Lfo, part, "LFO (" + partName + ")", "Placeholder until M7b: every LFO and modulation parameter of this part.",
                        partItems (lfoSuffixes(), part));
    }

    // Global pages: one component each (-1).
    std::vector<PlaceholderPage::Item> play;
    for (const char* id : { "key.mode", "key.split_point", "mix.balance", "global.vintage" })
        play.push_back ({ id, shortName (mirror.parameter (mirror.indexOf (id))), -1 });
    for (int part = 0; part < 2; ++part)
        for (auto& item : partItems (playPartSuffixes(), part))
        {
            item.label = (part == 0 ? "Lower " : "Upper ") + item.label;
            play.push_back (item);
        }
    addPlaceholder (VintagePage::Play, -1, "Play", "Placeholder until M7c: keyboard layout, character and both parts side by side.", std::move (play));

    std::vector<PlaceholderPage::Item> arp, fx;
    for (int i = 0; i < mirror.size(); ++i)
    {
        auto& p = mirror.parameter (i);
        if (p.paramID.startsWith ("arp.") || p.paramID.startsWith ("seq."))
            arp.push_back ({ p.paramID, shortName (p), -1 });
        else if (p.paramID.startsWith ("fx."))
            fx.push_back ({ p.paramID, shortName (p), -1 });
    }
    addPlaceholder (VintagePage::ArpSeq, -1, "Arp + Seq", "Placeholder until M7d: the arpeggiator, the step sequencer and its 16 steps.", std::move (arp));
    addPlaceholder (VintagePage::Effects, -1, "Effects", "Placeholder until M7c: chorus, delay and reverb.", std::move (fx));
    addPlaceholder (VintagePage::Browser, -1, "Browser",
                    "Placeholder until M7e: load, save and initialise programs with the preset bar at the top of the window.", {});
    registry.setContext ("frame", -1);
}

PlaceholderPage& VintageEditor::getPlaceholder (VintagePage page, int part)
{
    for (const auto& r : placeholderRefs)
        if (r.page == page && (r.part == part || r.part < 0))
            return *r.component;
    jassertfalse;
    return *placeholderRefs.front().component;
}

void VintageEditor::selectPage (int page, int part)
{
    editorState.page.store (page, std::memory_order_relaxed);
    editorState.part.store (part == 1 ? 1 : 0, std::memory_order_relaxed);
    syncFromEditorState();
}

void VintageEditor::syncFromEditorState()
{
    // The editor writes EditorState on every change and reads it back here (and on every tick), so
    // a session restore on any thread shows up at the next tick.
    const int page = juce::jlimit (0, static_cast<int> (kVintagePageNames.size()) - 1, editorState.page.load (std::memory_order_relaxed));
    const int part = editorState.part.load (std::memory_order_relaxed) == 1 ? 1 : 0;
    // A session restored while the editor is open brings its window size too.
    const int size = editorState.sizePercent.load (std::memory_order_relaxed);
    if (constructed && size != appliedSize && size >= shell::EditorState::kMinSizePercent && size <= shell::EditorState::kMaxSizePercent)
        setSizePercent (size);
    if (page != frame.getPage() || part != frame.getPart() || frame.currentPageComponent() == nullptr || ! frame.currentPageComponent()->isVisible())
    {
        frame.showPage (page, part);
        updateBanner();
    }
}

void VintageEditor::updateBanner()
{
    const bool whole = keyModeIndex >= 0 && model::stepIndex (mirror.value (keyModeIndex), 3) == 0;
    frame.setBanner (frame.getPart() == 1 && whole
                         ? juce::String::fromUTF8 ("Upper is not heard in Whole mode \xe2\x80\x94 switch to Dual or Split on the Play page")
                         : juce::String());
}

void VintageEditor::mirrorChanged (int index, bool)
{
    if (index == keyModeIndex)
        updateBanner();
}

void VintageEditor::tick (double nowMs)
{
    lastTickMs = nowMs;
    mirror.tick (nowMs);
    syncFromEditorState();

    const float rate = feed.internalRate.load (std::memory_order_relaxed);
    if (std::abs (rate - lastRate) > 0.5f)
    {
        lastRate = rate;
        for (auto& s : sound)
            s->setSampleRate (rate);
    }
    if (nowMs - lastMeterMs >= kMeterMs || nowMs < lastMeterMs) // (a test's clock may start below the real one)
    {
        lastMeterMs = nowMs;
        frame.updateMeters (feed.takePeak (0), feed.takePeak (1), feed.takeLimiterGainMin(), nowMs);
    }
}

void VintageEditor::onVBlank()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    lastVBlankMs = now;
    if (now - lastTickMs >= kMinTickMs)
        tick (now);
}

void VintageEditor::timerCallback()
{
    // Hosts and window systems may throttle vblank: then a 30 Hz timer drives the tick.
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (isShowing() && now - lastVBlankMs > kVBlankTimeoutMs && now - lastTickMs >= kMinTickMs)
        tick (now);
}

int VintageEditor::initialSizePercent (int saved, juce::Rectangle<int> userArea)
{
    if (saved >= shell::EditorState::kMinSizePercent && saved <= shell::EditorState::kMaxSizePercent)
        return saved;
    if (userArea.isEmpty())
        return 100;
    const auto fits = [&] (int percent) {
        return kDesignWidth * percent / 100 <= userArea.getWidth() * 9 / 10 && kDesignHeight * percent / 100 <= userArea.getHeight() * 9 / 10;
    };
    if (fits (100))
        return 100;
    for (int i = static_cast<int> (std::size (kMenuSizes)) - 1; i >= 0; --i)
        if (kMenuSizes[i] < 100 && fits (kMenuSizes[i]))
            return kMenuSizes[i];
    return kMenuSizes[0];
}

void VintageEditor::setSizePercent (int percent)
{
    percent = juce::jlimit (shell::EditorState::kMinSizePercent, shell::EditorState::kMaxSizePercent, percent);
    editorState.sizePercent.store (percent, std::memory_order_relaxed);
    appliedSize = percent;
    lastSetSize = { kDesignWidth * percent / 100, kDesignHeight * percent / 100 };
    setSize (lastSetSize.getWidth(), lastSetSize.getHeight());
}

void VintageEditor::showSizeMenu()
{
    juce::PopupMenu menu;
    const int current = juce::roundToInt (scale * 100.0f);
    for (int s : kMenuSizes)
        menu.addItem (s, juce::String (s) + " %", true, s == current);
    juce::Component::SafePointer<VintageEditor> self (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&frame.getRail()), [self] (int result) {
        if (self != nullptr && result > 0)
            self->setSizePercent (result);
    });
}

void VintageEditor::resized()
{
    scale = static_cast<float> (getWidth()) / static_cast<float> (kDesignWidth);
    frame.setTransform (juce::AffineTransform::scale (scale));
    frame.setBounds (0, 0, kDesignWidth, kDesignHeight);
    // A size set by the user (corner or host) is UI state; the one this editor chose is not stored.
    if (constructed && getLocalBounds() != lastSetSize.withPosition (0, 0))
    {
        const int percent = juce::jlimit (shell::EditorState::kMinSizePercent, shell::EditorState::kMaxSizePercent, juce::roundToInt (scale * 100.0f));
        editorState.sizePercent.store (percent, std::memory_order_relaxed);
        appliedSize = percent;
        lastSetSize = getLocalBounds();
    }
}

void VintageEditor::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

bool VintageEditor::keyPressed (const juce::KeyPress& k)
{
    // Ctrl/Cmd + 1 ... 7 select the pages (9.2.2).
    const int c = k.getKeyCode();
    if (k.getModifiers().isCommandDown() && c >= '1' && c <= '7')
    {
        selectPage (c - '1', frame.getPart());
        return true;
    }
    if (c == juce::KeyPress::escapeKey && frame.isOutputPopoverOpen())
    {
        frame.closeOutputPopover();
        return true;
    }
    return false;
}
} // namespace vs::ui
