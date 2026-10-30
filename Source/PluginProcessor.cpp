#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Parameter IDs
static const juce::String pMode      { "mode" };
static const juce::String pLowEn     { "lowEnable" };
static const juce::String pLowFreq   { "lowFreq" };
static const juce::String pLowThr    { "lowThresh" };
static const juce::String pLowRatio  { "lowRatio" };
static const juce::String pLowStr    { "lowStrength" };
static const juce::String pLowSolo   { "lowSolo" };
static const juce::String pHighEn    { "highEnable" };
static const juce::String pHighFreq  { "highFreq" };
static const juce::String pHighThr   { "highThresh" };
static const juce::String pHighRatio { "highRatio" };
static const juce::String pHighStr   { "highStrength" };
static const juce::String pHighSolo  { "highSolo" };
static const juce::String pLookahead { "lookahead" };
static const juce::String pPrecision { "precision" };
static const juce::String pOs        { "oversampling" };

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout HanChiAudioProcessor::createParameterLayout()
{
    using P  = juce::AudioParameterFloat;
    using PC = juce::AudioParameterChoice;
    using PB = juce::AudioParameterBool;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<PC> (juce::ParameterID (pMode, 1), "Processing Mode",
                 juce::StringArray { "Broadband", "Split" }, 1));

    layout.add (std::make_unique<PB> (juce::ParameterID (pLowEn, 1), "Low Band", true));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowFreq, 1), "Low X-Over Freq",
                 juce::NormalisableRange<float> (4000.0f, 9000.0f, 1.0f, 0.5f), 6000.0f, "Hz"));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowThr, 1), "Low Threshold",
                 juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -30.0f, "dB"));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowRatio, 1), "Low Ratio",
                 juce::NormalisableRange<float> (1.5f, 20.0f, 0.5f), 8.0f, ":1"));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowStr, 1), "Low Strength",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, "%"));
    layout.add (std::make_unique<PB> (juce::ParameterID (pLowSolo, 1), "Low Solo", false));

    layout.add (std::make_unique<PB> (juce::ParameterID (pHighEn, 1), "High Band", true));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighFreq, 1), "High X-Over Freq",
                 juce::NormalisableRange<float> (7000.0f, 13000.0f, 1.0f, 0.5f), 9500.0f, "Hz"));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighThr, 1), "High Threshold",
                 juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -32.0f, "dB"));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighRatio, 1), "High Ratio",
                 juce::NormalisableRange<float> (1.5f, 20.0f, 0.5f), 8.0f, ":1"));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighStr, 1), "High Strength",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, "%"));
    layout.add (std::make_unique<PB> (juce::ParameterID (pHighSolo, 1), "High Solo", false));

    layout.add (std::make_unique<P> (juce::ParameterID (pLookahead, 1), "Lookahead",
                 juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 3.0f, "ms"));

    layout.add (std::make_unique<PC> (juce::ParameterID (pPrecision, 1), "Precision",
                 juce::StringArray { "32-bit Float", "64-bit Float" }, 0));
    layout.add (std::make_unique<PC> (juce::ParameterID (pOs, 1), "Oversampling",
                 juce::StringArray { "1x", "2x", "4x", "8x" }, 0));

    return layout;
}

//==============================================================================
HanChiAudioProcessor::HanChiAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "HanChiState", createParameterLayout())
{
}

HanChiAudioProcessor::~HanChiAudioProcessor() = default;

//==============================================================================
bool HanChiAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    return in == out && (in == juce::AudioChannelSet::mono()
                      || in == juce::AudioChannelSet::stereo());
}

void HanChiAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // Everything (oversamplers, filters, delay lines, work buffers) is
    // allocated here, once. process() will never allocate.
    engineF.prepare (sampleRate, samplesPerBlock);
    engineD.prepare (sampleRate, samplesPerBlock);

    scratchF.setSize (2, samplesPerBlock, false, false, true);
    scratchD.setSize (2, samplesPerBlock, false, false, true);

    // Report a CONSTANT latency exactly once, here.
    // Never call setLatencySamples from processBlock(): a latency change while
    // the transport is running makes the host rebuild its compensation graph,
    // which is exactly what crashed Cubase when switching oversampling.
    setLatencySamples (juce::jmax (engineF.getLatencySamples(),
                                   engineD.getLatencySamples()));
}

