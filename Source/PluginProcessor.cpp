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
static const juce::String pLowStr    { "lowStrength" };
static const juce::String pHighStr   { "highStrength" };
static const juce::String pLowRatio  { "lowRatio" };
static const juce::String pHighRatio { "highRatio" };
static const juce::String pLowSolo   { "lowSolo" };
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

    for (int ch = 0; ch < 2; ++ch)
    {
        detMid[ch].prepare  (sampleRate);
        detHigh[ch].prepare (sampleRate);
        detWide[ch].prepare (sampleRate);
    }

    filtersDirty     = true;
    oversamplerDirty = true;
    // 在最后重建：会根据当前超采样率覆盖检测器时基，使分频/检测与超采样率一致
    rebuildOversamplersIfNeeded();

    // 超采样工作缓冲：2 通道 ×（最大块长 × 8），保证 8x 升频时有足够容量
    const int osCap = samplesPerBlock * 8;
    chainF.osBuf.setSize (2, osCap, false, false, true);
    chainD.osBuf.setSize (2, osCap, false, false, true);

    scratchF.setSize (2, samplesPerBlock, false, false, true);
    scratchD.setSize (2, samplesPerBlock, false, false, true);
}

void HanChiAudioProcessor::rebuildOversamplersIfNeeded()
{
    const int comboIdx = (int) apvts.getRawParameterValue (pOs)->load(); // 0..3 -> 1x,2x,4x,8x
    const size_t factor = (size_t) 1 << juce::jlimit (0, 3, comboIdx);

    // 仅在「倍率确实变化 / 脏标记」时重建，避免每 block 重分配
    if (! oversamplerDirty.exchange (false)
        && chainF.overs != nullptr && chainD.overs != nullptr
        && (int) oversamplerFactor.load() == (int) factor)
        return;

    // 固定按 2 通道（立体声）构建，与 osBuf 通道数严格一致，
    // processSamplesUp/Down 通道匹配，杜绝 2/4/8x 静音。
    chainF.overs = std::make_unique<juce::dsp::Oversampling<float>> (2, factor,
                   juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    chainD.overs = std::make_unique<juce::dsp::Oversampling<double>> (2, factor,
                   juce::dsp::Oversampling<double>::filterHalfBandPolyphaseIIR, true);

    chainF.overs->initProcessing ((size_t) currentBlockSize);
    chainD.overs->initProcessing ((size_t) currentBlockSize);
    chainF.overs->reset();
    chainD.overs->reset();

    oversamplerFactor = (int) factor;

    // 在超采样率上重设检测器时间常数（与分频器保持一致，频率/时基才正确）
    prepareDetectors (currentSampleRate * (double) factor);
    filtersDirty = true;   // 分频器需在超采样率下重算（见 processChain）
    updateLatency (0);
}

void HanChiAudioProcessor::prepareDetectors (double fs)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        detMid[ch].setTimeConstants (fs);
        detHigh[ch].setTimeConstants (fs);
        detWide[ch].setTimeConstants (fs);
    }
}

void HanChiAudioProcessor::rebuildBiquads()
{
    // 关键修复：分频器系数必须在「超采样率」下计算，否则 2/4/8x 时截止频率成倍偏低。
    const double fs = currentSampleRate * (double) oversamplerFactor.load();
    const double lf = apvts.getRawParameterValue (pLowFreq)->load();
    const double hf = apvts.getRawParameterValue (pHighFreq)->load();

    chainF.prepareFilters (fs, lf, hf);
    chainD.prepareFilters (fs, lf, hf);
    filtersDirty = false;
}

void HanChiAudioProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == pOs)
    {
        oversamplerDirty = true;   // 重建超采样器时一并重算检测器/分频器
    }
    if (id == pLowFreq || id == pHighFreq) filtersDirty = true;
}

void HanChiAudioProcessor::updateLatency (int lookSamples)
{
    double osLatency = 0.0;
    if (oversamplerFactor.load() > 1 && chainF.overs != nullptr)
        osLatency = chainF.overs->getLatencyInSamples();
    const int total = juce::roundToInt ((double) lookSamples + osLatency);
    if (total != lastLatencySamples)
    {
        lastLatencySamples = total;
        setLatencySamples (total);
    }
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
    if constexpr (std::is_same_v<T, float>)
        processChain (chainF, buffer);
    else
        processChain (chainD, buffer);
}

