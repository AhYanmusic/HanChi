#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// 参数 ID
static const juce::String pMode      { "mode" };
static const juce::String pLowEn     { "lowEnable" };
static const juce::String pLowFreq   { "lowFreq" };
static const juce::String pLowThr    { "lowThresh" };
static const juce::String pHighEn    { "highEnable" };
static const juce::String pHighFreq  { "highFreq" };
static const juce::String pHighThr   { "highThresh" };
static const juce::String pStrength  { "strength" };
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

    layout.add (std::make_unique<PC> (juce::ParameterID (pMode, 1), "处理模式",
                 juce::StringArray { "宽带 Broadband", "分频段 Split" }, 1));

    layout.add (std::make_unique<PB> (juce::ParameterID (pLowEn, 1), "低区启用", true));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowFreq, 1), "低区分频频率",
                 juce::NormalisableRange<float> (4000.0f, 9000.0f, 1.0f, 0.5f), 6000.0f, "Hz"));
    layout.add (std::make_unique<P> (juce::ParameterID (pLowThr, 1), "低区阈值",
                 juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -30.0f, "dB"));

    layout.add (std::make_unique<PB> (juce::ParameterID (pHighEn, 1), "高区启用", true));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighFreq, 1), "高区分频频率",
                 juce::NormalisableRange<float> (7000.0f, 13000.0f, 1.0f, 0.5f), 9500.0f, "Hz"));
    layout.add (std::make_unique<P> (juce::ParameterID (pHighThr, 1), "高区阈值",
                 juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -32.0f, "dB"));

    layout.add (std::make_unique<P> (juce::ParameterID (pStrength, 1), "强度",
                 juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, "%"));
    layout.add (std::make_unique<P> (juce::ParameterID (pLookahead, 1), "前瞻 Lookahead",
                 juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 3.0f, "ms"));

    layout.add (std::make_unique<PC> (juce::ParameterID (pPrecision, 1), "内部精度",
                 juce::StringArray { "32-bit 浮点", "64-bit 浮点" }, 0));
    layout.add (std::make_unique<PC> (juce::ParameterID (pOs, 1), "超采样",
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
    apvts.addParameterListener (pOs,      this);
    apvts.addParameterListener (pLowFreq, this);
    apvts.addParameterListener (pHighFreq, this);
}

HanChiAudioProcessor::~HanChiAudioProcessor()
{
    apvts.removeParameterListener (pOs,      this);
    apvts.removeParameterListener (pLowFreq, this);
    apvts.removeParameterListener (pHighFreq, this);
}

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

    filtersDirty     = true;
    oversamplerDirty = true;
    rebuildOversamplersIfNeeded();

    for (int ch = 0; ch < 2; ++ch)
    {
        detMid[ch].prepare  (sampleRate);
        detHigh[ch].prepare (sampleRate);
        detWide[ch].prepare (sampleRate);
    }
    scratchF.setSize (2, samplesPerBlock, false, false, true);
    scratchD.setSize (2, samplesPerBlock, false, false, true);
}

void HanChiAudioProcessor::rebuildOversamplersIfNeeded()
{
    if (! oversamplerDirty.exchange (false) && oversF != nullptr && oversD != nullptr)
        return;

    const int comboIdx = (int) apvts.getRawParameterValue (pOs)->load(); // 0..3 -> 1x,2x,4x,8x
    const size_t factor = (size_t) 1 << comboIdx;

    oversF = std::make_unique<juce::dsp::Oversampling<float>> (2, factor,
              juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    oversD = std::make_unique<juce::dsp::Oversampling<double>> (2, factor,
              juce::dsp::Oversampling<double>::filterHalfBandPolyphaseIIR, true);

    // JUCE 的 Oversampling 走 initProcessing(maxSamplesBeforeOversampling)
    oversF->initProcessing ((size_t) currentBlockSize);
    oversD->initProcessing ((size_t) currentBlockSize);
    oversF->reset();
    oversD->reset();
    oversamplerFactor = (int) factor;
}

void HanChiAudioProcessor::rebuildBiquads()
{
    const double fs = currentSampleRate;
    const float lf = apvts.getRawParameterValue (pLowFreq)->load();
    const float hf = apvts.getRawParameterValue (pHighFreq)->load();

    for (int ch = 0; ch < 2; ++ch)
    {
        lowLP_F[ch].set  (fs, lf, true);   midHP_F[ch].set (fs, lf, false);
        midLP_F[ch].set  (fs, hf, true);   highHP_F[ch].set (fs, hf, false);
        lowLP_D[ch].set  (fs, lf, true);   midHP_D[ch].set (fs, lf, false);
        midLP_D[ch].set  (fs, hf, true);   highHP_D[ch].set (fs, hf, false);
    }
    filtersDirty = false;
}

void HanChiAudioProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == pOs) oversamplerDirty = true;
    if (id == pLowFreq || id == pHighFreq) filtersDirty = true;
}

