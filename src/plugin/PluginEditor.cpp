#include "PluginEditor.h"

#include <MusicTheory.h>

using namespace ui;

namespace
{
    juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    juce::Font tracked (float height, bool bold, float tracking)
    {
        auto options = juce::FontOptions (height).withKerningFactor (tracking);
        if (bold)
            options = options.withStyle ("Bold");
        return juce::Font (options);
    }

    float textWidth (const juce::Font& f, const juce::String& s)
    {
        return juce::GlyphArrangement::getStringWidth (f, s);
    }

    /** Largest font (up to maxHeight) at which `text` fits in `width`. */
    juce::Font fitted (const juce::String& text, float maxHeight, float width, bool bold)
    {
        auto f = font (maxHeight, bold);
        const float w = textWidth (f, text);
        if (w > width && w > 0.0f)
            f = font (maxHeight * width / w, bold);
        return f;
    }

    void drawChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour c, float s)
    {
        g.setColour (c.withAlpha (0.14f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (c);
        g.setFont (font (12.0f * s, true));
        g.drawText (text, r, juce::Justification::centred);
    }

    constexpr int majorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
    constexpr int minorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
} // namespace

// =============================================================================
// Card
// =============================================================================
void Card::paint (juce::Graphics& g)
{
    const float s = scale();
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = 14.0f * s;

    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, radius);

    // Faint accent glow along the top edge.
    juce::ColourGradient glow (accent.withAlpha (0.10f), bounds.getCentreX(), bounds.getY(),
                               accent.withAlpha (0.0f), bounds.getCentreX(), bounds.getY() + 90.0f * s, false);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (bounds, radius);

    g.setColour (colours::border);
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    // Title row
    const float pad = 18.0f * s;
    auto titleRow = juce::Rectangle<float> (pad, pad, bounds.getWidth() - 2 * pad, 14.0f * s);
    g.setColour (accent);
    g.fillEllipse (titleRow.getX(), titleRow.getCentreY() - 3.0f * s, 6.0f * s, 6.0f * s);
    g.setColour (colours::textDim);
    g.setFont (tracked (11.0f * s, true, 0.14f));
    g.drawText (title, titleRow.withTrimmedLeft (14.0f * s), juce::Justification::centredLeft);

    if (state == nullptr)
        return;

    // Confidence bar along the bottom.
    const float conf = valid() ? juce::jlimit (0.0f, 1.0f, confidence()) : 0.0f;
    auto bar = juce::Rectangle<float> (pad, bounds.getBottom() - pad - 4.0f * s, bounds.getWidth() - 2 * pad, 4.0f * s);
    auto label = bar.translated (0, -16.0f * s).withHeight (12.0f * s);
    g.setFont (font (11.0f * s));
    g.setColour (colours::textMuted);
    g.drawText ("Confidence", label, juce::Justification::centredLeft);
    g.drawText (valid() ? juce::String (juce::roundToInt (conf * 100.0f)) + "%" : juce::String ("-"), label, juce::Justification::centredRight);

    g.setColour (colours::border);
    g.fillRoundedRectangle (bar, bar.getHeight() * 0.5f);
    if (conf > 0.0f)
    {
        g.setColour (accent.withAlpha (0.35f + 0.65f * conf));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (bar.getHeight(), bar.getWidth() * conf)), bar.getHeight() * 0.5f);
    }

    paintContent (g, contentArea());
}

juce::Rectangle<float> Card::contentArea() const
{
    const float s = scale();
    const float pad = 18.0f * s;
    return { pad, pad + 26.0f * s, getWidth() - 2 * pad, getHeight() - 2 * pad - 26.0f * s - 30.0f * s };
}

void Card::paintPlaceholder (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& hint)
{
    const float s = scale();
    g.setColour (colours::textMuted);
    g.setFont (font (56.0f * s, true));
    g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94"), area.removeFromTop (78.0f * s), juce::Justification::centredLeft);
    g.setFont (font (13.0f * s));
    g.drawText (hint, area.removeFromTop (20.0f * s), juce::Justification::centredLeft);
}

