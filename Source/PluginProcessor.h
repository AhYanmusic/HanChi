#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <map>
#include <vector>

//==============================================================================
/** HanChi 汉齿 · 中文齿音 VST3 插件（v1.1）
 *
 *  双频段动态去齿音：低区 4-9kHz（嘶/嘘，s x c z）+ 高区 7-13kHz（咝/刺感），
 *  Linkwitz-Riley（两极级联 Butterworth Q=0.7071）4 阶分频；
 *  宽带 / 分频段两种模式；前瞻 Lookahead 0-10ms；强度 0-100%；
 *  内部精度 32/64-bit 浮点可选；1x-8x 多相半带超采样；
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
    //======================================================================
    void parameterChanged (const juce::String& id, float newValue) override;
    void rebuildOversamplersIfNeeded();
    void rebuildBiquads();
    void updateLatency (int lookSamples);
    void applyParamValues (const std::map<juce::String, float>& values);

    // 按内部精度取对应滤波器组 / 延迟线 / 超采样器（特化在 .cpp 中）
    template <typename T> auto& lowLP_X();
    template <typename T> auto& midHP_X();
    template <typename T> auto& midLP_X();
    template <typename T> auto& highHP_X();
    template <typename T> auto& dLowX();
    template <typename T> auto& dMidX();
    template <typename T> auto& dHighX();
    template <typename T> auto& oversX();

    template <typename T>
    void processInternal (juce::AudioBuffer<T>& buffer);

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

    /** 峰值包络检测 + 3:1 软压缩 + 攻/放平滑 */
    template <typename T>
    struct Detector
    {
        T env = T (0), gain = T (1);
        T atk = T (0), rel = T (0), envRel = T (0);

        void prepare (double fs)
        {
            envRel = (T) std::exp (-1.0 / (fs * 0.030));   // 包络释放 30ms
            atk    = (T) std::exp (-1.0 / (fs * 0.0005));  // 增益攻击 0.5ms
            rel    = (T) std::exp (-1.0 / (fs * 0.080));   // 增益释放 80ms
            env = T (0); gain = T (1);
        }
        T processSample (T absIn, T thresholdDb, T strength01)
        {
            env = absIn > env ? absIn : env * envRel;
            const T envDb  = juce::Decibels::gainToDecibels (env, T (-120));
            const T over   = envDb - thresholdDb;
            T target = T (1);
            if (over > T (0))
            {
                const T grDb = -over * (T (1) - T (1) / T (3)) * strength01; // 3:1
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
    std::atomic<int>  oversamplerFactor { 1 };
    std::atomic<bool> oversamplerDirty  { true };
    std::atomic<bool> filtersDirty      { true };

    std::unique_ptr<juce::dsp::Oversampling<float>>  oversF;
    std::unique_ptr<juce::dsp::Oversampling<double>> oversD;

    double currentSampleRate = 48000.0;
    int    currentBlockSize  = 512;

    // float 路径滤波器组
    Biquad<float>  lowLP_F[2], midHP_F[2], midLP_F[2], highHP_F[2];
    // double 路径滤波器组
    Biquad<double> lowLP_D[2], midHP_D[2], midLP_D[2], highHP_D[2];

    // 检测器（double 运算，两条路径共用，只有一条路径在同一时间运行）
    Detector<double> detMid[2], detHigh[2], detWide[2];

    // 前瞻延迟线：低 / 中 / 高三段独立延迟（保证分频求和相位一致）
    DelayLine<float>  dLowF[2], dMidF[2], dHighF[2];
    DelayLine<double> dLowD[2], dMidD[2], dHighD[2];

    juce::AudioBuffer<float>  scratchF;
    juce::AudioBuffer<double> scratchD;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HanChiAudioProcessor)
};
