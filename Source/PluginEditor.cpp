#include "PluginEditor.h"

//==============================================================================
HanChiAudioProcessorEditor::HanChiAudioProcessorEditor (HanChiAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setResizable (true, true);
    setResizeLimits (760, 520, 1200, 820);

    auto initSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& text)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);
        addAndMakeVisible (s);
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    };

    initSlider (lowFreqSlider,   lowFreqLabel,   "低区分频");
    initSlider (lowThrSlider,    lowThrLabel,    "低区阈值");
    initSlider (highFreqSlider,  highFreqLabel,  "高区分频");
    initSlider (highThrSlider,   highThrLabel,   "高区阈值");
    initSlider (strengthSlider,  strengthLabel,  "强度");
    initSlider (lookaheadSlider, lookaheadLabel, "前瞻");

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

    modeBox.addItem ("宽带 Broadband", 1);
    modeBox.addItem ("分频段 Split", 2);
    initCombo (modeBox, modeLabel, "模式");
    modeAttach = std::make_unique<ComboAttach> (processor.apvts, "mode", modeBox);

    precisionBox.addItem ("32-bit", 1);
    precisionBox.addItem ("64-bit", 2);
    initCombo (precisionBox, precisionLabel, "精度");
    precisionAttach = std::make_unique<ComboAttach> (processor.apvts, "precision", precisionBox);

    oversamplingBox.addItem ("1x", 1);
    oversamplingBox.addItem ("2x", 2);
    oversamplingBox.addItem ("4x", 3);
    oversamplingBox.addItem ("8x", 4);
    initCombo (oversamplingBox, oversamplingLabel, "超采样");
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

    setSize (860, 560);
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
    templateBox.addItem ("— 选择模板 —", 1);
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
    if (name.isEmpty() || name == "— 选择模板 —") return;

    if (HanChiAudioProcessor::getBuiltinTemplateNames().contains (name))
        processor.applyBuiltinTemplate (name);
    else
        processor.applyUserTemplate (name);
}

void HanChiAudioProcessorEditor::saveTemplateDialog()
{
    auto* aw = new juce::AlertWindow ("存为模板", "请输入模板名称：",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", "我的模板");
    aw->addButton ("保存", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("取消", 0, juce::KeyPress (juce::KeyPress::escapeKey));

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
    g.drawText ("HanChi 汉齿 · 中文齿音处理器", getLocalBounds().removeFromTop (44),
                juce::Justification::centred);

    g.setFont (juce::FontOptions (13.0f));
    g.setColour (juce::Colour (0xff8a919c));
    g.drawText ("双频段动态去齿音 ｜ 低区 4-9kHz（嘶/嘘）  高区 7-13kHz（咝/刺感）",
                getLocalBounds().removeFromTop (64).removeFromBottom (18),
                juce::Justification::centred);

    // 增益削减表（右下角）
    auto meterArea = getLocalBounds().removeFromBottom (30)
                         .removeFromRight (280).withTrimmedRight (16);
    const float grL = processor.grLowDb.load();
    const float grH = processor.grHighDb.load();

    g.setColour (juce::Colour (0xff8a919c));
    g.setFont (juce::FontOptions (12.0f));

    auto drawMeter = [&] (juce::Rectangle<int>& zone, const juce::String& title,
                          float grDb, juce::Colour fill)
    {
        g.drawText (title, zone.removeFromLeft (56).reduced (2), juce::Justification::centredRight);
        auto bar = zone.removeFromLeft (64).reduced (4);
        g.setColour (juce::Colour (0xff2a2f38));
        g.fillRect (bar);
        const float frac = juce::jlimit (0.0f, 1.0f, -grDb / 24.0f);
        g.setColour (fill);
        g.fillRect (bar.removeFromRight ((int) (bar.getWidth() * frac)));
        g.setColour (juce::Colour (0xff8a919c));
        g.drawText (juce::String (grDb, 1) + " dB", bar.translated (4, 0).withWidth (56),
                    juce::Justification::centredLeft);
        zone.removeFromLeft (10);
    };

    drawMeter (meterArea, "低区 GR", grL, juce::Colour (0xff4f9ddb));
    drawMeter (meterArea, "高区 GR", grH, juce::Colour (0xffd97b4f));
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
