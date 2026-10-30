#include "PluginEditor.h"

//==============================================================================
HanChiAudioProcessorEditor::HanChiAudioProcessorEditor (HanChiAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setResizable (true, true);
    setResizeLimits (880, 720, 1400, 1100);

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
    initSlider (lowRatioSlider,  lowRatioLabel,  "Low Ratio");
    initSlider (lowStrSlider,    lowStrLabel,    "Low Strength");
    initSlider (highFreqSlider,  highFreqLabel,  "High X-Over");
    initSlider (highThrSlider,   highThrLabel,   "High Thresh");
    initSlider (highRatioSlider, highRatioLabel, "High Ratio");
    initSlider (highStrSlider,   highStrLabel,   "High Strength");
    initSlider (lookaheadSlider, lookaheadLabel, "Lookahead");

    lowFreqSlider.setColour  (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff4f9ddb));
    highFreqSlider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffd97b4f));

    lowFreqAttach    = std::make_unique<SliderAttach> (processor.apvts, "lowFreq",     lowFreqSlider);
    lowThrAttach     = std::make_unique<SliderAttach> (processor.apvts, "lowThresh",   lowThrSlider);
    lowRatioAttach   = std::make_unique<SliderAttach> (processor.apvts, "lowRatio",    lowRatioSlider);
    lowStrAttach     = std::make_unique<SliderAttach> (processor.apvts, "lowStrength", lowStrSlider);
    highFreqAttach   = std::make_unique<SliderAttach> (processor.apvts, "highFreq",    highFreqSlider);
    highThrAttach    = std::make_unique<SliderAttach> (processor.apvts, "highThresh",  highThrSlider);
    highRatioAttach  = std::make_unique<SliderAttach> (processor.apvts, "highRatio",   highRatioSlider);
    highStrAttach    = std::make_unique<SliderAttach> (processor.apvts, "highStrength",highStrSlider);
    lookaheadAttach  = std::make_unique<SliderAttach> (processor.apvts, "lookahead",   lookaheadSlider);

    lowEnableAttach  = std::make_unique<ButtonAttach> (processor.apvts, "lowEnable",  lowEnableButton);
    highEnableAttach = std::make_unique<ButtonAttach> (processor.apvts, "highEnable", highEnableButton);
    lowSoloAttach    = std::make_unique<ButtonAttach> (processor.apvts, "lowSolo",    lowSoloButton);
    highSoloAttach   = std::make_unique<ButtonAttach> (processor.apvts, "highSolo",   highSoloButton);
    addAndMakeVisible (lowEnableButton);
    addAndMakeVisible (highEnableButton);
    addAndMakeVisible (lowSoloButton);
    addAndMakeVisible (highSoloButton);

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

    setSize (980, 840);
    startTimerHz (30);
}

HanChiAudioProcessorEditor::~HanChiAudioProcessorEditor()
{
    stopTimer();
}

void HanChiAudioProcessorEditor::timerCallback()
{
    const float grL = processor.grLowDb.load();
    const float grH = processor.grHighDb.load();

    // 峰值保持：立刻跟随更大的削减，之后缓慢回落（约 1.5 s）
    auto track = [] (float& peak, float cur)
    {
        if (cur < peak)                 peak = cur;
        else                            peak += (cur - peak) * 0.03f;
        if (peak > 0.0f)                peak = 0.0f;
    };
    track (peakLowDb,  grL);
    track (peakHighDb, grH);

    // Solo 仅对「分频段(Split)」模式有意义；宽带模式下禁用（状态保留，切回 Split 仍生效）
    const bool splitMode = processor.apvts.getParameter ("mode")->getValue() > 0.5f;
    lowSoloButton.setEnabled (splitMode);
    highSoloButton.setEnabled (splitMode);

    repaint();
}