void HanChiAudioProcessor::releaseResources()
{
}

//==============================================================================
void HanChiAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = juce::jmin (buffer.getNumSamples(), scratchF.getNumSamples());

    if (numCh <= 0 || n <= 0)
        return;

    if (apvts.getRawParameterValue (pPrecision)->load() > 0.5f)   // 64-bit internal
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float*  src = buffer.getReadPointer (ch);
            double*       dst = scratchD.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = (double) src[i];
        }

        processInternal<double> (scratchD);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const double* src = scratchD.getReadPointer (ch);
            float*        dst = buffer.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = (float) src[i];
        }
    }
    else                                                          // 32-bit internal
    {
        processInternal<float> (buffer);
    }
}

void HanChiAudioProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = juce::jmin (buffer.getNumSamples(), scratchD.getNumSamples());

    if (numCh <= 0 || n <= 0)
        return;

    if (apvts.getRawParameterValue (pPrecision)->load() > 0.5f)   // 64-bit internal
    {
        processInternal<double> (buffer);
    }
    else                                                          // 32-bit internal
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            const double* src = buffer.getReadPointer (ch);
            float*        dst = scratchF.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = (float) src[i];
        }

        processInternal<float> (scratchF);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = scratchF.getReadPointer (ch);
            double*      dst = buffer.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = (double) src[i];
        }
    }
}

//==============================================================================
template <typename T>
void HanChiAudioProcessor::processInternal (juce::AudioBuffer<T>& buffer)
{
    hanchi::Params p;

    p.splitMode = apvts.getRawParameterValue (pMode)->load() > 0.5f;

    p.lowEn     = apvts.getRawParameterValue (pLowEn)->load() > 0.5f;
    p.lowFreq   = (double) apvts.getRawParameterValue (pLowFreq)->load();
    p.lowThr    = (double) apvts.getRawParameterValue (pLowThr)->load();
    p.lowRatio  = (double) apvts.getRawParameterValue (pLowRatio)->load();
    p.lowStr    = (double) apvts.getRawParameterValue (pLowStr)->load() * 0.01;
    p.lowSolo   = apvts.getRawParameterValue (pLowSolo)->load() > 0.5f;

    p.highEn    = apvts.getRawParameterValue (pHighEn)->load() > 0.5f;
    p.highFreq  = (double) apvts.getRawParameterValue (pHighFreq)->load();
    p.highThr   = (double) apvts.getRawParameterValue (pHighThr)->load();
    p.highRatio = (double) apvts.getRawParameterValue (pHighRatio)->load();
    p.highStr   = (double) apvts.getRawParameterValue (pHighStr)->load() * 0.01;
    p.highSolo  = apvts.getRawParameterValue (pHighSolo)->load() > 0.5f;

    p.lookMs    = (double) apvts.getRawParameterValue (pLookahead)->load();

    const int osIdx = juce::jlimit (0, 3, (int) apvts.getRawParameterValue (pOs)->load());
    p.osFactor  = 1 << osIdx;

    if constexpr (std::is_same_v<T, float>)
    {
        engineF.process (buffer, p);
        grLowDb  = engineF.getGrLowDb();
        grHighDb = engineF.getGrHighDb();
    }
    else
    {
        engineD.process (buffer, p);
        grLowDb  = engineD.getGrLowDb();
        grHighDb = engineD.getGrHighDb();
    }
}

template void HanChiAudioProcessor::processInternal<float>  (juce::AudioBuffer<float>&);
template void HanChiAudioProcessor::processInternal<double> (juce::AudioBuffer<double>&);

//==============================================================================
// State save / restore
void HanChiAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void HanChiAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// Template system
juce::File HanChiAudioProcessor::getUserTemplateDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("HanChi").getChildFile ("Templates");
    dir.createDirectory();
    return dir;
}

juce::StringArray HanChiAudioProcessor::getBuiltinTemplateNames()
{
    return { "Male Standard", "Female Standard", "Heavy Sibilance", "Bright Female",
             "Rap / Hard", "Folk / Guzheng Light", "Voice-over Broadcast", "Broadband Vintage De-Esser" };
}

