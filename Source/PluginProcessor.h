#pragma once

#include <JuceHeader.h>
#include "HanChiEngine.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <type_traits>
#include <vector>

//==============================================================================
/**
 * HanChi - Chinese Vocal De-Esser (v2.0)
 *
 *  Dual band dynamic de-esser: low band 4-9 kHz (s / sh / c) + high band
 *  7-13 kHz (hiss / sizzle), Linkwitz-Riley 4th order crossover.
 *  Broadband / Split modes; lookahead 0-10 ms;
 *  per band: threshold, ratio, strength and solo audition;
 *  32-bit / 64-bit internal precision; 1x - 8x oversampling.
 *  8 built-in templates + user templates (save / load / delete).
 *
 *  v2.0 stability rules:
 *   - oversamplers are pre-built in prepareToPlay(), never rebuilt on the
 *     audio thread;
 *   - the latency reported to the host is constant, so switching oversampling
 *     while the transport is running cannot trigger a host latency re-graph
 *     (the Cubase crash that was reported);
 *   - process() performs no allocation at all.
 */
class HanChiAudioProcessor : public juce::AudioProcessor
{
public:
    HanChiAudioProcessor();
    ~HanChiAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&,  juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return "HanChi"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    int  getNumPrograms() override                         { return 1; }
    int  getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //------ template system -------------------------------------------------
    static juce::StringArray getBuiltinTemplateNames();
    juce::StringArray getUserTemplateNames() const;
    void applyBuiltinTemplate (const juce::String& name);
    bool applyUserTemplate (const juce::String& name);   // false = missing / unreadable
    bool saveUserTemplate (const juce::String& name);    // false = illegal name
    bool deleteUserTemplate (const juce::String& name);
    static juce::File getUserTemplateDir();

    //------ gain reduction read by the editor (dB, zero or negative) --------
    std::atomic<float> grLowDb  { 0.0f };
    std::atomic<float> grHighDb { 0.0f };

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    template <typename T>
    void processInternal (juce::AudioBuffer<T>& buffer);

    void applyParamValues (const std::map<juce::String, float>& values);

    hanchi::Engine<float>  engineF;
    hanchi::Engine<double> engineD;

    juce::AudioBuffer<float>  scratchF;
    juce::AudioBuffer<double> scratchD;

    double currentSampleRate = 48000.0;
    int    currentBlockSize  = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HanChiAudioProcessor)
};