// =============================================================================
// Tempo
// =============================================================================
TempoCard::TempoCard (juce::AudioProcessorValueTreeState& parameters) : Card ("TEMPO", colours::tempo), apvts (parameters)
{
    const juce::String names[] = { juce::String::fromUTF8 ("\xc2\xbd\xc3\x97"), juce::String::fromUTF8 ("1\xc3\x97"), juce::String::fromUTF8 ("2\xc3\x97") };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = scaleButtons[(size_t) i];
        b.setButtonText (names[i]);
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonOnColourId, colours::tempo);
        b.setTooltip ("Show the detected tempo halved, as-is, or doubled");
        b.onClick = [this, i]
        {
            if (auto* p = apvts.getParameter ("tempoScale"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 ((float) i));
                p->endChangeGesture();
            }
            syncButtons();
        };
        addAndMakeVisible (b);
    }
    syncButtons();
}

TempoCard::~TempoCard() = default;

void TempoCard::syncButtons()
{
    const int index = juce::roundToInt (apvts.getRawParameterValue ("tempoScale")->load());
    for (int i = 0; i < 3; ++i)
        scaleButtons[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
}

void TempoCard::resized()
{
    const float s = scale();
    auto area = contentArea();
    auto row = juce::Rectangle<float> (area.getX(), area.getY() + 112.0f * s, 150.0f * s, 26.0f * s).toNearestInt();
    const int w = row.getWidth() / 3;
    for (auto& b : scaleButtons)
        b.setBounds (row.removeFromLeft (w).reduced (juce::roundToInt (2 * s), 0));
}

void TempoCard::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    if (! state->tempoValid)
    {
        paintPlaceholder (g, area, state->hold ? "Hold" : juce::String::fromUTF8 ("Listening\xe2\x80\xa6"));
        return;
    }

    const double bpm = state->bpm * state->tempoScale;
    const auto value = juce::String (bpm, 1);
    auto big = font (64.0f * s, true);
    auto valueRow = area.removeFromTop (78.0f * s);
    g.setColour (colours::text);
    g.setFont (big);
    g.drawText (value, valueRow, juce::Justification::centredLeft);

    const float w = textWidth (big, value);
    g.setColour (colours::textDim);
    g.setFont (font (15.0f * s, true));
    g.drawText ("BPM", valueRow.withTrimmedLeft (w + 8.0f * s).withTrimmedTop (30.0f * s), juce::Justification::centredLeft);

    // Small secondary line: rounded value and the host's tempo, if any.
    g.setFont (font (12.5f * s));
    g.setColour (colours::textDim);
    juce::String line = juce::String::fromUTF8 ("\xe2\x89\x88 ") + juce::String (juce::roundToInt (bpm)) + " BPM";
    if (state->hostBpm > 0.0f)
        line << juce::String::fromUTF8 ("   \xc2\xb7   Host ") << juce::String (state->hostBpm, 1);
    g.drawText (line, area.removeFromTop (22.0f * s), juce::Justification::centredLeft);
}

// =============================================================================
// Key
// =============================================================================
void KeyCard::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    if (! state->keyValid || state->key < 0)
    {
        paintPlaceholder (g, area, state->hold ? "Hold" : juce::String::fromUTF8 ("Listening\xe2\x80\xa6"));
        return;
    }

    const int key = state->key;
    const auto name = utf8 (kt::theory::keyName (key));
    g.setColour (colours::text);
    g.setFont (fitted (name, 44.0f * s, area.getWidth(), true));
    g.drawText (name, area.removeFromTop (78.0f * s), juce::Justification::centredLeft);

    // Relative key
    const int rel = kt::theory::relativeKey (key);
    auto relRow = area.removeFromTop (22.0f * s);
    g.setFont (font (13.0f * s));
    g.setColour (colours::textDim);
    g.drawText ("Relative", relRow, juce::Justification::centredLeft);
    g.setColour (colours::text);
    g.setFont (font (13.0f * s, true));
    g.drawText (utf8 (kt::theory::keyName (rel)), relRow.withTrimmedLeft (62.0f * s), juce::Justification::centredLeft);

    area.removeFromTop (8.0f * s);

    // Chips: short name, Camelot code, tuning
    auto chips = area.removeFromTop (24.0f * s);
    const auto shortName = utf8 (kt::theory::shortKeyName (key));
    const auto cam = utf8 (kt::theory::camelot (key));
    const float a4 = 440.0f * std::pow (2.0f, state->tuningCents / 1200.0f);
    const auto tuning = "A4 " + juce::String (a4, 1);

    auto chip = [&] (const juce::String& text, juce::Colour c)
    {
        const float w = textWidth (font (12.0f * s, true), text) + 18.0f * s;
        drawChip (g, chips.removeFromLeft (w), text, c, s);
        chips.removeFromLeft (6.0f * s);
    };
    chip (shortName, colours::key);
    chip (cam, colours::key);
    if (std::abs (state->tuningCents) >= 5.0f)
        chip (tuning, colours::textDim);
}