juce::StringArray HanChiAudioProcessor::getUserTemplateNames() const
{
    juce::StringArray names;

    for (const auto& f : getUserTemplateDir().findChildFiles (juce::File::findFiles, false, "*.xml"))
        names.add (f.getFileNameWithoutExtension());

    names.sortNatural();
    return names;
}

void HanChiAudioProcessor::applyParamValues (const std::map<juce::String, float>& values)
{
    for (const auto& [id, v] : values)
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
}

void HanChiAudioProcessor::applyBuiltinTemplate (const juce::String& name)
{
    if (name == "Male Standard")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6000},{pLowThr,-30},{pLowRatio,6},{pLowStr,55},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-32},{pHighRatio,8},{pHighStr,60},{pLookahead,3} });
    else if (name == "Female Standard")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6500},{pLowThr,-26},{pLowRatio,8},{pLowStr,60},
                            {pHighEn,1},{pHighFreq,10000},{pHighThr,-28},{pHighRatio,10},{pHighStr,65},{pLookahead,3} });
    else if (name == "Heavy Sibilance")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,5500},{pLowThr,-22},{pLowRatio,10},{pLowStr,80},
                            {pHighEn,1},{pHighFreq,9000},{pHighThr,-24},{pHighRatio,12},{pHighStr,85},{pLookahead,4} });
    else if (name == "Bright Female")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,7000},{pLowThr,-28},{pLowRatio,8},{pLowStr,50},
                            {pHighEn,1},{pHighFreq,11000},{pHighThr,-26},{pHighRatio,8},{pHighStr,55},{pLookahead,3} });
    else if (name == "Rap / Hard")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,5000},{pLowThr,-20},{pLowRatio,12},{pLowStr,90},
                            {pHighEn,1},{pHighFreq,8500},{pHighThr,-22},{pHighRatio,14},{pHighStr,95},{pLookahead,5} });
    else if (name == "Folk / Guzheng Light")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6500},{pLowThr,-34},{pLowRatio,4},{pLowStr,35},
                            {pHighEn,1},{pHighFreq,10000},{pHighThr,-36},{pHighRatio,6},{pHighStr,40},{pLookahead,2} });
    else if (name == "Voice-over Broadcast")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6000},{pLowThr,-24},{pLowRatio,8},{pLowStr,75},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-25},{pHighRatio,10},{pHighStr,80},{pLookahead,4} });
    else if (name == "Broadband Vintage De-Esser")
        applyParamValues ({ {pMode,0},{pLowEn,1},{pLowFreq,6000},{pLowThr,-30},{pLowRatio,6},{pLowStr,60},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-32},{pHighRatio,8},{pHighStr,60},{pLookahead,2} });
}

bool HanChiAudioProcessor::saveUserTemplate (const juce::String& name)
{
    const juce::String safe = name.trim();

    if (safe.isEmpty() || safe.containsAnyOf ("/\\:*?\"<>|"))
        return false;

    juce::XmlElement xml ("HanChiTemplate");

    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            xml.setAttribute (ranged->paramID, ranged->convertFrom0to1 (ranged->getValue()));

    return xml.writeTo (getUserTemplateDir().getChildFile (safe + ".xml"));
}

bool HanChiAudioProcessor::applyUserTemplate (const juce::String& name)
{
    const juce::File f = getUserTemplateDir().getChildFile (name + ".xml");

    if (! f.existsAsFile())
        return false;

    std::unique_ptr<juce::XmlElement> xml (juce::parseXML (f));

    if (xml == nullptr || ! xml->hasTagName ("HanChiTemplate"))
        return false;

    std::map<juce::String, float> values;

    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (xml->hasAttribute (ranged->paramID))
                values[ranged->paramID] = (float) xml->getDoubleAttribute (ranged->paramID);

    applyParamValues (values);
    return true;
}

bool HanChiAudioProcessor::deleteUserTemplate (const juce::String& name)
{
    return getUserTemplateDir().getChildFile (name + ".xml").deleteFile();
}

//==============================================================================
juce::AudioProcessorEditor* HanChiAudioProcessor::createEditor()
{
    return new HanChiAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HanChiAudioProcessor();
}