void HanChiAudioProcessor::updateLatency (int lookSamples)
{
    double osLatency = 0.0;
    if (oversamplerFactor > 1)
        osLatency = oversF != nullptr ? oversF->getLatencyInSamples() : 0.0;
    setLatencySamples (juce::roundToInt ((double) lookSamples + osLatency));
}

//==============================================================================
void HanChiAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    rebuildOversamplersIfNeeded();
    if (filtersDirty) rebuildBiquads();

    if (apvts.getRawParameterValue (pPrecision)->load() > 0.5f)   // 内部 64-bit
    {
        scratchD.setSize (buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::transform (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + buffer.getNumSamples(),
                            scratchD.getWritePointer (ch), [] (float v) { return (double) v; });
        processInternal<double> (scratchD);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::transform (scratchD.getReadPointer (ch), scratchD.getReadPointer (ch) + buffer.getNumSamples(),
                            buffer.getWritePointer (ch), [] (double v) { return (float) v; });
    }
    else
    {
        processInternal<float> (buffer);
    }
}

void HanChiAudioProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    rebuildOversamplersIfNeeded();
    if (filtersDirty) rebuildBiquads();

    if (apvts.getRawParameterValue (pPrecision)->load() > 0.5f)
    {
        processInternal<double> (buffer);
    }
    else                                                          // 内部 32-bit
    {
        scratchF.setSize (buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::transform (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + buffer.getNumSamples(),
                            scratchF.getWritePointer (ch), [] (double v) { return (float) v; });
        processInternal<float> (scratchF);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::transform (scratchF.getReadPointer (ch), scratchF.getReadPointer (ch) + buffer.getNumSamples(),
                            buffer.getWritePointer (ch), [] (float v) { return (double) v; });
    }
}

//==============================================================================
template <typename T>
void HanChiAudioProcessor::processInternal (juce::AudioBuffer<T>& buffer)
{
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

    const bool splitMode  = apvts.getRawParameterValue (pMode)->load() > 0.5f;
    const bool lowEn      = apvts.getRawParameterValue (pLowEn)->load() > 0.5f;
    const bool highEn     = apvts.getRawParameterValue (pHighEn)->load() > 0.5f;
    const double lowThr   = (double) apvts.getRawParameterValue (pLowThr)->load();
    const double highThr  = (double) apvts.getRawParameterValue (pHighThr)->load();
    const double strength = (double) apvts.getRawParameterValue (pStrength)->load() * 0.01;
    const int lookSamples = (int) std::round (apvts.getRawParameterValue (pLookahead)->load()
                                              * 0.001 * currentSampleRate);

    // 前瞻延迟线尺寸
    auto& dLow  = dLowX<T>();
    auto& dMid  = dMidX<T>();
    auto& dHigh = dHighX<T>();
    for (int ch = 0; ch < 2; ++ch) { dLow[ch].setSize (lookSamples); dMid[ch].setSize (lookSamples); dHigh[ch].setSize (lookSamples); }

    // 超采样：升频
    T* chPtr[2] { nullptr, nullptr };
    int chunk = 0;
    if (oversamplerFactor > 1)
    {
        auto& os = oversX<T>();
        auto up = os.processSamplesUp (juce::dsp::AudioBlock<T> (buffer));
        chunk = (int) up.getNumSamples();
        for (int ch = 0; ch < numChannels; ++ch)
            chPtr[ch] = up.getChannelPointer (ch);
    }
    else
    {
        chunk = buffer.getNumSamples();
        for (int ch = 0; ch < numChannels; ++ch)
            chPtr[ch] = buffer.getWritePointer (ch);
    }

    // 滤波器组（按精度选择）
    auto& lowLP  = lowLP_X<T>();
    auto& midHP  = midHP_X<T>();
    auto& midLP  = midLP_X<T>();
    auto& highHP = highHP_X<T>();

    float grMidMin = 0.0f, grHighMin = 0.0f;

    for (int i = 0; i < chunk; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const T in = chPtr[ch][i];

            // ---- 频段拆分（滤波器作用于未延迟信号，供检测器使用） ----
            const T low  = lowLP[ch].processSample (in);
            const T midH = midHP[ch].processSample (in);
            const T mid  = midLP[ch].processSample (midH);
            const T high = highHP[ch].processSample (in);

            T out;
            if (splitMode)
            {
                const T dLow  = dLow[ch].pushAndPop (low);
                const T dMid  = dMid[ch].pushAndPop (mid);
                const T dHigh = dHigh[ch].pushAndPop (high);

                T gMid = T (1), gHigh = T (1);
                if (lowEn)
                {
                    gMid = (T) detMid[ch].processSample ((double) std::abs (mid), lowThr, strength);
                    grMidMin = juce::jmin (grMidMin, juce::Decibels::gainToDecibels ((float) gMid));
                }
                if (highEn)
                {
                    gHigh = (T) detHigh[ch].processSample ((double) std::abs (high), highThr, strength);
                    grHighMin = juce::jmin (grHighMin, juce::Decibels::gainToDecibels ((float) gHigh));
                }
                out = dLow + dMid * gMid + dHigh * gHigh;
            }
            else // 宽带模式：检测取中/高段较大包络，整路统一衰减（模拟老式宽带 DeEsser）
            {
                const T d = dMid[ch].pushAndPop (in);
                const double det = (double) juce::jmax (std::abs (mid), std::abs (high));
                const T g = (T) detWide[ch].processSample (det, lowThr, strength);
                grMidMin = juce::jmin (grMidMin, juce::Decibels::gainToDecibels ((float) g));
                out = d * g;
            }
            chPtr[ch][i] = out;
        }
    }

    // 超采样：降频
    if (oversamplerFactor > 1)
    {
        auto outBlock = juce::dsp::AudioBlock<T> (buffer);
        oversX<T>().processSamplesDown (outBlock);
    }

    grLowDb  = grMidMin;
    grHighDb = grHighMin;
    updateLatency (lookSamples);
}