// =============================================================================
// Time signature
// =============================================================================
void MeterCard::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    if (! state->meterValid)
    {
        paintPlaceholder (g, area, state->hold ? "Hold" : juce::String::fromUTF8 ("Listening\xe2\x80\xa6"));
        return;
    }

    // Stacked numerals, as on a score.
    auto numerals = area.removeFromLeft (64.0f * s).removeFromTop (110.0f * s);
    g.setColour (colours::text);
    g.setFont (font (50.0f * s, true));
    g.drawText (juce::String (state->numerator), numerals.removeFromTop (55.0f * s), juce::Justification::centred);
    g.drawText (juce::String (state->denominator), numerals, juce::Justification::centred);

    area.removeFromLeft (12.0f * s);

    // Description
    g.setColour (colours::text);
    g.setFont (font (14.0f * s, true));
    g.drawText (utf8 (kt::theory::meterDescription (state->beatsPerBar, state->compound)),
                area.removeFromTop (22.0f * s), juce::Justification::centredLeft);
    g.setColour (colours::textDim);
    g.setFont (font (12.0f * s));
    const auto beats = juce::String (state->beatsPerBar) + (state->beatsPerBar == 1 ? " beat" : " beats") + " per bar";
    g.drawText (beats, area.removeFromTop (18.0f * s), juce::Justification::centredLeft);
    g.drawText (state->compound ? "Beats split in three" : "Beats split in two", area.removeFromTop (18.0f * s),
                juce::Justification::centredLeft);

    area.removeFromTop (10.0f * s);

    // Beat grid: one group per beat, with its subdivisions; the downbeat is lit.
    auto grid = area.removeFromTop (16.0f * s);
    const int subdivisions = state->compound ? 3 : 2;
    const float big = 9.0f * s, small = 5.0f * s, gap = 5.0f * s, groupGap = 10.0f * s;
    float x = grid.getX();
    for (int b = 0; b < state->beatsPerBar; ++b)
    {
        for (int sub = 0; sub < subdivisions; ++sub)
        {
            const float d = sub == 0 ? big : small;
            const auto c = (b == 0 && sub == 0) ? colours::meter : (sub == 0 ? colours::text.withAlpha (0.7f) : colours::textMuted);
            g.setColour (c);
            g.fillEllipse (x, grid.getCentreY() - d * 0.5f, d, d);
            x += d + gap;
        }
        x += groupGap - gap;
    }

    if (state->hostNumerator > 0)
    {
        area.removeFromTop (8.0f * s);
        g.setColour (colours::textMuted);
        g.setFont (font (12.0f * s));
        g.drawText ("Host " + juce::String (state->hostNumerator) + "/" + juce::String (state->hostDenominator),
                    area.removeFromTop (18.0f * s), juce::Justification::centredLeft);
    }
}

// =============================================================================
// Chroma
// =============================================================================
void ChromaView::paint (juce::Graphics& g)
{
    if (state == nullptr)
        return;

    const auto bounds = getLocalBounds().toFloat();
    const float s = getHeight() / 100.0f;
    auto area = bounds.reduced (16.0f * s, 12.0f * s);

    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds.reduced (0.5f), 14.0f * s);
    g.setColour (colours::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 14.0f * s, 1.0f);

    auto header = area.removeFromTop (14.0f * s);
    g.setColour (colours::textDim);
    g.setFont (tracked (11.0f * s, true, 0.14f));
    g.drawText ("PITCH CLASSES", header, juce::Justification::centredLeft);

    // Which pitch classes belong to the detected key's scale?
    std::array<bool, 12> inScale {};
    int tonic = -1;
    if (state->keyValid && state->key >= 0)
    {
        tonic = kt::theory::tonic (state->key);
        const int* scale = kt::theory::isMinor (state->key) ? minorScale : majorScale;
        for (int i = 0; i < 7; ++i)
            inScale[(size_t) ((tonic + scale[i]) % 12)] = true;
    }

    area.removeFromTop (6.0f * s);
    auto labels = area.removeFromBottom (14.0f * s);
    const float colW = area.getWidth() / 12.0f;
    for (int pc = 0; pc < 12; ++pc)
    {
        auto col = juce::Rectangle<float> (area.getX() + pc * colW, area.getY(), colW, area.getHeight()).reduced (colW * 0.18f, 0);
        const float v = juce::jlimit (0.0f, 1.0f, state->chroma[(size_t) pc]);

        g.setColour (colours::border.withAlpha (0.6f));
        g.fillRoundedRectangle (col, 3.0f * s);

        const auto c = pc == tonic ? colours::key : (inScale[(size_t) pc] ? colours::key.withAlpha (0.55f) : colours::textMuted);
        g.setColour (c);
        g.fillRoundedRectangle (col.withTrimmedTop (col.getHeight() * (1.0f - v)), 3.0f * s);

        g.setColour (pc == tonic ? colours::text : colours::textDim);
        g.setFont (font (10.5f * s, pc == tonic));
        g.drawText (utf8 (kt::theory::pitchClassName (pc)),
                    juce::Rectangle<float> (area.getX() + pc * colW, labels.getY(), colW, labels.getHeight()),
                    juce::Justification::centred);
    }
}

