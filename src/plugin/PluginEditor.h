#pragma once

#include "LookAndFeel.h"
#include "PluginProcessor.h"

/** Snapshot of the processor's Readout plus smoothed values for drawing. */
struct DisplayState
{
    bool tempoValid = false, keyValid = false, meterValid = false, hold = false;
    float bpm = 0, tempoConfidence = 0, keyConfidence = 0, meterConfidence = 0, tuningCents = 0;
    float tempoScale = 1.0f;
    int key = -1, numerator = 4, denominator = 4, beatsPerBar = 4;
    bool compound = false;
    std::array<float, 12> chroma {};
    float level = 0, hostBpm = 0;
    int hostNumerator = 0, hostDenominator = 0;
};

class Card : public juce::Component
{
public:
    Card (juce::String titleText, juce::Colour accentColour) : title (std::move (titleText)), accent (accentColour) {}

    void paint (juce::Graphics&) override;
    void setState (const DisplayState& s) { state = &s; }

protected:
    /** Area below the title and above the confidence bar. */
    juce::Rectangle<float> contentArea() const;
    virtual void paintContent (juce::Graphics&, juce::Rectangle<float> area) = 0;
    virtual float confidence() const = 0;
    virtual bool valid() const = 0;
    void paintPlaceholder (juce::Graphics&, juce::Rectangle<float> area, const juce::String& hint);

    float scale() const { return getWidth() / 250.0f; }

    juce::String title;
    juce::Colour accent;
    const DisplayState* state = nullptr;
};

class TempoCard final : public Card
{
public:
    explicit TempoCard (juce::AudioProcessorValueTreeState&);
    ~TempoCard() override;
    void resized() override;
    void syncButtons();

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
    float confidence() const override { return state->tempoConfidence; }
    bool valid() const override { return state->tempoValid; }

    juce::AudioProcessorValueTreeState& apvts;
    std::array<juce::TextButton, 3> scaleButtons;
};

class KeyCard final : public Card
{
public:
    KeyCard() : Card ("KEY", ui::colours::key) {}

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
    float confidence() const override { return state->keyConfidence; }
    bool valid() const override { return state->keyValid; }
};

class MeterCard final : public Card
{
public:
    MeterCard() : Card ("TIME SIGNATURE", ui::colours::meter) {}

private:
    void paintContent (juce::Graphics&, juce::Rectangle<float>) override;
    float confidence() const override { return state->meterConfidence; }
    bool valid() const override { return state->meterValid; }
};

class ChromaView final : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void setState (const DisplayState& s) { state = &s; }

private:
    const DisplayState* state = nullptr;
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

    TempoCard tempoCard;
    KeyCard keyCard;
    MeterCard meterCard;
    ChromaView chroma;

    juce::TextButton holdButton { "Hold" }, resetButton { "Reset" };
    juce::ComboBox rangeBox, memoryBox;
    juce::Label rangeLabel { {}, "TEMPO RANGE" }, memoryLabel { {}, "KEY MEMORY" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> holdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rangeAttachment, memoryAttachment;

    juce::Rectangle<float> headerArea, levelArea, statusArea;

    static constexpr int baseWidth = 820, baseHeight = 470;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyTempoEditor)
};