// 按精度取滤波器组 / 延迟线 / 超采样器的辅助函数
template <> inline auto& HanChiAudioProcessor::lowLP_X<float>()  { return lowLP_F; }
template <> inline auto& HanChiAudioProcessor::lowLP_X<double>() { return lowLP_D; }
template <> inline auto& HanChiAudioProcessor::midHP_X<float>()  { return midHP_F; }
template <> inline auto& HanChiAudioProcessor::midHP_X<double>() { return midHP_D; }
template <> inline auto& HanChiAudioProcessor::midLP_X<float>()  { return midLP_F; }
template <> inline auto& HanChiAudioProcessor::midLP_X<double>() { return midLP_D; }
template <> inline auto& HanChiAudioProcessor::highHP_X<float>()  { return highHP_F; }
template <> inline auto& HanChiAudioProcessor::highHP_X<double>() { return highHP_D; }
template <> inline auto& HanChiAudioProcessor::dLowX<float>()  { return dLowF; }
template <> inline auto& HanChiAudioProcessor::dLowX<double>() { return dLowD; }
template <> inline auto& HanChiAudioProcessor::dMidX<float>()  { return dMidF; }
template <> inline auto& HanChiAudioProcessor::dMidX<double>() { return dMidD; }
template <> inline auto& HanChiAudioProcessor::dHighX<float>()  { return dHighF; }
template <> inline auto& HanChiAudioProcessor::dHighX<double>() { return dHighD; }
template <> inline auto& HanChiAudioProcessor::oversX<float>()  { return *oversF; }
template <> inline auto& HanChiAudioProcessor::oversX<double>() { return *oversD; }

template void HanChiAudioProcessor::processInternal<float>  (juce::AudioBuffer<float>&);
template void HanChiAudioProcessor::processInternal<double> (juce::AudioBuffer<double>&);

//==============================================================================
// 状态保存 / 恢复
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
// 模板系统
juce::File HanChiAudioProcessor::getUserTemplateDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("HanChi").getChildFile ("Templates");
    dir.createDirectoryIfMissing();
    return dir;
}

juce::StringArray HanChiAudioProcessor::getBuiltinTemplateNames()
{
    return { "男声标准", "女声标准", "齿音偏重", "女声清亮",
             "说唱强压", "民谣古风轻手", "配音广播级", "宽带模拟老 DeEsser" };
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
    if (name == "男声标准")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6000},{pLowThr,-30},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-32},{pStrength,55},{pLookahead,3} });
    else if (name == "女声标准")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6500},{pLowThr,-26},
                            {pHighEn,1},{pHighFreq,10000},{pHighThr,-28},{pStrength,60},{pLookahead,3} });
    else if (name == "齿音偏重")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,5500},{pLowThr,-22},
                            {pHighEn,1},{pHighFreq,9000},{pHighThr,-24},{pStrength,80},{pLookahead,4} });
    else if (name == "女声清亮")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,7000},{pLowThr,-28},
                            {pHighEn,1},{pHighFreq,11000},{pHighThr,-26},{pStrength,50},{pLookahead,3} });
    else if (name == "说唱强压")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,5000},{pLowThr,-20},
                            {pHighEn,1},{pHighFreq,8500},{pHighThr,-22},{pStrength,90},{pLookahead,5} });
    else if (name == "民谣古风轻手")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6500},{pLowThr,-34},
                            {pHighEn,1},{pHighFreq,10000},{pHighThr,-36},{pStrength,35},{pLookahead,2} });
    else if (name == "配音广播级")
        applyParamValues ({ {pMode,1},{pLowEn,1},{pLowFreq,6000},{pLowThr,-24},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-25},{pStrength,75},{pLookahead,4} });
    else if (name == "宽带模拟老 DeEsser")
        applyParamValues ({ {pMode,0},{pLowEn,1},{pLowFreq,6000},{pLowThr,-30},
                            {pHighEn,1},{pHighFreq,9500},{pHighThr,-32},{pStrength,60},{pLookahead,2} });
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
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HanChiAudioProcessor();
}
