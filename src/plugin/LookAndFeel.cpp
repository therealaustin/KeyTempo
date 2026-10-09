#include "LookAndFeel.h"

namespace ui
{
juce::Font font (float height, bool bold)
{
    auto options = juce::FontOptions (height);
    if (bold)
        options = options.withStyle ("Bold");
    return juce::Font (options);
}

LookAndFeel::LookAndFeel()
{
    using namespace colours;
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::TextButton::buttonColourId, panelRaised);
    setColour (juce::TextButton::buttonOnColourId, tempo);
    setColour (juce::TextButton::textColourOffId, textDim);
    setColour (juce::TextButton::textColourOnId, background);
    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, panelRaised);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, border);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::Label::textColourId, textDim);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (r.getHeight() * 0.5f, 8.0f * uiScale);
    const bool on = b.getToggleState();

    auto fill = on ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
    if (! on && highlighted)
        fill = fill.brighter (0.15f);
    if (down)
        fill = fill.darker (0.15f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    if (! on)
    {
        g.setColour (colours::border);
        g.drawRoundedRectangle (r, radius, 1.0f);
    }
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jmin (13.0f * uiScale, buttonHeight * 0.5f), true);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                     .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, 1);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    const float radius = juce::jmin (r.getHeight() * 0.5f, 8.0f * uiScale);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (box.isMouseOver (true) ? 0.08f : 0.0f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, radius, 1.0f);

    // Chevron
    const float s = height * 0.16f;
    const float cx = (float) width - height * 0.55f, cy = height * 0.5f;
    juce::Path p;
    p.startNewSubPath (cx - s, cy - s * 0.45f);
    p.lineTo (cx, cy + s * 0.55f);
    p.lineTo (cx + s, cy - s * 0.45f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (p, juce::PathStrokeType (1.5f * uiScale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return font (juce::jmin (13.0f * uiScale, box.getHeight() * 0.48f));
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (juce::roundToInt (10 * uiScale), 0, box.getWidth() - box.getHeight() - juce::roundToInt (6 * uiScale), box.getHeight());
    label.setFont (getComboBoxFont (box));
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (colours::panelRaised);
    g.setColour (colours::border);
    g.drawRect (0, 0, width, height);
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                     bool isHighlighted, bool isTicked, bool, const juce::String& text,
                                     const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
        return;
    auto r = area.reduced (4, 1);
    if (isHighlighted && isActive)
    {
        g.setColour (colours::border);
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
    }
    g.setColour (isTicked ? colours::tempo : colours::text.withAlpha (isActive ? 1.0f : 0.4f));
    g.setFont (getPopupMenuFont());
    g.drawFittedText (text, r.reduced (10, 0), juce::Justification::centredLeft, 1);
}

juce::Font LookAndFeel::getPopupMenuFont()
{
    return font (14.0f * juce::jmax (1.0f, uiScale));
}

juce::Font LookAndFeel::getLabelFont (juce::Label&)
{
    return font (11.0f * uiScale, true);
}
} // namespace ui
