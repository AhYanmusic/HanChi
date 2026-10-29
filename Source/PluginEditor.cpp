#include "PluginEditor.h"

//==============================================================================
HanChiAudioProcessorEditor::HanChiAudioProcessorEditor (HanChiAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setResizable (true, true);
    setResizeLimits (760, 660, 1200, 980);

    auto initSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& text)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);
        addAndMakeVisible (s);
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    };

    initSlider (lowFreqSlider,   lowFreqLabel,   "Low X-Over");
    initSlider (lowThrSlider,    lowThrLabel,    "Low Thresh");
    initSlider (highFreqSlider,  highFreqLabel,  "High X-Over");
    initSlider (highThrSlider,   highThrLabel,   "High Thresh");
    initSlider (strengthSlider,  strengthLabel,  "Strength");
    initSlider (lookaheadSlider, lookaheadLabel, "Lookahead");

    lowFreqSlider.setColour  (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff4f9ddb));
    highFreqSlider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffd97b4f));

    lowFreqAttach   = std::make_unique<SliderAttach> (processor.apvts, "lowFreq",    lowFreqSlider);
    lowThrAttach    = std::make_unique<SliderAttach> (processor.apvts, "lowThresh",  lowThrSlider);
    highFreqAttach  = std::make_unique<SliderAttach> (processor.apvts, "highFreq",   highFreqSlider);
    highThrAttach   = std::make_unique<SliderAttach> (processor.apvts, "highThresh", highThrSlider);
    strengthAttach  = std::make_unique<SliderAttach> (processor.apvts, "strength",   strengthSlider);
    lookaheadAttach = std::make_unique<SliderAttach> (processor.apvts, "lookahead",  lookaheadSlider);

    lowEnableAttach  = std::make_unique<ButtonAttach> (processor.apvts, "lowEnable",  lowEnableButton);
    highEnableAttach = std::make_unique<ButtonAttach> (processor.apvts, "highEnable", highEnableButton);
    addAndMakeVisible (lowEnableButton);
    addAndMakeVisible (highEnableButton);

    auto initCombo = [this] (juce::ComboBox& c, juce::Label& l, const juce::String& text)
    {
        addAndMakeVisible (c);
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };

    modeBox.addItem ("Broadband", 1);
    modeBox.addItem ("Split", 2);
    initCombo (modeBox, modeLabel, "Mode");
    modeAttach = std::make_unique<ComboAttach> (processor.apvts, "mode", modeBox);

    precisionBox.addItem ("32-bit", 1);
    precisionBox.addItem ("64-bit", 2);
    initCombo (precisionBox, precisionLabel, "Precision");
    precisionAttach = std::make_unique<ComboAttach> (processor.apvts, "precision", precisionBox);

    oversamplingBox.addItem ("1x", 1);
    oversamplingBox.addItem ("2x", 2);
    oversamplingBox.addItem ("4x", 3);
    oversamplingBox.addItem ("8x", 4);
    initCombo (oversamplingBox, oversamplingLabel, "Oversampling");
    oversamplingAttach = std::make_unique<ComboAttach> (processor.apvts, "oversampling", oversamplingBox);

    // 模板区
    addAndMakeVisible (templateBox);
    templateBox.setJustificationType (juce::Justification::centred);
    templateBox.onChange = [this] { applySelectedTemplate(); };
    addAndMakeVisible (saveTemplateButton);
    addAndMakeVisible (deleteTemplateButton);
    saveTemplateButton.onClick = [this] { saveTemplateDialog(); };
    deleteTemplateButton.onClick = [this]
    {
        const auto name = templateBox.getText();
        if (processor.getUserTemplateNames().contains (name))
        {
            processor.deleteUserTemplate (name);
            refreshTemplateList();
        }
    };
    refreshTemplateList();

    setSize (860, 730);
    startTimerHz (30);
}

HanChiAudioProcessorEditor::~HanChiAudioProcessorEditor()
{
    stopTimer();
}

void HanChiAudioProcessorEditor::timerCallback()
{
    repaint();
}

void HanChiAudioProcessorEditor::refreshTemplateList (const juce::String& selectName)
{
    templateBox.clear (juce::dontSendNotification);
    templateBox.addItem ("— Select Preset —", 1);
    int id = 2;
    for (const auto& n : HanChiAudioProcessor::getBuiltinTemplateNames())
        templateBox.addItem (n, id++);
    templateBox.addSeparator();
    for (const auto& n : processor.getUserTemplateNames())
        templateBox.addItem (n, id++);

    if (selectName.isNotEmpty())
    {
        int sel = 0;
        for (int i = 0; i < templateBox.getNumItems(); ++i)
            if (templateBox.getItemText (i) == selectName) { sel = i; break; }
        templateBox.setSelectedItemIndex (sel, juce::dontSendNotification);
    }
    else
    {
        templateBox.setSelectedItemIndex (0, juce::dontSendNotification);
    }
}