template <typename T>
void HanChiAudioProcessor::processChain (Chain<T>& c, juce::AudioBuffer<T>& buffer)
{
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0) return;

    const int factor = juce::jmax (1, (int) oversamplerFactor.load());

    const bool splitMode  = apvts.getRawParameterValue (pMode)->load() > 0.5f;
    const bool lowEn      = apvts.getRawParameterValue (pLowEn)->load() > 0.5f;
    const bool highEn     = apvts.getRawParameterValue (pHighEn)->load() > 0.5f;
    const double lowThr   = (double) apvts.getRawParameterValue (pLowThr)->load();
    const double highThr  = (double) apvts.getRawParameterValue (pHighThr)->load();
    const double lowStr   = (double) apvts.getRawParameterValue (pLowStr)->load() * 0.01;
    const double highStr  = (double) apvts.getRawParameterValue (pHighStr)->load() * 0.01;
    const double lowRatio = (double) apvts.getRawParameterValue (pLowRatio)->load();
    const double highRatio= (double) apvts.getRawParameterValue (pHighRatio)->load();
    const bool lowSolo    = apvts.getRawParameterValue (pLowSolo)->load() > 0.5f;
    const bool highSolo   = apvts.getRawParameterValue (pHighSolo)->load() > 0.5f;
    // 前瞻按超采样率换算，听感上的延迟时间保持一致
    const int lookSamples = (int) std::round ((double) apvts.getRawParameterValue (pLookahead)->load()
                                              * 0.001 * currentSampleRate * (double) factor);

    // 前瞻延迟线尺寸（低 / 中 / 高三段一致，保证求和相位对齐）
    for (int ch = 0; ch < 2; ++ch)
    {
        c.dLow[ch].setSize (lookSamples);
        c.dMid[ch].setSize (lookSamples);
        c.dHigh[ch].setSize (lookSamples);
    }

    // 超采样仅在「倍率>1 且缓冲区容量足够」时启用；否则回退到直接处理（绝不静音）
    const bool useOS = (factor > 1) && (c.overs != nullptr)
                       && (n <= currentBlockSize)
                       && ((long long) n * (long long) factor <= (long long) c.osBuf.getNumSamples());

    T* chPtr[2] { nullptr, nullptr };
    int chunk = 0;
    juce::dsp::AudioBlock<T> osUpBlock;   // 升频后的 block（降频时复用，长度 = n*factor）

    if (useOS)
    {
        c.osBuf.clear();
        for (int ch = 0; ch < numCh; ++ch)
            c.osBuf.copyFrom (ch, 0, buffer, ch, 0, n);

        auto fullBlock = juce::dsp::AudioBlock<T> (c.osBuf);
        auto inBlock   = fullBlock.getSubBlock (0, (size_t) n);
        osUpBlock      = c.overs->processSamplesUp (inBlock);   // 就地升频，长度变为 n*factor
        chunk = (int) osUpBlock.getNumSamples();
        for (int ch = 0; ch < numCh; ++ch)
            chPtr[ch] = osUpBlock.getChannelPointer (ch);
    }
    else
    {
        chunk = n;
        for (int ch = 0; ch < numCh; ++ch)
            chPtr[ch] = buffer.getWritePointer (ch);
    }

    float grMidMin = 0.0f, grHighMin = 0.0f;

    for (int i = 0; i < chunk; ++i)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            const T in = chPtr[ch][i];

            // ---- 频段拆分（滤波器作用于未延迟信号，供检测器使用） ----
            const T low  = c.lowLP[ch].processSample (in);
            const T midH = c.midHP[ch].processSample (in);
            const T mid  = c.midLP[ch].processSample (midH);
            const T high = c.highHP[ch].processSample (in);

            T out;
            if (splitMode)
            {
                const T dLow  = c.dLow[ch].pushAndPop (low);
                const T dMid  = c.dMid[ch].pushAndPop (mid);
                const T dHigh = c.dHigh[ch].pushAndPop (high);

                T gMid = T (1), gHigh = T (1);
                if (lowEn)
                {
                    gMid = (T) detMid[ch].processSample ((double) std::abs (mid),  lowThr,  lowStr,  lowRatio);
                    grMidMin = juce::jmin (grMidMin, juce::Decibels::gainToDecibels ((float) gMid));
                }
                if (highEn)
                {
                    gHigh = (T) detHigh[ch].processSample ((double) std::abs (high), highThr, highStr, highRatio);
                    grHighMin = juce::jmin (grHighMin, juce::Decibels::gainToDecibels ((float) gHigh));
                }

                // Solo：只听被选中的频段（未被选中的频段静音，便于单独检查该段去齿音）
                const T lowPart  = dLow + dMid * gMid;
                const T highPart = dHigh * gHigh;
                if (lowSolo && !highSolo)       out = lowPart;
                else if (highSolo && !lowSolo)  out = highPart;
                else                            out = lowPart + highPart;
            }
            else // 宽带模式：检测取中/高段较大包络，整路统一衰减（模拟老式宽带 DeEsser）
            {
                const T d = c.dMid[ch].pushAndPop (in);
                const double det = (double) juce::jmax (std::abs (mid), std::abs (high));
                // 宽带模式用两段的平均比例/强度
                const T g = (T) detWide[ch].processSample (det, lowThr,
                                                          (lowStr + highStr) * 0.5,
                                                          (lowRatio + highRatio) * 0.5);
                grMidMin = juce::jmin (grMidMin, juce::Decibels::gainToDecibels ((float) g));
                out = d * g;
            }
            chPtr[ch][i] = out;
        }
    }

    if (useOS)
    {
        // 就地降频：结果写回 osBuf 的前 n 个样本
        c.overs->processSamplesDown (osUpBlock);
        for (int ch = 0; ch < numCh; ++ch)
            buffer.copyFrom (ch, 0, c.osBuf, ch, 0, n);
    }

    grLowDb  = grMidMin;
    grHighDb = grHighMin;
    updateLatency (lookSamples);
}

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