void HanChiAudioProcessorEditor::refreshTemplateList (const juce::String& selectName)
{
    templateBox.clear (juce::dontSendNotification);
    templateBox.addItem ("- Select Preset -", 1);
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
    if (name.isEmpty() || name == "- Select Preset -") return;

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
    g.drawText ("HanChi - Chinese Vocal De-Esser", getLocalBounds().removeFromTop (44),
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
    area.removeFromTop (96);    // 低区旋钮行
    area.removeFromTop (6);
    area.removeFromTop (26);    // 低区开关行（启用 / Solo）
    area.removeFromTop (10);
    area.removeFromTop (96);    // 高区旋钮行
    area.removeFromTop (6);
    area.removeFromTop (26);    // 高区开关行（启用 / Solo）
    area.removeFromTop (10);
    drawBandGraph (g, area);
}

//==============================================================================
// 频段图示：横轴为对数频率，标出两个分频点、三个频段块，以及各自当前的压缩量
void HanChiAudioProcessorEditor::drawBandGraph (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff15191f));
    g.fillRoundedRectangle (area.toFloat(), 6.0f);

    const double fMin = 2000.0, fMax = 20000.0;
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
    const bool splitMode = processor.apvts.getParameter ("mode")->getValue() > 0.5f;

    auto plot = area.reduced (8);
    plot.removeFromTop (16);      // 图例行
    plot.removeFromBottom (34);   // 频率刻度行

    const int nameH = 18;
    const int bodyY = plot.getY() + nameH;
    const int bodyH = juce::jmax (0, plot.getBottom() - bodyY);

    struct Zone { int x0, x1; juce::Colour c; juce::String name; float gr; float peak; bool solo; };
    const Zone zones[]
    {
        { plot.getX(),  xLow,             juce::Colour (0xff6b7280), "Pass-through (dry)",       0.0f,   0.0f,       false },
        { xLow,         xHigh,            juce::Colour (0xff4f9ddb), "Low Band  (s / sh / c)",   grL,    peakLowDb,  (splitMode && lowSoloButton.getToggleState())  },
        { xHigh,        plot.getRight(),  juce::Colour (0xffd97b4f), "High Band  (hiss/sizzle)", grH,    peakHighDb, (splitMode && highSoloButton.getToggleState()) }
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

        // Solo 指示
        if (z.solo)
        {
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.setColour (juce::Colour (0xffe6c34a));
            g.drawText ("SOLO", z.x0 + 2, bodyY + 2, z.x1 - z.x0 - 4, 14, juce::Justification::centredRight);
        }

        // 峰值保持线：显示这一两秒内压到过的最深值
        if (z.peak < -0.05f && bodyH > 4)
        {
            const float pf = juce::jlimit (0.0f, 1.0f, -z.peak / maxDb);
            const int py = bodyY + (int) ((float) bodyH * pf);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawHorizontalLine (py, (float) (z.x0 + 2), (float) (z.x1 - 2));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("peak " + juce::String (z.peak, 1),
                        z.x0 + 2, py - 12, z.x1 - z.x0 - 4, 11, juce::Justification::centredRight);
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
    for (double f : { 2000.0, 3000.0, 5000.0, 7000.0, 10000.0, 15000.0, 20000.0 })
    {
        const int x = xFor (f);
        g.drawVerticalLine (x, (float) area.getBottom() - 22, (float) area.getBottom() - 16);
        g.drawText (juce::String (f / 1000.0, 0) + "k",
                    x - 18, area.getBottom() - 14, 36, 12, juce::Justification::centred);
    }

    // 图例
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (juce::Colour (0xff8a919c));
    g.drawText ("Band Map   |   filled height = gain reduction   |   white line = peak   |   full bar = -20 dB",
                area.getX() + 10, area.getY() + 6, 560, 14, juce::Justification::centredLeft);
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

    // 旋钮放置助手：在给定列里放一个旋钮 + 其标签
    auto placeKnob = [] (juce::Rectangle<int> col, juce::Slider& s, juce::Label& l)
    {
        auto cell = col.reduced (8, 2);
        s.setBounds (cell.removeFromTop (cell.getHeight() - 22));
        l.setBounds (cell.removeFromBottom (22));
    };
    const int kw5 = area.getWidth() / 5;   // 每行 5 列

    // ---- 低区旋钮行（Low X-Over / Low Thresh / Low Ratio / Low Strength / Lookahead） ----
    auto lowKnobs = area.removeFromTop (96);
    placeKnob (lowKnobs.removeFromLeft (kw5), lowFreqSlider,   lowFreqLabel);
    placeKnob (lowKnobs.removeFromLeft (kw5), lowThrSlider,    lowThrLabel);
    placeKnob (lowKnobs.removeFromLeft (kw5), lowRatioSlider,  lowRatioLabel);
    placeKnob (lowKnobs.removeFromLeft (kw5), lowStrSlider,    lowStrLabel);
    placeKnob (lowKnobs,                      lookaheadSlider, lookaheadLabel); // 最后一列吃剩余

    area.removeFromTop (6);
    auto lowToggles = area.removeFromTop (26);
    lowEnableButton.setBounds (lowToggles.removeFromLeft (110).reduced (4));
    lowToggles.removeFromLeft (12);
    lowSoloButton.setBounds   (lowToggles.removeFromLeft (90).reduced (4));

    area.removeFromTop (10);

    // ---- 高区旋钮行（High X-Over / High Thresh / High Ratio / High Strength） ----
    auto highKnobs = area.removeFromTop (96);
    placeKnob (highKnobs.removeFromLeft (kw5), highFreqSlider,  highFreqLabel);
    placeKnob (highKnobs.removeFromLeft (kw5), highThrSlider,   highThrLabel);
    placeKnob (highKnobs.removeFromLeft (kw5), highRatioSlider, highRatioLabel);
    placeKnob (highKnobs.removeFromLeft (kw5), highStrSlider,   highStrLabel);

    area.removeFromTop (6);
    auto highToggles = area.removeFromTop (26);
    highEnableButton.setBounds (highToggles.removeFromLeft (110).reduced (4));
    highToggles.removeFromLeft (12);
    highSoloButton.setBounds   (highToggles.removeFromLeft (90).reduced (4));

    area.removeFromTop (10);
}
