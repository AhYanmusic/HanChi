#pragma once

#include "PluginProcessor.h"

//==============================================================================
class HanChiAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit HanChiAudioProcessorEditor (HanChiAudioProcessor&);
    ~HanChiAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshTemplateList (const juce::String& selectName = {});
    void applySelectedTemplate();
    void saveTemplateDialog();
    void drawBandGraph (juce::Graphics&, juce::Rectangle<int>);

    HanChiAudioProcessor& processor;

    // 频段参数
    juce::Slider lowFreqSlider, lowThrSlider, highFreqSlider, highThrSlider,
                 strengthSlider, lookaheadSlider;
    juce::Label  lowFreqLabel, lowThrLabel, highFreqLabel, highThrLabel,
                 strengthLabel, lookaheadLabel;

    juce::ToggleButton lowEnableButton { "Low Band" }, highEnableButton { "High Band" };

    // 模式 / 精度 / 超采样
    juce::ComboBox modeBox, precisionBox, oversamplingBox;
    juce::Label    modeLabel, precisionLabel, oversamplingLabel;

    // 模板
    juce::ComboBox templateBox;
    juce::TextButton saveTemplateButton { "Save Preset" },
                     deleteTemplateButton { "Delete" };

    using SliderAttach  = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttach   = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttach  = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttach> lowFreqAttach, lowThrAttach, highFreqAttach,
                                  highThrAttach, strengthAttach, lookaheadAttach;
    std::unique_ptr<ComboAttach>  modeAttach, precisionAttach, oversamplingAttach;
    std::unique_ptr<ButtonAttach> lowEnableAttach, highEnableAttach;

    float peakLowDb  = 0.0f;   // 峰值保持（负值，dB）
    float peakHighDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HanChiAudioProcessorEditor)
};
