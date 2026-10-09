#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
namespace colours
{
    const juce::Colour background   { 0xff0c0e13 };
    const juce::Colour panel        { 0xff141720 };
    const juce::Colour panelRaised  { 0xff1b1f2a };
    const juce::Colour border       { 0xff252a37 };
    const juce::Colour text         { 0xfff1f3f8 };
    const juce::Colour textDim      { 0xff8b93a7 };
    const juce::Colour textMuted    { 0xff4d5568 };
    const juce::Colour tempo        { 0xff3dd6f5 };
    const juce::Colour key          { 0xffa78bfa };
    const juce::Colour meter        { 0xffffb547 };
    const juce::Colour good         { 0xff4ade80 };
} // namespace colours

juce::Font font (float height, bool bold = false);

/** Flat, rounded, dark styling for the stock widgets we use. */
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool down, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;

    juce::Font getLabelFont (juce::Label&) override;

    float uiScale = 1.0f; // set by the editor on resize
};
} // namespace ui
