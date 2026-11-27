#include "vintage/ui/VintageEditor.h"

#include "UiData.h"
#include "vintage/ui/ArpSeqPage.h"
#include "vintage/ui/EffectsPage.h"
#include "vintage/ui/EnvelopesPage.h"
#include "vintage/ui/LfoPage.h"
#include "vintage/ui/BrowserPage.h"
#include "vintage/ui/PlayPage.h"
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
        { kVintagePageNames[1], "Envelopes", PageIcon::Envelopes, true, "Envelopes: how the filter, pitch and loudness move on each note. Drag the dots on the shapes." },
        { kVintagePageNames[2], "LFO", PageIcon::Lfo, true, "LFO: the slow wobble and what it moves. Drag the dots to set the delay and the speed." },
        { kVintagePageNames[3], "Play", PageIcon::Play, false, "Play: how the two parts share the keyboard, and how each part is played. Press the keys to play; drag the split marker to move it." },
        { kVintagePageNames[4], "Arp + Seq", PageIcon::ArpSeq, false, "Arp + Seq: pick a pattern or paint steps; the picture shows what the arpeggiator and the sequencer will play." },
        { kVintagePageNames[5], "Effects", PageIcon::Effects, false, "Effects: chorus, delay and reverb on both parts, in signal order. Drag the dots on the pictures." },
        { kVintagePageNames[6], "Browser", PageIcon::Browser, false, "Browser: pick a program from the grid, load it or just one of its sounds, and save your own." },
    };
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
      frame (ctx, vintagePages(), bar, ProductName::make (p.getName(), VintageSynthProcessor::kVersion)),
      vblank (this, [this] { onVBlank(); })
{
    setLookAndFeel (&theme);
    setOpaque (true);
    addAndMakeVisible (frame);
    buildPages();

    frame.onPageChosen = [this] (int page) { selectPage (page, frame.getPart()); };
    frame.onPartChosen = [this] (int part) { selectPage (frame.getPage(), part); };
    frame.onSizeMenu = [this] { showSizeMenu(); };
    bar.smartSave = true;         // Save: in place for a user bank, else Save as (9.3.7)
    bar.setCompactLayout (true);  // the program name gets the room
    bar.onOpenBrowser = [this] { selectPage (static_cast<int> (VintagePage::Browser), frame.getPart()); };

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
    for (auto& e : envelopes)
        e->cancelInteraction();
    for (auto& l : lfo)
        l->cancelInteraction();
    playPage->cancelInteraction();
    effectsPage->cancelInteraction();
    if (arpSeq != nullptr)
        arpSeq->cancelInteraction();
    frame.cancelInteraction();
    mirror.unsubscribe (this);
    mirror.endAllGestures();
    setLookAndFeel (nullptr);
}

void VintageEditor::buildPages()
{
    for (int part = 0; part < 2; ++part)
    {
        registry.setContext (kVintagePageNames[0], part);
        sound[part] = std::make_unique<SoundPage> (ctx, part);
        sound[part]->onOpenEffects = [this] { selectPage (static_cast<int> (VintagePage::Effects), frame.getPart()); };
        frame.addPageComponent (0, part, *sound[part]);

        registry.setContext (kVintagePageNames[1], part);
        envelopes[part] = std::make_unique<EnvelopesPage> (ctx, part);
        frame.addPageComponent (static_cast<int> (VintagePage::Envelopes), part, *envelopes[part]);

        registry.setContext (kVintagePageNames[2], part);
        lfo[part] = std::make_unique<LfoPage> (ctx, part);
        frame.addPageComponent (static_cast<int> (VintagePage::Lfo), part, *lfo[part]);
    }

    // Global pages: one component each (-1).
    registry.setContext (kVintagePageNames[static_cast<size_t> (VintagePage::Play)], -1);
    playPage = std::make_unique<PlayPage> (ctx, feed, &static_cast<VintageSynthProcessor&> (processor).getUiNotes());
    frame.addPageComponent (static_cast<int> (VintagePage::Play), -1, *playPage);

    registry.setContext (kVintagePageNames[static_cast<size_t> (VintagePage::ArpSeq)], -1);
    arpSeq = std::make_unique<ArpSeqPage> (ctx, feed, static_cast<VintageSynthProcessor&> (processor).getState(), editorState);
    frame.addPageComponent (static_cast<int> (VintagePage::ArpSeq), -1, *arpSeq);
    registry.setContext (kVintagePageNames[static_cast<size_t> (VintagePage::Effects)], -1);
    effectsPage = std::make_unique<EffectsPage> (ctx, editorState);
    frame.addPageComponent (static_cast<int> (VintagePage::Effects), -1, *effectsPage);
    registry.setContext (kVintagePageNames[static_cast<size_t> (VintagePage::Browser)], -1);
    browser = std::make_unique<BrowserPage> (ctx, static_cast<VintageSynthProcessor&> (processor).getPresets(), [this] { return frame.getPart(); });
    frame.addPageComponent (static_cast<int> (VintagePage::Browser), -1, *browser);
    registry.setContext ("frame", -1);
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

void VintageEditor::releaseKeysIfHidden()
{
    if (! isShowing())
        playPage->getStrip().endPlaying();
}

void VintageEditor::timerCallback()
{
    releaseKeysIfHidden();
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
