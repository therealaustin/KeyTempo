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

    float chipWidth (const juce::String& text, float s) { return textWidth (font (12.0f * s, true), text) + 20.0f * s; }

    const juce::String listening = juce::String::fromUTF8 ("Listening\xe2\x80\xa6");

    juce::String feelLabel (int feel)
    {
        return kt::feelName ((kt::Feel) feel);
    }
} // namespace

// =============================================================================
// Panel
// =============================================================================
void Panel::paint (juce::Graphics& g)
{
    const float s = scale();
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = 14.0f * s;

    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, radius);

    juce::ColourGradient glow (accent.withAlpha (0.10f), bounds.getCentreX(), bounds.getY(),
                               accent.withAlpha (0.0f), bounds.getCentreX(), bounds.getY() + 90.0f * s, false);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (bounds, radius);

    g.setColour (colours::border);
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    const float pad = 18.0f * s;
    auto titleRow = juce::Rectangle<float> (pad, pad, bounds.getWidth() - 2 * pad, 14.0f * s);
    g.setColour (accent);
    g.fillEllipse (titleRow.getX(), titleRow.getCentreY() - 3.0f * s, 6.0f * s, 6.0f * s);
    g.setColour (colours::textDim);
    g.setFont (tracked (11.0f * s, true, 0.14f));
    g.drawText (title, titleRow.withTrimmedLeft (14.0f * s), juce::Justification::centredLeft);

    if (state == nullptr)
        return;

    if (const auto b = badge(); b.isNotEmpty())
    {
        const float w = chipWidth (b, s);
        drawChip (g, juce::Rectangle<float> (titleRow.getRight() - w, titleRow.getCentreY() - 11.0f * s, w, 22.0f * s), b, accent, s);
    }

    if (showsConfidence())
    {
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
    }

    paintContent (g, contentArea());
}

juce::Rectangle<float> Panel::contentArea() const
{
    const float s = scale();
    const float pad = 18.0f * s;
    const float bottom = showsConfidence() ? 30.0f * s : 0.0f;
    return { pad, pad + 26.0f * s, getWidth() - 2 * pad, getHeight() - 2 * pad - 26.0f * s - bottom };
}

