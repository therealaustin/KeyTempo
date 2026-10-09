#pragma once

#include "LookAndFeel.h"
#include "PluginProcessor.h"

/** Snapshot of the processor's Readout plus smoothed values for drawing. */
struct DisplayState
{
    bool tempoValid = false, keyValid = false, meterValid = false, hold = false;
    float bpm = 0, alternateBpm = 0, tempoConfidence = 0, keyConfidence = 0, meterConfidence = 0, tuningCents = 0;
    float tempoScale = 1.0f;
    int feel = 0, genre = 0;
    int key = -1, numerator = 4, denominator = 4, beatsPerBar = 4;
    bool compound = false;
    float level = 0, hostBpm = 0;
    int hostNumerator = 0, hostDenominator = 0;
    std::array<float, Readout::historyLength> history {}, historyShown {}; // oldest first
};

/** Rounded panel with a dot + title, an optional confidence bar, and content. */
class Panel : public juce::Component
{
public:
    Panel (juce::String titleText, juce::Colour accentColour) : title (std::move (titleText)), accent (accentColour) {}

    void paint (juce::Graphics&) override;
    void setState (const DisplayState& s) { state = &s; }

protected:
    virtual void paintContent (juce::Graphics&, juce::Rectangle<float> area) = 0;
    virtual bool showsConfidence() const { return false; }
    virtual float confidence() const { return 0.0f; }
    virtual bool valid() const { return false; }
    /** Optional text drawn at the right of the title row. */
    virtual juce::String badge() const { return {}; }

    juce::Rectangle<float> contentArea() const;
    float scale() const { return uiScale; }

    juce::String title;
    juce::Colour accent;
    const DisplayState* state = nullptr;

public:
    float uiScale = 1.0f; // set by the editor
};

class TempoPanel final : public Panel
{
public:
    explicit TempoPanel (juce::AudioProcessorValueTreeState&);
    void resized() override;
    void syncButtons();

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
    void paintHistory (juce::Graphics&, juce::Rectangle<float>);
    bool showsConfidence() const override { return true; }
    float confidence() const override { return state->tempoConfidence; }
    bool valid() const override { return state->tempoValid; }
    juce::String badge() const override;

    juce::AudioProcessorValueTreeState& apvts;
    std::array<juce::TextButton, 3> scaleButtons;
    juce::Rectangle<float> buttonRow;
};

class KeyPanel final : public Panel
{
public:
    KeyPanel() : Panel ("KEY", ui::colours::key) {}

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
    juce::String badge() const override;
};

class MeterPanel final : public Panel
{
public:
    MeterPanel() : Panel ("TIME SIGNATURE", ui::colours::meter) {}

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
};

class GenrePanel final : public Panel
{
public:
    explicit GenrePanel (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;

    juce::ComboBox box;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

class KeyTempoEditor final : public juce::AudioProcessorEditor,
                             private juce::Timer
{
public:
    explicit KeyTempoEditor (KeyTempoProcessor&);
    ~KeyTempoEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::String statusText() const;

    KeyTempoProcessor& processor;
    ui::LookAndFeel lnf;
    DisplayState display;

    TempoPanel tempoPanel;
    GenrePanel genrePanel;
    KeyPanel keyPanel;
    MeterPanel meterPanel;

    juce::TextButton holdButton { "Hold" }, resetButton { "Reset" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> holdAttachment;

    juce::Rectangle<float> headerArea, levelArea;

    static constexpr int baseWidth = 820, baseHeight = 470;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyTempoEditor)
};