// =============================================================================
// Editor
// =============================================================================
KeyTempoEditor::KeyTempoEditor (KeyTempoProcessor& p)
    : AudioProcessorEditor (p), processor (p), tempoCard (p.getState())
{
    setLookAndFeel (&lnf);

    for (auto* card : std::initializer_list<Card*> { &tempoCard, &keyCard, &meterCard })
    {
        card->setState (display);
        addAndMakeVisible (card);
    }
    chroma.setState (display);
    addAndMakeVisible (chroma);

    holdButton.setClickingTogglesState (true);
    holdButton.setColour (juce::TextButton::buttonOnColourId, colours::meter);
    holdButton.setTooltip ("Freeze the readings");
    holdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.getState(), "hold", holdButton);
    addAndMakeVisible (holdButton);

    resetButton.setTooltip ("Forget everything heard so far and start again");
    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible (resetButton);

    rangeBox.addItemList (KeyTempoProcessor::tempoRangeNames(), 1);
    memoryBox.addItemList (KeyTempoProcessor::keyMemoryNames(), 1);
    rangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.getState(), "tempoRange", rangeBox);
    memoryAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.getState(), "keyMemory", memoryBox);
    for (auto* box : { &rangeBox, &memoryBox })
        addAndMakeVisible (box);
    for (auto* label : { &rangeLabel, &memoryLabel })
    {
        label->setColour (juce::Label::textColourId, colours::textDim);
        addAndMakeVisible (label);
    }

    setResizable (true, true);
    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) baseWidth / baseHeight);
        c->setSizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);
    }
    setSize (baseWidth, baseHeight);

    timerCallback();
    startTimerHz (30);
}

KeyTempoEditor::~KeyTempoEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

juce::String KeyTempoEditor::statusText() const
{
    if (display.hold)
        return "Hold";
    if (display.level < 1.0e-4f)
        return "No input";
    if (display.tempoValid || display.keyValid)
        return "Analyzing";
    return juce::String::fromUTF8 ("Listening\xe2\x80\xa6");
}

void KeyTempoEditor::paint (juce::Graphics& g)
{
    const float s = getWidth() / (float) baseWidth;
    g.fillAll (colours::background);

    // Title
    auto header = headerArea;
    g.setColour (colours::text);
    g.setFont (font (20.0f * s, true));
    const juce::String name ("KeyTempo");
    g.drawText (name, header, juce::Justification::centredLeft);
    const float nameW = textWidth (font (20.0f * s, true), name);

    // Status pill
    const auto status = statusText();
    const bool active = status == "Analyzing";
    const auto statusColour = display.hold ? colours::meter : (active ? colours::good : colours::textDim);
    auto pill = juce::Rectangle<float> (header.getX() + nameW + 14.0f * s, header.getCentreY() - 11.0f * s,
                                        textWidth (font (12.0f * s, true), status) + 30.0f * s, 22.0f * s);
    g.setColour (statusColour.withAlpha (0.12f));
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
    g.setColour (statusColour);
    g.fillEllipse (pill.getX() + 10.0f * s, pill.getCentreY() - 3.0f * s, 6.0f * s, 6.0f * s);
    g.setFont (font (12.0f * s, true));
    g.drawText (status, pill.withTrimmedLeft (22.0f * s), juce::Justification::centredLeft);

    // Input meter
    const float db = juce::Decibels::gainToDecibels (display.level, -60.0f);
    const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour (colours::textMuted);
    g.setFont (font (10.0f * s, true));
    g.drawText ("IN", levelArea.withWidth (18.0f * s), juce::Justification::centredLeft);
    auto meter = levelArea.withTrimmedLeft (20.0f * s).withSizeKeepingCentre (levelArea.getWidth() - 20.0f * s, 5.0f * s);
    g.setColour (colours::border);
    g.fillRoundedRectangle (meter, 2.5f * s);
    juce::ColourGradient grad (colours::good, meter.getX(), 0, colours::meter, meter.getRight(), 0, false);
    grad.addColour (0.85, colours::meter);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * norm), 2.5f * s);
}