// =============================================================================
// Tempo
// =============================================================================
TempoPanel::TempoPanel (juce::AudioProcessorValueTreeState& parameters) : Panel ("TEMPO", colours::tempo), apvts (parameters)
{
    const juce::String names[] = { juce::String::fromUTF8 ("\xc2\xbd\xc3\x97"), juce::String::fromUTF8 ("1\xc3\x97"), juce::String::fromUTF8 ("2\xc3\x97") };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = scaleButtons[(size_t) i];
        b.setButtonText (names[i]);
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

void TempoPanel::syncButtons()
{
    const int index = juce::roundToInt (apvts.getRawParameterValue ("tempoScale")->load());
    for (int i = 0; i < 3; ++i)
        scaleButtons[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
}

juce::String TempoPanel::badge() const
{
    return state != nullptr && state->tempoValid && state->feel != 0 ? feelLabel (state->feel) : juce::String();
}

void TempoPanel::resized()
{
    const float s = scale();
    const auto area = contentArea();
    auto row = juce::Rectangle<float> (area.getX(), area.getY() + 128.0f * s, 156.0f * s, 28.0f * s);
    buttonRow = row;
    auto r = row.toNearestInt();
    const int w = r.getWidth() / 3;
    for (auto& b : scaleButtons)
        b.setBounds (r.removeFromLeft (w).reduced (juce::roundToInt (2 * s), 0));
}

void TempoPanel::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    auto top = area.removeFromTop (124.0f * s);

    if (! state->tempoValid)
    {
        g.setColour (colours::textMuted);
        g.setFont (font (96.0f * s, true));
        g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94"), top, juce::Justification::centredLeft);
        g.setFont (font (14.0f * s));
        g.drawText (state->hold ? juce::String ("Hold") : listening, top.withTrimmedLeft (90.0f * s), juce::Justification::centredLeft);
    }
    else
    {
        const double bpm = state->bpm * state->tempoScale;
        const auto text = juce::String (bpm, 1);
        const auto whole = text.upToFirstOccurrenceOf (".", false, false);
        const auto decimals = text.fromFirstOccurrenceOf (".", true, false);

        const auto big = font (112.0f * s, true);
        const auto small = font (40.0f * s, true);
        const float wWhole = textWidth (big, whole);
        const float wDec = textWidth (small, decimals);

        g.setColour (colours::text);
        g.setFont (big);
        g.drawText (whole, top.withWidth (wWhole + 4), juce::Justification::centredLeft);

        // Decimals and the unit share the big digits' baseline.
        const float baseline = top.getCentreY() + big.getAscent() * 0.5f - big.getDescent() * 0.35f;
        g.setColour (colours::textDim);
        g.setFont (small);
        g.drawSingleLineText (decimals, juce::roundToInt (top.getX() + wWhole + 2.0f * s), juce::roundToInt (baseline));
        g.setFont (font (17.0f * s, true));
        g.drawSingleLineText ("BPM", juce::roundToInt (top.getX() + wWhole + wDec + 12.0f * s), juce::roundToInt (baseline));
    }

    // Secondary line beside the multiplier buttons: alternate reading and host tempo.
    auto info = juce::Rectangle<float> (buttonRow.getRight() + 16.0f * s, buttonRow.getY(), area.getRight() - buttonRow.getRight() - 16.0f * s, buttonRow.getHeight());
    juce::StringArray parts;
    // Only octave alternates are worth showing: those are what the multiplier buttons fix.
    if (state->tempoValid && state->alternateBpm > 0.0f && state->bpm > 0.0f)
    {
        const float ratio = state->alternateBpm / state->bpm;
        if (std::abs (ratio - 0.5f) < 0.02f || std::abs (ratio - 2.0f) < 0.08f)
            parts.add ("Also fits " + juce::String (state->alternateBpm * state->tempoScale, 1)
                       + (ratio < 1.0f ? juce::String::fromUTF8 (" (\xc2\xbd\xc3\x97)") : juce::String::fromUTF8 (" (2\xc3\x97)")));
    }
    if (state->hostBpm > 0.0f)
        parts.add ("Host " + juce::String (state->hostBpm, 1));
    g.setColour (colours::textDim);
    g.setFont (font (12.5f * s));
    g.drawText (parts.joinIntoString (juce::String::fromUTF8 ("   \xc2\xb7   ")), info, juce::Justification::centredRight);

    area.removeFromTop (44.0f * s);
    paintHistory (g, area.reduced (0, 4.0f * s));
}

void TempoPanel::paintHistory (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    auto label = area.removeFromTop (14.0f * s);
    g.setColour (colours::textMuted);
    g.setFont (font (11.0f * s));
    g.drawText ("Readings, last 60 s", label, juce::Justification::centredLeft);
    area.removeFromTop (4.0f * s);

    g.setColour (colours::panelRaised);
    g.fillRoundedRectangle (area, 8.0f * s);

    const float centre = state->tempoValid ? state->bpm : 0.0f;
    if (centre <= 0.0f)
        return;

    // Log-scaled y axis from half to double the shown tempo: octave errors show up as
    // dots on the dashed guide lines, 3:2 confusions between them.
    const auto plot = area.reduced (10.0f * s, 8.0f * s);
    auto yFor = [&] (float bpm)
    {
        const float octaves = std::log2 (bpm / centre); // -1..1
        return plot.getCentreY() - octaves * plot.getHeight() * 0.5f;
    };

    for (float mult : { 0.5f, 2.0f })
    {
        const float y = yFor (centre * mult);
        g.setColour (colours::border);
        const float dashes[] = { 3.0f * s, 4.0f * s };
        g.drawDashedLine ({ plot.getX(), y, plot.getRight() - 34.0f * s, y }, dashes, 2, 1.0f);
        g.setColour (colours::textMuted);
        g.setFont (font (10.0f * s));
        g.drawText (mult < 1.0f ? juce::String::fromUTF8 ("\xc2\xbd\xc3\x97") : juce::String::fromUTF8 ("2\xc3\x97"),
                    juce::Rectangle<float> (plot.getRight() - 30.0f * s, y - 7.0f * s, 30.0f * s, 14.0f * s), juce::Justification::centredRight);
    }

    const int n = Readout::historyLength;
    const float dx = (plot.getWidth() - 34.0f * s) / (float) (n - 1);

    // Shown tempo as a line
    juce::Path line;
    bool started = false;
    for (int i = 0; i < n; ++i)
    {
        const float v = state->historyShown[(size_t) i];
        if (v <= 0.0f)
        {
            started = false;
            continue;
        }
        const float x = plot.getX() + i * dx, y = juce::jlimit (plot.getY(), plot.getBottom(), yFor (v));
        if (! started) line.startNewSubPath (x, y); else line.lineTo (x, y);
        started = true;
    }
    g.setColour (colours::tempo.withAlpha (0.8f));
    g.strokePath (line, juce::PathStrokeType (2.0f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Each window's reading as a dot
    for (int i = 0; i < n; ++i)
    {
        const float v = state->history[(size_t) i];
        if (v <= 0.0f || v < centre * 0.45f || v > centre * 2.2f)
            continue;
        const bool agrees = std::abs (v - centre) / centre < 0.015f;
        const float d = (agrees ? 3.0f : 4.0f) * s;
        g.setColour (agrees ? colours::text.withAlpha (0.45f) : colours::meter.withAlpha (0.85f));
        g.fillEllipse (plot.getX() + i * dx - d * 0.5f, yFor (v) - d * 0.5f, d, d);
    }
}

// =============================================================================
// Genre
// =============================================================================
GenrePanel::GenrePanel (juce::AudioProcessorValueTreeState& apvts) : Panel ("GENRE", colours::textDim)
{
    box.addItemList (KeyTempoProcessor::genreNames(), 1);
    box.setTooltip ("Auto suits most music. Choosing a genre locks its tempo conventions "
                    "(e.g. Drum & Bass always reads 160-190, never 87).");
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "genre", box);
    addAndMakeVisible (box);
}

void GenrePanel::resized()
{
    const float s = scale();
    const auto area = contentArea();
    box.setBounds (area.withHeight (30.0f * s).toNearestInt());
}

void GenrePanel::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    area.removeFromTop (36.0f * s);
    g.setColour (colours::textMuted);
    g.setFont (font (11.0f * s));
    const auto range = kt::tempoRangeFor ((kt::Genre) state->genre);
    const auto text = state->genre == 0 ? juce::String ("Detects the genre's feel automatically")
                                        : "Locked to " + juce::String ((int) range.lo) + "-" + juce::String ((int) range.hi) + " BPM";
    g.drawText (text, area.removeFromTop (16.0f * s), juce::Justification::centredLeft);
}

// =============================================================================
// Key
// =============================================================================
void KeyPanel::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    if (! state->keyValid || state->key < 0)
    {
        g.setColour (colours::textMuted);
        g.setFont (font (13.0f * s));
        g.drawText (state->hold ? juce::String ("Hold") : listening, area.removeFromTop (34.0f * s), juce::Justification::centredLeft);
        return;
    }

    const int key = state->key;
    const auto name = utf8 (kt::theory::keyName (key));
    auto row = area.removeFromTop (30.0f * s);
    g.setColour (colours::text);
    g.setFont (fitted (name, 26.0f * s, row.getWidth(), true));
    g.drawText (name, row, juce::Justification::centredLeft);

    area.removeFromTop (2.0f * s);
    const int rel = kt::theory::relativeKey (key);
    auto relRow = area.removeFromTop (17.0f * s);
    g.setFont (font (12.5f * s));
    g.setColour (colours::textDim);
    g.drawText ("Relative " + utf8 (kt::theory::keyName (rel)) + juce::String::fromUTF8 (" \xc2\xb7 ") + utf8 (kt::theory::camelot (rel)),
                relRow, juce::Justification::centredLeft);

    if (state->keyConfidence < 0.35f)
    {
        g.setColour (colours::textMuted);
        g.setFont (font (11.5f * s));
        g.drawText (juce::String::fromUTF8 ("Uncertain \xe2\x80\x94 still listening"), area.removeFromTop (16.0f * s), juce::Justification::centredLeft);
    }
}