void HanChiAudioProcessorEditor::applySelectedTemplate()
{
    const auto name = templateBox.getText();
    if (name.isEmpty() || name == "— Select Preset —") return;

    if (HanChiAudioProcessor::getBuiltinTemplateNames().contains (name))
        processor.applyBuiltinTemplate (name);
    else
        processor.applyUserTemplate (name);
}

void HanChiAudioProcessorEditor::saveTemplateDialog()
{
    auto* aw = new juce::AlertWindow ("Save Preset", "Enter a preset name:",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", "My Preset");
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create
        ([this, aw] (int result)
        {
            if (result == 1)
            {
                const auto name = aw->getTextEditorContents ("name");
                if (processor.saveUserTemplate (name))
                    refreshTemplateList (name.trim());
            }
        }), true);
}

//==============================================================================
void HanChiAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2229));

    g.setColour (juce::Colour (0xffe8e2d6));
    g.setFont (juce::Font (juce::FontOptions (26.0f, juce::Font::bold)));
    g.drawText ("HanChi  ·  Chinese Vocal De-Esser", getLocalBounds().removeFromTop (44),
                juce::Justification::centred);

    g.setFont (juce::FontOptions (13.0f));
    g.setColour (juce::Colour (0xff8a919c));
    g.drawText ("Dual-Band Dynamic De-Esser   |   Low 4-9 kHz (s / sh / c)   -   High 7-13 kHz (hiss / sizzle)",
                getLocalBounds().removeFromTop (64).removeFromBottom (18),
                juce::Justification::centred);

    // 频段图示（布局与 resized() 保持一致）
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (56);    // 标题区
    area.removeFromTop (30);    // 模板行
    area.removeFromTop (8);
    area.removeFromTop (26);    // 选项行
    area.removeFromTop (12);
    area.removeFromTop (210);   // 旋钮区
    area.removeFromTop (8);
    area.removeFromTop (26);    // 频段启用
    area.removeFromTop (12);
    drawBandGraph (g, area);
}

//==============================================================================
// 频段图示：横轴为对数频率，标出两个分频点、三个频段块，以及各自当前的压缩量
void HanChiAudioProcessorEditor::drawBandGraph (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff15191f));
    g.fillRoundedRectangle (area.toFloat(), 6.0f);

    const double fMin = 1000.0, fMax = 20000.0;
    const double logMin  = std::log (fMin);
    const double logSpan = std::log (fMax) - logMin;

    auto xFor = [&] (double f)
    {
        const double t = juce::jlimit (0.0, 1.0,
            (std::log (juce::jlimit (fMin, fMax, f)) - logMin) / logSpan);
        return area.getX() + (int) (t * (double) area.getWidth());
    };

    const double lowF  = lowFreqSlider.getValue();
    const double highF = highFreqSlider.getValue();
    const int xLow  = xFor (lowF);
    const int xHigh = xFor (highF);

    const float grL   = processor.grLowDb.load();
    const float grH   = processor.grHighDb.load();
    const float maxDb = 20.0f;   // 满格 = -20 dB

    auto plot = area.reduced (8);
    plot.removeFromTop (16);      // 图例行
    plot.removeFromBottom (34);   // 频率刻度行

    const int nameH = 18;
    const int bodyY = plot.getY() + nameH;
    const int bodyH = juce::jmax (0, plot.getBottom() - bodyY);

    struct Zone { int x0, x1; juce::Colour c; juce::String name; float gr; };
    const Zone zones[]
    {
        { plot.getX(),  xLow,             juce::Colour (0xff6b7280), "Pass-through (dry)",       0.0f },
        { xLow,         xHigh,            juce::Colour (0xff4f9ddb), "Low Band  (s / sh / c)",   grL  },
        { xHigh,        plot.getRight(),  juce::Colour (0xffd97b4f), "High Band  (hiss/sizzle)", grH  }
    };

    for (const auto& z : zones)
    {
        if (z.x1 - z.x0 < 2) continue;
        auto zr = juce::Rectangle<int> (z.x0, bodyY, z.x1 - z.x0, bodyH);

        g.setColour (z.c.withAlpha (0.14f));
        g.fillRect (zr);

        // 压缩量：色块自顶部向下填充，满格代表 -20 dB
        const float frac = juce::jlimit (0.0f, 1.0f, -z.gr / maxDb);
        if (frac > 0.0f)
        {
            g.setColour (z.c.withAlpha (0.50f));
            g.fillRect (zr.withHeight ((int) ((float) zr.getHeight() * frac)));
        }

        g.setColour (z.c.withAlpha (0.75f));
        g.drawRect (zr, 1);

        g.setFont (juce::FontOptions (11.0f));
        g.setColour (juce::Colour (0xffc8ced8));
        g.drawText (z.name, z.x0, plot.getY(), z.x1 - z.x0, nameH, juce::Justification::centred);

        if (z.gr < -0.05f)
        {
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.setColour (juce::Colours::white);
            g.drawText (juce::String (z.gr, 1) + " dB", zr.reduced (4), juce::Justification::centred);
        }
    }

    // 分频点竖线 + 频率数值
    auto drawXover = [&] (int x, double freq, juce::Colour c)
    {
        g.setColour (c);
        g.drawVerticalLine (x, (float) bodyY, (float) plot.getBottom());
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String (freq, 0) + " Hz", x - 55, plot.getBottom() + 2, 110, 14,
                    juce::Justification::centred);
    };
    drawXover (xLow,  lowF,  juce::Colour (0xff4f9ddb));
    drawXover (xHigh, highF, juce::Colour (0xffd97b4f));

    // dB 参考线（-20 dB 顶部 / -10 dB 中线）
    g.setFont (juce::FontOptions (9.0f));
    for (float db : { -20.0f, -10.0f })
    {
        const float fr = juce::jlimit (0.0f, 1.0f, -db / maxDb);
        const int y = bodyY + (int) ((float) bodyH * fr);
        g.setColour (juce::Colour (0xff2a2f38));
        g.drawHorizontalLine (y, (float) plot.getX(), (float) plot.getRight());
        g.setColour (juce::Colour (0xff6b7280));
        g.drawText (juce::String (db, 0) + " dB", plot.getRight() - 44, y - 11, 44, 11,
                    juce::Justification::centredRight);
    }

    // 频率刻度
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (juce::Colour (0xff6b7280));
    for (double f : { 1000.0, 2000.0, 3000.0, 5000.0, 7000.0, 10000.0, 15000.0, 20000.0 })
    {
        const int x = xFor (f);
        g.drawVerticalLine (x, (float) area.getBottom() - 22, (float) area.getBottom() - 16);
        g.drawText (juce::String (f / 1000.0, 0) + "k",
                    x - 18, area.getBottom() - 14, 36, 12, juce::Justification::centred);
    }

    // 图例
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (juce::Colour (0xff8a919c));
    g.drawText ("Band Map   |   filled height = gain reduction   |   full bar = -20 dB",
                area.getX() + 10, area.getY() + 6, 460, 14, juce::Justification::centredLeft);
}

void HanChiAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (56);

    // 模板行
    auto tplRow = area.removeFromTop (30);
    templateBox.setBounds (tplRow.removeFromLeft (240));
    tplRow.removeFromLeft (12);
    saveTemplateButton.setBounds (tplRow.removeFromLeft (90));
    tplRow.removeFromLeft (6);
    deleteTemplateButton.setBounds (tplRow.removeFromLeft (90));

    area.removeFromTop (8);

    // 选项行：模式 / 精度 / 超采样
    auto optRow = area.removeFromTop (26);
    modeLabel.setBounds (optRow.removeFromLeft (46));
    modeBox.setBounds (optRow.removeFromLeft (150));
    optRow.removeFromLeft (16);
    precisionLabel.setBounds (optRow.removeFromLeft (40));
    precisionBox.setBounds (optRow.removeFromLeft (90));
    optRow.removeFromLeft (16);
    oversamplingLabel.setBounds (optRow.removeFromLeft (60));
    oversamplingBox.setBounds (optRow.removeFromLeft (70));

    area.removeFromTop (12);

    // 旋钮区（6 列）
    auto knobs = area.removeFromTop (210);
    const int kw = knobs.getWidth() / 6;

    auto place = [&] (juce::Slider& s, juce::Label& l, int index)
    {
        auto col = knobs.removeFromLeft (kw);
        if (index == 5) col = knobs; // 最后一列吃掉剩余宽度
        auto cell = col.reduced (6);
        s.setBounds (cell.removeFromTop (cell.getHeight() - 24));
        l.setBounds (cell.removeFromBottom (24));
    };

    place (lowFreqSlider,   lowFreqLabel,   0);
    place (lowThrSlider,    lowThrLabel,    1);
    place (highFreqSlider,  highFreqLabel,  2);
    place (highThrSlider,   highThrLabel,   3);
    place (strengthSlider,  strengthLabel,  4);
    place (lookaheadSlider, lookaheadLabel, 5);

    area.removeFromTop (8);
    auto bandRow = area.removeFromTop (26);
    lowEnableButton.setBounds  (bandRow.removeFromLeft (100).reduced (4));
    bandRow.removeFromLeft (24);
    highEnableButton.setBounds (bandRow.removeFromLeft (100).reduced (4));
}