void KeyTempoEditor::resized()
{
    const float s = getWidth() / (float) baseWidth;
    lnf.uiScale = s;
    const int margin = juce::roundToInt (20 * s), gap = juce::roundToInt (14 * s);

    auto area = getLocalBounds().reduced (margin);

    // Header
    auto header = area.removeFromTop (juce::roundToInt (34 * s));
    resetButton.setBounds (header.removeFromRight (juce::roundToInt (70 * s)).reduced (0, juce::roundToInt (3 * s)));
    header.removeFromRight (juce::roundToInt (8 * s));
    holdButton.setBounds (header.removeFromRight (juce::roundToInt (70 * s)).reduced (0, juce::roundToInt (3 * s)));
    header.removeFromRight (juce::roundToInt (18 * s));
    levelArea = header.removeFromRight (juce::roundToInt (120 * s)).toFloat();
    headerArea = header.toFloat();

    area.removeFromTop (juce::roundToInt (14 * s));

    // Footer: chroma on the left, settings on the right
    auto footer = area.removeFromBottom (juce::roundToInt (108 * s));
    area.removeFromBottom (gap);

    auto settings = footer.removeFromRight (juce::roundToInt (220 * s));
    footer.removeFromRight (gap);
    chroma.setBounds (footer);

    const int rowH = juce::roundToInt (16 * s), boxH = juce::roundToInt (30 * s);
    auto placeSetting = [&] (juce::Label& label, juce::ComboBox& box)
    {
        label.setBounds (settings.removeFromTop (rowH));
        label.setFont (tracked (11.0f * s, true, 0.14f));
        label.setBorderSize ({ 0, 2, 0, 0 });
        box.setBounds (settings.removeFromTop (boxH));
        settings.removeFromTop (juce::roundToInt (14 * s));
    };
    settings.removeFromTop (juce::roundToInt (2 * s));
    placeSetting (rangeLabel, rangeBox);
    placeSetting (memoryLabel, memoryBox);

    // Cards
    const int cardW = (area.getWidth() - 2 * gap) / 3;
    tempoCard.setBounds (area.removeFromLeft (cardW));
    area.removeFromLeft (gap);
    keyCard.setBounds (area.removeFromLeft (cardW));
    area.removeFromLeft (gap);
    meterCard.setBounds (area);
}

void KeyTempoEditor::timerCallback()
{
    const auto& r = processor.getReadout();
    auto& apvts = processor.getState();

    // Smooth confidences so the bars glide rather than jump.
    auto glide = [] (float current, float target) { return current + (target - current) * 0.2f; };

    display.tempoValid = r.tempoValid;
    display.bpm = r.bpm;
    display.tempoConfidence = glide (display.tempoConfidence, r.tempoValid ? r.tempoConfidence.load() : 0.0f);
    const int scaleIndex = juce::roundToInt (apvts.getRawParameterValue ("tempoScale")->load());
    display.tempoScale = scaleIndex == 0 ? 0.5f : (scaleIndex == 2 ? 2.0f : 1.0f);

    display.keyValid = r.keyValid;
    display.key = r.key;
    display.tuningCents = r.tuningCents;
    display.keyConfidence = glide (display.keyConfidence, r.keyValid ? r.keyConfidence.load() : 0.0f);
    for (size_t i = 0; i < 12; ++i)
        display.chroma[i] = glide (display.chroma[i], r.chroma[i].load());

    display.meterValid = r.meterValid;
    display.numerator = r.numerator;
    display.denominator = r.denominator;
    display.beatsPerBar = r.beatsPerBar;
    display.compound = r.compound;
    display.meterConfidence = glide (display.meterConfidence, r.meterValid ? r.meterConfidence.load() : 0.0f);

    display.level = r.inputLevel;
    display.hostBpm = r.hostBpm;
    display.hostNumerator = r.hostNumerator;
    display.hostDenominator = r.hostDenominator;
    display.hold = apvts.getRawParameterValue ("hold")->load() > 0.5f;

    tempoCard.syncButtons();
    repaint();
}