juce::String KeyPanel::badge() const
{
    return state != nullptr && state->keyValid && state->key >= 0 ? utf8 (kt::theory::camelot (state->key)) : juce::String();
}

// =============================================================================
// Time signature
// =============================================================================
void MeterPanel::paintContent (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float s = scale();
    if (! state->meterValid)
    {
        g.setColour (colours::textMuted);
        g.setFont (font (13.0f * s));
        g.drawText (state->hold ? juce::String ("Hold") : listening, area.removeFromTop (34.0f * s), juce::Justification::centredLeft);
        return;
    }

    auto row = area.removeFromTop (30.0f * s);
    const auto sig = juce::String (state->numerator) + "/" + juce::String (state->denominator);
    const auto big = font (28.0f * s, true);
    g.setColour (colours::text);
    g.setFont (big);
    g.drawText (sig, row, juce::Justification::centredLeft);
    g.setColour (colours::textDim);
    g.setFont (font (12.5f * s));
    g.drawText (utf8 (kt::theory::meterDescription (state->beatsPerBar, state->compound)),
                row.withTrimmedLeft (textWidth (big, sig) + 12.0f * s), juce::Justification::centredLeft);

    if (state->meterConfidence < 0.35f)
    {
        g.setColour (colours::textMuted);
        g.setFont (font (11.5f * s));
        g.drawText (juce::String::fromUTF8 ("Uncertain \xe2\x80\x94 still listening"), area.removeFromTop (16.0f * s), juce::Justification::centredLeft);
    }

    if (state->hostNumerator > 0)
    {
        area.removeFromTop (4.0f * s);
        g.setColour (colours::textMuted);
        g.setFont (font (12.0f * s));
        g.drawText ("Host " + juce::String (state->hostNumerator) + "/" + juce::String (state->hostDenominator),
                    area.removeFromTop (18.0f * s), juce::Justification::centredLeft);
    }
}

