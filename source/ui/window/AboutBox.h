#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>
#include "../AVDexedTheme.h"
#include "../AVDexedLanguage.h"
#include "../DXLookNFeel.h"

/** Public product information; technical provenance remains in notices. */
class AboutBox final : public juce::DialogWindow, private juce::ChangeListener {
    class AVAboutContent final : public juce::Component {
    public:
        AVAboutContent() {
            if (auto svg = juce::XmlDocument::parse(juce::String::fromUTF8(BinaryData::AVDexed_InfoIcon_svg,
                                                                         BinaryData::AVDexed_InfoIcon_svgSize)))
                AVInfoIcon = juce::Drawable::createFromSVG(*svg);
            for (auto* link : { &AVDexedLink, &AVSurgeLink }) {
                link->setColour(juce::HyperlinkButton::textColourId, AVDexedTheme::oceanHighlight);
                auto linkFont = AVDexedTheme::font(13.0f, "Medium");
                linkFont.setUnderline(true);
                link->setFont(linkFont, false, juce::Justification::centredLeft);
                link->setWantsKeyboardFocus(true);
                addAndMakeVisible(link);
            }
            AVRefreshLanguage();
            setSize(540, 500);
        }
        void AVRefreshLanguage() {
            AVDexedLink.setButtonText(AVDexedLanguageService::getInstance().text("originalProject"));
            AVDexedLink.setTitle(AVDexedLink.getButtonText());
            AVSurgeLink.setTitle(AVSurgeLink.getButtonText());
            repaint();
        }
        void resized() override {
            AVDexedLink.setBounds(24, getHeight() - 68, getWidth() - 48, 28);
            AVSurgeLink.setBounds(24, getHeight() - 36, getWidth() - 48, 28);
        }
        void paint(juce::Graphics& g) override {
            AVDexedTheme::panel(g, getLocalBounds().toFloat(), 10.0f);
            if (AVInfoIcon != nullptr)
                AVInfoIcon->drawWithin(g, { 24.0f, 24.0f, 32.0f, 32.0f }, juce::RectanglePlacement::centred, 1.0f);
            g.setColour(AVDexedTheme::textPrimary);
            g.setFont(AVDexedTheme::font(23.0f, "SemiBold"));
            g.drawText("AV Dexed", 70, 20, getWidth() - 94, 42, juce::Justification::centredLeft);
            auto& language = AVDexedLanguageService::getInstance();
            g.setFont(AVDexedTheme::font(11.0f));
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawText(language.format("aboutVersion", {{ "version", DEXED_VERSION }}), 24, 70, getWidth() - 48, 26, juce::Justification::centredLeft);
            juce::AttributedString credits;
            credits.append(language.text("aboutCredits"), AVDexedTheme::font(11.5f, "Regular"), AVDexedTheme::textPrimary);
            credits.setLineSpacing(3.0f);
            juce::TextLayout layout;
            layout.createLayout(credits, static_cast<float>(getWidth() - 48));
            layout.draw(g, { 24.0f, 106.0f, static_cast<float>(getWidth() - 48), static_cast<float>(getHeight() - 182) });
        }
    private:
        juce::HyperlinkButton AVDexedLink { "DEXED", juce::URL("https://asb2m10.github.io/dexed/") };
        juce::HyperlinkButton AVSurgeLink { "Surge Synthesizer Team", juce::URL("https://surge-synthesizer.github.io/") };
        std::unique_ptr<juce::Drawable> AVInfoIcon;
    };
public:
    AboutBox() : juce::DialogWindow(AVDexedLanguageService::getInstance().text("aboutDexed"), AVDexedTheme::canvas, true) {
        setLookAndFeel(&AVLookAndFeel);
        setUsingNativeTitleBar(true);
        setAlwaysOnTop(true);
        auto* content = new AVAboutContent();
        AVContent = content;
        setContentOwned(content, true);
        AVDexedLanguageService::getInstance().addChangeListener(this);
    }
    ~AboutBox() override {
        AVDexedLanguageService::getInstance().removeChangeListener(this);
        setLookAndFeel(nullptr);
    }
    void closeButtonPressed() override { exitModalState(0); setVisible(false); }
private:
    DXLookNFeel AVLookAndFeel;
    AVAboutContent* AVContent = nullptr;
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        setName(AVDexedLanguageService::getInstance().text("aboutDexed"));
        AVContent->AVRefreshLanguage();
    }
};
