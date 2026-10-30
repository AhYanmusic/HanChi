#pragma once

#include <JuceHeader.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <type_traits>
#include <vector>

//==============================================================================
/** HanChi 汉齿 · 中文齿音 VST3 插件（v1.4）
 *
 *  双频段动态去齿音：低区 4-9kHz（嘶/嘘，s x c z）+ 高区 7-13kHz（咝/刺感），
 *  Linkwitz-Riley（两极级联 Butterworth Q=0.7071）4 阶分频；
 *  宽带 / 分频段两种模式；前瞻 Lookahead 0-10ms；
 *  每个频段独立控制：阈值 / 压缩比 / 强度 / 单独 Solo 试听；
 *  内部精度 32/64-bit 浮点可选；1x-8x 多相半带超采样（按真实通道数构建、并在超采样率下重算分频与检测）；
 *  8 套中文人声模板 + 用户模板（存为 / 载入 / 删除）。
 */
class HanChiAudioProcessor : public juce::AudioProcessor,
                             private juce::AudioProcessorValueTreeState::Listener
{
public:
    HanChiAudioProcessor();
    ~HanChiAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }
    const juce::String getName() const override            { return "HanChi"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //------ 模板系统 -------------------------------------------------------
    static juce::StringArray getBuiltinTemplateNames();
    juce::StringArray getUserTemplateNames() const;
    void applyBuiltinTemplate (const juce::String& name);
    bool applyUserTemplate (const juce::String& name);   // false = 不存在/解析失败
    bool saveUserTemplate (const juce::String& name);    // false = 名字非法
    bool deleteUserTemplate (const juce::String& name);
    static juce::File getUserTemplateDir();

    //------ 供编辑器读取的增益削减表（dB，0 或负值） ------------------------
    std::atomic<float> grLowDb  { 0.0f };   // 低区（中频段）GR
    std::atomic<float> grHighDb { 0.0f };   // 高区 GR

    juce::AudioProcessorValueTreeState apvts;

    // 供编辑器创建参数布局（也用于模板对照）
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    template <typename T> struct Chain;   // 前置声明，定义见下方
    //======================================================================
    void parameterChanged (const juce::String& id, float newValue) override;
    void rebuildOversamplersIfNeeded();
    void rebuildBiquads();
    void prepareDetectors (double fs);
    void updateLatency (int lookSamples);
    void applyParamValues (const std::map<juce::String, float>& values);

    template <typename T>
    void processInternal (juce::AudioBuffer<T>& buffer);

    /** 在指定精度的信号链上完成实际 DSP */
    template <typename T>
    void processChain (Chain<T>& chain, juce::AudioBuffer<T>& buffer);

    //======================================================================
    /** 两极级联 Butterworth（Q=0.7071）= Linkwitz-Riley 4 阶 */
    template <typename T>
    struct Biquad
    {
        std::array<juce::dsp::IIR::Filter<T>, 2> stages;

        void set (double fs, double fc, bool isLowPass)
        {
            using C = juce::dsp::IIR::Coefficients<T>;
            for (auto& f : stages)
            {
                f.coefficients = isLowPass ? C::makeLowPass  (fs, fc, T (0.70710678))
                                           : C::makeHighPass (fs, fc, T (0.70710678));
                f.reset();
            }
        }
        T processSample (T in)
        {
            in = stages[0].processSample (in);
            return stages[1].processSample (in);
        }
    };

    /** 峰值包络检测 + 可设比例硬压缩（满强度时最大削减 20 dB）+ 攻/放平滑
     *  ratio    : 压缩比（如 8 = 8:1），每频段独立
     *  strength01: 强度 0..1，线性缩放最大削减上限（满强度 = 20 dB）
     */
    template <typename T>
    struct Detector
    {
        T env = T (0), gain = T (1);
        T atk = T (0), rel = T (0), envRel = T (0);

        // 仅更新时基常数（超采样率变化时不重置包络状态，避免爆音）
        void setTimeConstants (double fs)
        {
            envRel = (T) std::exp (-1.0 / (fs * 0.030));   // 包络释放 30ms
            atk    = (T) std::exp (-1.0 / (fs * 0.0005));  // 增益攻击 0.5ms
            rel    = (T) std::exp (-1.0 / (fs * 0.080));   // 增益释放 80ms
        }
        void reset()   { env = T (0); gain = T (1); }
        void prepare (double fs) { setTimeConstants (fs); reset(); }

        T processSample (T absIn, T thresholdDb, T strength01, T ratio)
        {
            env = absIn > env ? absIn : env * envRel;
            const T envDb  = juce::Decibels::gainToDecibels (env, T (-120));
            const T over   = envDb - thresholdDb;
            T target = T (1);
            if (over > T (0))
            {
                const T ratioF = ratio > T (1) ? ratio : T (1.0001);
                // 去齿音需要硬比例：削减 = 超出量 × (1 - 1/ratio)
                const T grRaw = over * (T (1) - T (1) / ratioF);
                // 满强度(100%)时最大削减 20 dB；强度旋钮线性缩放该上限
                const T maxGr = T (20) * strength01;
                const T grDb  = -juce::jmin (grRaw, maxGr);
                target = juce::Decibels::decibelsToGain (grDb, T (-120));
            }
            gain = target < gain ? gain + atk * (target - gain)
                                 : gain + rel * (target - gain);
            return gain;
        }
    };

    /** 简单环形延迟线（前瞻） */
    template <typename T>
    struct DelayLine
    {
        std::vector<T> data;
        int pos = 0;

        void setSize (int s)
        {
            if (s < 0) s = 0;
            if ((int) data.size() != s)
            {
                data.assign ((size_t) s, T (0));
                pos = 0;
            }
        }
        void clear() { std::fill (data.begin(), data.end(), T (0)); }
        T pushAndPop (T in)
        {
            if (data.empty()) return in;
            const T out = data[(size_t) pos];
            data[(size_t) pos] = in;
            if (++pos >= (int) data.size()) pos = 0;
            return out;
        }
    };

    //======================================================================
    /** 一档精度（float 或 double）下的完整信号链 */
    template <typename T>
    struct Chain
    {
        Biquad<T>    lowLP[2], midHP[2], midLP[2], highHP[2];
        DelayLine<T> dLow[2], dMid[2], dHigh[2];
        std::unique_ptr<juce::dsp::Oversampling<T>> overs;
        juce::AudioBuffer<T> osBuf;   // 超采样工作缓冲：容量 = 最大块长 × 8，固定 2 通道

        void prepareFilters (double fs, double lowFreq, double highFreq)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                lowLP[ch].set  (fs, lowFreq,  true);
                midHP[ch].set  (fs, lowFreq,  false);
                midLP[ch].set  (fs, highFreq, true);
                highHP[ch].set (fs, highFreq, false);
            }
        }
    };

    std::atomic<int>  oversamplerFactor  { 1 };
    std::atomic<bool> oversamplerDirty   { true };
    std::atomic<bool> filtersDirty       { true };

    double currentSampleRate = 48000.0;
    int    currentBlockSize  = 512;
    int    lastLatencySamples = 0;     // 上次上报的延迟，避免每 block 重复 setLatencySamples

    // 检测器（double 运算，两条路径共用，只有一条路径在同一时间运行）
    Detector<double> detMid[2], detHigh[2], detWide[2];

    Chain<float>  chainF;
    Chain<double> chainD;

    juce::AudioBuffer<float>  scratchF;
    juce::AudioBuffer<double> scratchD;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HanChiAudioProcessor)
};