// =============================================================================
// Editor
// =============================================================================
KeyTempoEditor::KeyTempoEditor (KeyTempoProcessor& p)
    : AudioProcessorEditor (p), processor (p), tempoPanel (p.getState()), genrePanel (p.getState())
{
    setLookAndFeel (&lnf);

    for (auto* panel : std::initializer_list<Panel*> { &tempoPanel, &genrePanel, &keyPanel, &meterPanel })
    {
        panel->setState (display);
        addAndMakeVisible (panel);
    }

    holdButton.setClickingTogglesState (true);
    holdButton.setColour (juce::TextButton::buttonOnColourId, colours::meter);
    holdButton.setTooltip ("Freeze the readings");
    holdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.getState(), "hold", holdButton);
    addAndMakeVisible (holdButton);

    resetButton.setTooltip ("Forget everything heard so far and start again (e.g. for the next track)");
    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible (resetButton);

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
    if (display.tempoValid)
        return "Analyzing";
    return listening;
}

void KeyTempoEditor::paint (juce::Graphics& g)
{
    const float s = getWidth() / (float) baseWidth;
    g.fillAll (colours::background);

    auto header = headerArea;
    g.setColour (colours::text);
    g.setFont (font (20.0f * s, true));
    const juce::String name ("KeyTempo");
    g.drawText (name, header, juce::Justification::centredLeft);
    const float nameW = textWidth (font (20.0f * s, true), name);

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
    for (auto* panel : std::initializer_list<Panel*> { &tempoPanel, &genrePanel, &keyPanel, &meterPanel })
        panel->uiScale = s;

    const int margin = juce::roundToInt (20 * s), gap = juce::roundToInt (14 * s);
    auto area = getLocalBounds().reduced (margin);

    auto header = area.removeFromTop (juce::roundToInt (34 * s));
    resetButton.setBounds (header.removeFromRight (juce::roundToInt (70 * s)).reduced (0, juce::roundToInt (3 * s)));
    header.removeFromRight (juce::roundToInt (8 * s));
    holdButton.setBounds (header.removeFromRight (juce::roundToInt (70 * s)).reduced (0, juce::roundToInt (3 * s)));
    header.removeFromRight (juce::roundToInt (18 * s));
    levelArea = header.removeFromRight (juce::roundToInt (120 * s)).toFloat();
    headerArea = header.toFloat();

    area.removeFromTop (juce::roundToInt (14 * s));

    auto right = area.removeFromRight (juce::roundToInt (250 * s));
    area.removeFromRight (gap);
    tempoPanel.setBounds (area);

    genrePanel.setBounds (right.removeFromTop (juce::roundToInt (100 * s)));
    right.removeFromTop (gap);
    const int rest = right.getHeight() - gap;
    keyPanel.setBounds (right.removeFromTop (rest / 2));
    right.removeFromTop (gap);
    meterPanel.setBounds (right);
}

void KeyTempoEditor::timerCallback()
{
    const auto& r = processor.getReadout();
    auto& apvts = processor.getState();

    auto glide = [] (float current, float target) { return current + (target - current) * 0.2f; };

    display.tempoValid = r.tempoValid;
    display.bpm = r.bpm;
    display.alternateBpm = r.alternateBpm;
    display.feel = r.feel;
    display.tempoConfidence = glide (display.tempoConfidence, r.tempoValid ? r.tempoConfidence.load() : 0.0f);
    const int scaleIndex = juce::roundToInt (apvts.getRawParameterValue ("tempoScale")->load());
    display.tempoScale = scaleIndex == 0 ? 0.5f : (scaleIndex == 2 ? 2.0f : 1.0f);
    display.genre = juce::roundToInt (apvts.getRawParameterValue ("genre")->load());

    const int write = r.historyWrite.load();
    for (int i = 0; i < Readout::historyLength; ++i)
    {
        const auto src = (size_t) ((write + i) % Readout::historyLength);
        display.history[(size_t) i] = r.history[src].load();
        display.historyShown[(size_t) i] = r.historyShown[src].load();
    }

    display.keyValid = r.keyValid;
    display.key = r.key;
    display.tuningCents = r.tuningCents;
    display.keyConfidence = glide (display.keyConfidence, r.keyValid ? r.keyConfidence.load() : 0.0f);

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

    tempoPanel.syncButtons();
    repaint();
}
