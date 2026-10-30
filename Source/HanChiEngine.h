#pragma once

// ============================================================================
//  HanChi Engine - pure DSP core (header only).
//  Shared by the VST3 plugin and by the CI self-test program, so that the
//  audio math can be verified in the cloud before the plugin ever ships.
//
//  Design rules (these are what fixed the previous silence / crash reports):
//   1. Oversamplers for 2x / 4x / 8x are ALL built once inside prepare().
//      process() never allocates, never rebuilds a filter bank.
//   2. processSamplesUp() returns a VIEW of the oversampler internal buffer.
//      The caller MUST process in place on that returned block, and MUST pass
//      the ORIGINAL-SIZE block (not the upsampled one) to processSamplesDown().
//      Passing the upsampled block makes JUCE index its internal buffers at
//      n * factor^2, which reads/writes out of bounds -> silence and crashes.
//   3. The reported latency is CONSTANT for the whole lifetime of prepare().
//      A trim delay line on the output pads the real processing delay up to
//      that constant, so the host never sees the latency change -> no crash
//      when the user switches oversampling while the transport is running.
//   4. Everything is fixed 2-channel internally; mono hosts duplicate ch0.
// ============================================================================

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace hanchi
{

// ============================================================================
//  Parameters snapshot (plain data, no host dependency)
// ============================================================================
struct Params
{
    bool   splitMode = true;

    bool   lowEn     = true;
    double lowFreq   = 6000.0;
    double lowThr    = -30.0;
    double lowRatio  = 8.0;
    double lowStr    = 0.6;      // 0..1
    bool   lowSolo   = false;

    bool   highEn    = true;
    double highFreq  = 9500.0;
    double highThr   = -32.0;
    double highRatio = 8.0;
    double highStr   = 0.6;      // 0..1
    bool   highSolo  = false;

    double lookMs    = 3.0;      // 0..10 ms
    int    osFactor  = 1;        // 1, 2, 4 or 8
};

// ============================================================================
//  Topology-preserving-transform state variable filter (TPT SVF), 2nd order.
//
//  Chosen over a direct-form biquad on purpose: at 8x oversampling the 6 kHz
//  crossover point sits at fc/fs = 0.0156, where direct-form coefficients
//  lose accuracy and the phase response (which the band-summing relies on)
//  drifts. The TPT structure stays well conditioned at any fc/fs ratio, which
//  is why every top-tier dynamic EQ / de-esser uses it.
//
//  One instance produces the lowpass AND the highpass output from the same
//  state, so the two paths are phase coherent - another requirement for the
//  crossover to sum back to the original signal.
// ============================================================================
template <typename T>
struct TptSvf
{
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 0.0;
    T ic1 = T (0), ic2 = T (0);

    // retained for diagnostics
    double designFs = 0.0, designFc = 0.0, gCoef = 0.0;

    void set (double fs, double fc, double q)
    {
        const double f = juce::jlimit (10.0, fs * 0.45, fc);
        const double g = std::tan (3.14159265358979323846 * f / fs);

        designFs = fs; designFc = f; gCoef = g;

        k  = 1.0 / juce::jmax (0.05, q);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() { ic1 = T (0); ic2 = T (0); }

    /** Returns the lowpass output and writes the highpass output to hpOut. */
    T process (T v0, T& hpOut)
    {
        const T v3 = v0 - ic2;
        const T v1 = (T) (a1 * (double) ic1 + a2 * (double) v3);
        const T v2 = (T) ((double) ic2 + a2 * (double) ic1 + a3 * (double) v3);

        ic1 = (T) (2.0 * (double) v1) - ic1;
        ic2 = (T) (2.0 * (double) v2) - ic2;

        hpOut = v0 - (T) (k * (double) v1) - v2;
        return v2;
    }

    T processLP (T v0) { T hp; return process (v0, hp); }
    T processHP (T v0) { T hp; process (v0, hp); return hp; }
};

// ============================================================================
//  3-way crossover built from complementary lowpass differences.
//
//      low  = LP4(f1)
//      mid  = LP4(f2) - LP4(f1)
//      high = in - LP4(f2)
//
//  Sum:  low + mid + high = LP4(f1) + LP4(f2) - LP4(f1) + in - LP4(f2) = in
//
//  This is an IDENTICAL reconstruction - sample for sample, at any sample rate,
//  with any filter - unlike a serial Linkwitz-Riley tree. In a serial tree the
//  second split contributes an allpass rather than an identity, so the bands do
//  not sum back and, worse, "mid + high" ends up roughly 75 degrees away from
//  the input. Subtracting a vector at that angle barely changes the magnitude,
//  which is how a de-esser shows 20 dB of gain reduction yet changes nothing.
//
//  With this structure:   out = g * in + (1 - g) * low
//  Since "low" is heavily attenuated up in the sibilance region, the reduction
//  always lands, and zero gain reduction is a bit-exact bypass.
// ============================================================================
template <typename T>
struct Crossover
{
    static constexpr double q = 0.7071067811865476;   // Butterworth 2nd order

    TptSvf<T> l1a, l1b;   // LP4 at f1
    TptSvf<T> l2a, l2b;   // LP4 at f2

    double setFs = 0.0, setF1 = 0.0, setF2 = 0.0;

    void set (double fs, double f1, double f2)
    {
        setFs = fs; setF1 = f1; setF2 = f2;
        l1a.set (fs, f1, q);  l1b.set (fs, f1, q);
        l2a.set (fs, f2, q);  l2b.set (fs, f2, q);
    }

    void reset()
    {
        l1a.reset(); l1b.reset();
        l2a.reset(); l2b.reset();
    }

    struct Out { T low, mid, high; };

    Out process (T in)
    {
        const T low = l1b.processLP (l1a.processLP (in));   // LP4(f1)
        const T lp2 = l2b.processLP (l2a.processLP (in));   // LP4(f2)

        const T mid  = lp2 - low;       // band between f1 and f2
        const T high = in - lp2;        // everything above f2

        return { low, mid, high };
    }
};

// ============================================================================
//  Peak-envelope detector + compressor
//  Detection always runs in double precision for maximum headroom accuracy.
// ============================================================================
struct Detector
{
    double env    = 0.0;
    double gain   = 1.0;
    double atk    = 0.0;
    double rel    = 0.0;
    double envRel = 0.0;

    void setTimeConstants (double fs)
    {
        const double s = juce::jmax (1000.0, fs);
        envRel = std::exp (-1.0 / (s * 0.030));   // envelope release 30 ms
        atk    = std::exp (-1.0 / (s * 0.0005));  // gain attack 0.5 ms
        rel    = std::exp (-1.0 / (s * 0.080));   // gain release 80 ms
    }

    void reset() { env = 0.0; gain = 1.0; }

    void prepare (double fs) { setTimeConstants (fs); reset(); }

    /** absIn = rectified band signal, thresholdDb, strength 0..1, ratio >= 1.5 */
    double processSample (double absIn, double thresholdDb, double strength01, double ratio)
    {
        env = (absIn > env) ? absIn : env * envRel;

        const double envDb = juce::Decibels::gainToDecibels (env, -200.0);
        const double over  = envDb - thresholdDb;

        double target = 1.0;

        if (over > 0.0)
        {
            const double r     = juce::jmax (1.0001, ratio);
            const double grRaw = over * (1.0 - 1.0 / r);
            const double maxGr = 20.0 * juce::jlimit (0.0, 1.0, strength01);
            const double grDb  = -juce::jmin (grRaw, maxGr);
            target = juce::Decibels::decibelsToGain (grDb, -200.0);
        }

        gain = (target < gain) ? gain + atk * (target - gain)
                               : gain + rel * (target - gain);

        return gain;
    }
};

// ============================================================================
//  Variable delay line - size is allocated ONCE at prepare(); the delay amount
//  can then change every block without any allocation on the audio thread.
// ============================================================================
template <typename T>
struct VarDelay
{
    std::vector<T> buf;
    int size = 1;
    int w    = 0;

    void prepare (int maxDelay)
    {
        size = juce::jmax (1, maxDelay + 2);
        buf.assign ((size_t) size, T (0));
        w = 0;
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), T (0));
        w = 0;
    }

    int getMaxDelay() const noexcept { return size - 1; }

    T process (T in, int delay)
    {
        const int d = juce::jlimit (0, size - 1, delay);

        buf[(size_t) w] = in;

        int r = w - d;
        if (r < 0)
            r += size;

        const T out = buf[(size_t) r];

        if (++w >= size)
            w = 0;

        return out;
    }
};

// ============================================================================
//  Lightweight diagnostics - used by the CI self-test to see the real value of
//  every stage. Accumulation is a handful of adds per sample.
// ============================================================================
struct Diagnostics
{
    double inSq = 0.0, lowSq = 0.0, midSq = 0.0, highSq = 0.0, outSq = 0.0;
    double gLowMin = 1.0, gLowMax = 0.0;
    double gHighMin = 1.0, gHighMax = 0.0;
    long long n = 0;

    void reset() { *this = Diagnostics {}; }
};

// ============================================================================
//  Engine
// ============================================================================
template <typename T>
class Engine
{
public:
    Engine() = default;

    Diagnostics diag;
    void resetDiag() { diag.reset(); }

    // ---------------------------------------------------------------- prepare
    void prepare (double sampleRate, int maxBlockSize)
    {
        fs       = juce::jmax (8000.0, sampleRate);
        maxBlock = juce::jmax (1, maxBlockSize);

        // 1x needs no oversampler; 2x / 4x / 8x are built here, once.
        os[0] = nullptr;

        for (int i = 1; i <= 3; ++i)
        {
            const size_t factor = (size_t) (1 << i);   // 2, 4, 8
            os[i] = std::make_unique<juce::dsp::Oversampling<T>> (
                        (size_t) 2, factor,
                        juce::dsp::Oversampling<T>::filterHalfBandPolyphaseIIR,
                        false);
            os[i]->initProcessing ((size_t) maxBlock);
            os[i]->reset();
        }

        osLatency[0] = 0;
        for (int i = 1; i <= 3; ++i)
            osLatency[i] = juce::jmin (256, (int) os[i]->getLatencyInSamples());

        // Lookahead: 10 ms is the parameter maximum.
        lookOrigMax = (int) std::ceil (10.0 * 0.001 * fs) + 2;
        lookMax     = lookOrigMax * 8 + 4;   // same delay expressed at 8x rate

        // Constant latency reported to the host.
        int maxOsLat = 0;
        for (int i = 0; i < 4; ++i)
            maxOsLat = juce::jmax (maxOsLat, osLatency[i]);

        fixedLatency = lookOrigMax + maxOsLat + 4;
        trimMax      = fixedLatency + 8;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int b = 0; b < 4; ++b)
                lookDelay[ch][b].prepare (lookMax);

            trim[ch].prepare (trimMax);

            detLow[ch].prepare  (fs);
            detHigh[ch].prepare (fs);
            detWide[ch].prepare (fs);
        }

        osBuf.setSize    (2, maxBlock * 8 + 16, false, false, true);
        inBackup.setSize (2, maxBlock,          false, false, true);

        filtersValid = false;
        detFs        = 0.0;
        prepared     = true;

        reset();
    }

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int b = 0; b < 4; ++b)
                lookDelay[ch][b].reset();

            xover[ch].reset();
            trim[ch].reset();

            detLow[ch].reset();
            detHigh[ch].reset();
            detWide[ch].reset();
        }

        for (int i = 1; i <= 3; ++i)
            if (os[i] != nullptr)
                os[i]->reset();

        if (osBuf.getNumSamples() > 0)     osBuf.clear();
        if (inBackup.getNumSamples() > 0)  inBackup.clear();

        grLowDb = grHighDb = 0.0f;
    }

    /** Constant for the whole prepare() lifetime - safe to report once. */
    int getLatencySamples() const noexcept { return prepared ? fixedLatency : 0; }

    float getGrLowDb()  const noexcept { return grLowDb; }
    float getGrHighDb() const noexcept { return grHighDb; }

    // diagnostics
    double debugSetFs() const noexcept { return xover[0].setFs; }
    double debugSetF1() const noexcept { return xover[0].setF1; }
    double debugSetF2() const noexcept { return xover[0].setF2; }
    double debugG1()    const noexcept { return xover[0].l1a.gCoef; }
    double debugG2()    const noexcept { return xover[0].l2a.gCoef; }

    // ---------------------------------------------------------------- process
    void process (juce::AudioBuffer<T>& buffer, const Params& p)
    {
        const int numCh = juce::jmin (buffer.getNumChannels(), 2);
        const int n     = buffer.getNumSamples();

        if (! prepared || numCh <= 0 || n <= 0)
            return;

        const int nSafe = juce::jmin (n, maxBlock);

        // ---- keep a copy of the input (anti-silence safety net) -----------
        double inEnergy = 0.0;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const T* src = buffer.getReadPointer (ch);
            T* dst       = inBackup.getWritePointer (ch);

            for (int i = 0; i < nSafe; ++i)
            {
                dst[i] = src[i];
                inEnergy += (double) std::abs ((double) src[i]);
            }
        }

        const int factor = (p.osFactor == 8) ? 8
                         : (p.osFactor == 4) ? 4
                         : (p.osFactor == 2) ? 2 : 1;
        const int osIdx  = (factor == 8) ? 3
                         : (factor == 4) ? 2
                         : (factor == 2) ? 1 : 0;

        updateFiltersAndDetectors (p, factor);

        const double fsOs  = fs * (double) factor;
        const int    lookOs   = juce::jlimit (0, lookDelay[0][0].getMaxDelay(),
                                              (int) std::llround (p.lookMs * 0.001 * fsOs));
        const int    lookOrig = juce::jlimit (0, lookOrigMax,
                                              (int) std::llround (p.lookMs * 0.001 * fs));

        const bool useOS = (factor > 1)
                        && (os[osIdx] != nullptr)
                        && (nSafe <= maxBlock)
                        && ((long long) nSafe * (long long) factor
                                <= (long long) osBuf.getNumSamples());

        // ---- copy the host signal into the fixed 2-channel work buffer ----
        osBuf.clear (0, nSafe * factor);

        for (int ch = 0; ch < 2; ++ch)
        {
            const int srcCh = (ch < numCh) ? ch : 0;   // mono -> duplicate ch0
            osBuf.copyFrom (ch, 0, buffer, srcCh, 0, nSafe);
        }

        // ---- upsample ------------------------------------------------------
        juce::dsp::AudioBlock<T> inBlock;
        T* ptr[2] = { nullptr, nullptr };
        int chunk = nSafe;

        if (useOS)
        {
            auto full = juce::dsp::AudioBlock<T> (osBuf);
            inBlock   = full.getSubBlock (0, (size_t) nSafe);

            juce::dsp::AudioBlock<const T> constIn (inBlock);
            auto up = os[osIdx]->processSamplesUp (constIn);

            // The processing length is EXACTLY nSafe * factor - the number of
            // samples the oversampler produced. Never derive it from the work
            // buffer capacity: osBuf is intentionally over-allocated, and using
            // that larger size here writes past the end of the oversampler's
            // internal buffer, corrupting neighbouring filter state every
            // block (which silently wrecks the crossover response).
            const int want = nSafe * factor;

            if (up.getNumSamples() < (size_t) want)
                return;   // defensive: the output is left untouched -> safe

            chunk  = want;
            ptr[0] = up.getChannelPointer (0);
            ptr[1] = up.getChannelPointer (1);
        }
        else
        {
            ptr[0] = osBuf.getWritePointer (0);
            ptr[1] = osBuf.getWritePointer (1);
        }

        if (ptr[0] == nullptr || ptr[1] == nullptr)
            return;   // never expected; bail out without touching the output

        // ---- main sample loop (always 2 channels) --------------------------
        float grMinLow = 0.0f, grMinHigh = 0.0f;

        for (int i = 0; i < chunk; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const T in = ptr[ch][i];

                // ---- band split (filters run on the un-delayed signal) ----
                const auto bands = xover[ch].process (in);
                const T low  = bands.low;
                const T mid  = bands.mid;
                const T high = bands.high;

                T out;

                if (p.splitMode)
                {
                    const T dIn   = lookDelay[ch][0].process (in,   lookOs);
                    const T dLow  = lookDelay[ch][1].process (low,  lookOs);
                    const T dMid  = lookDelay[ch][2].process (mid,  lookOs);
                    const T dHigh = lookDelay[ch][3].process (high, lookOs);

                    T gLow = T (1), gHigh = T (1);

                    if (p.lowEn)
                    {
                        gLow = (T) detLow[ch].processSample ((double) std::abs ((double) mid),
                                                             p.lowThr, p.lowStr, p.lowRatio);
                        grMinLow = juce::jmin (grMinLow,
                                               juce::Decibels::gainToDecibels ((float) gLow, -200.0f));
                    }

                    if (p.highEn)
                    {
                        gHigh = (T) detHigh[ch].processSample ((double) std::abs ((double) high),
                                                               p.highThr, p.highStr, p.highRatio);
                        grMinHigh = juce::jmin (grMinHigh,
                                                juce::Decibels::gainToDecibels ((float) gHigh, -200.0f));
                    }

                    if (p.lowSolo && ! p.highSolo)
                    {
                        out = dLow + dMid * gLow;          // audition everything below f2
                    }
                    else if (p.highSolo && ! p.lowSolo)
                    {
                        out = dHigh * gHigh;               // audition everything above f2
                    }
                    else
                    {
                        // Subtractive processing.
                        //
                        // Start from the untouched (delayed) signal and remove
                        // only the sibilance bands by the amount each detector
                        // asks for:
                        //
                        //     out = in - (1 - gLow) * mid - (1 - gHigh) * high
                        //
                        // A crossover tree never sums back to exactly unity
                        // (the second split contributes an allpass, not an
                        // identity), so the naive "sum the bands" form leaks
                        // energy and cancels out the gain reduction. Working
                        // subtractively means zero gain reduction is an exact
                        // bypass at every oversampling factor, and the amount
                        // removed is always the true band content.
                        out = dIn;

                        if (p.lowEn)   out -= (T (1) - gLow)  * dMid;
                        if (p.highEn)  out -= (T (1) - gHigh) * dHigh;

                        diag.inSq   += (double) dIn   * (double) dIn;
                        diag.lowSq  += (double) dLow  * (double) dLow;
                        diag.midSq  += (double) dMid  * (double) dMid;
                        diag.highSq += (double) dHigh * (double) dHigh;
                        diag.outSq  += (double) out   * (double) out;
                        diag.gLowMin  = juce::jmin (diag.gLowMin,  (double) gLow);
                        diag.gLowMax  = juce::jmax (diag.gLowMax,  (double) gLow);
                        diag.gHighMin = juce::jmin (diag.gHighMin, (double) gHigh);
                        diag.gHighMax = juce::jmax (diag.gHighMax, (double) gHigh);
                        ++diag.n;
                    }
                }
                else
                {
                    // Broadband: one gain applied to the whole signal,
                    // detector fed by the louder of the two sibilance bands.
                    const T d = lookDelay[ch][0].process (in, lookOs);

                    const double det = juce::jmax (std::abs ((double) mid),
                                                   std::abs ((double) high));

                    const T g = (T) detWide[ch].processSample (det, p.lowThr,
                                                               (p.lowStr  + p.highStr)  * 0.5,
                                                               (p.lowRatio + p.highRatio) * 0.5);

                    grMinLow = juce::jmin (grMinLow,
                                           juce::Decibels::gainToDecibels ((float) g, -200.0f));

                    out = d * g;
                }

                ptr[ch][i] = out;
            }
        }

        // ---- downsample ----------------------------------------------------
        // IMPORTANT: pass the ORIGINAL-size block (nSafe samples), never the
        // upsampled one. JUCE treats this as the output size and scales it up
        // internally by each stage factor.
        if (useOS)
            os[osIdx]->processSamplesDown (inBlock);

        // ---- trim to the constant latency, then write back -----------------
        const int trimD = juce::jlimit (0, trim[0].getMaxDelay(),
                                        fixedLatency - lookOrig - osLatency[osIdx]);

        for (int ch = 0; ch < 2; ++ch)
        {
            const T* src = osBuf.getReadPointer (ch);
            T* dst       = (ch < numCh) ? buffer.getWritePointer (ch) : nullptr;

            for (int i = 0; i < nSafe; ++i)
            {
                const T v = trim[ch].process (src[i], trimD);

                if (dst != nullptr)
                    dst[i] = v;
            }
        }

        // ---- anti-silence safety net ---------------------------------------
        // Only ever fires if something below us misbehaves: if the input had
        // signal but the output came out silent or non-finite, pass through.
        if (inEnergy > 1e-6)
        {
            double outEnergy = 0.0;
            bool   bad       = false;

            for (int ch = 0; ch < numCh && ! bad; ++ch)
            {
                const T* src = buffer.getReadPointer (ch);

                for (int i = 0; i < nSafe; ++i)
                {
                    const double v = (double) src[i];

                    if (! std::isfinite (v))
                    {
                        bad = true;
                        break;
                    }

                    outEnergy += std::abs (v);
                }
            }

            if (bad || outEnergy < 1e-9)
            {
                for (int ch = 0; ch < numCh; ++ch)
                    buffer.copyFrom (ch, 0, inBackup, ch, 0, nSafe);
            }
        }

        grLowDb  = grMinLow;
        grHighDb = grMinHigh;
    }

private:
    // ------------------------------------------------------------------------
    void updateFiltersAndDetectors (const Params& p, int factor)
    {
        const double fsOs = fs * (double) factor;

        if (std::abs (detFs - fsOs) > 0.5)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                detLow[ch].setTimeConstants  (fsOs);
                detHigh[ch].setTimeConstants (fsOs);
                detWide[ch].setTimeConstants (fsOs);
            }

            detFs = fsOs;
        }

        if (! filtersValid
            || lastFactor != factor
            || std::abs (lastLowF  - p.lowFreq)  > 0.5
            || std::abs (lastHighF - p.highFreq) > 0.5)
        {
            const double nyq = fsOs * 0.45;
            const double lo  = juce::jlimit (200.0, nyq, p.lowFreq);
            const double hi  = juce::jlimit (lo + 200.0, nyq, p.highFreq);

            for (int ch = 0; ch < 2; ++ch)
                xover[ch].set (fsOs, lo, hi);

            lastFactor  = factor;
            lastLowF    = p.lowFreq;
            lastHighF   = p.highFreq;
            filtersValid = true;
        }
    }

    // ------------------------------------------------------------------------
    double fs = 48000.0;
    int    maxBlock = 512;
    bool   prepared = false;

    std::unique_ptr<juce::dsp::Oversampling<T>> os[4];
    int osLatency[4] = { 0, 0, 0, 0 };

    int lookOrigMax = 482;
    int lookMax     = 3860;
    int fixedLatency = 0;
    int trimMax      = 0;

    Crossover<T> xover[2];

    VarDelay<T> lookDelay[2][4];   // in / low / mid / high, per channel
    VarDelay<T> trim[2];

    Detector detLow[2], detHigh[2], detWide[2];

    juce::AudioBuffer<T> osBuf;
    juce::AudioBuffer<T> inBackup;

    bool   filtersValid = false;
    int    lastFactor   = 0;
    double lastLowF     = 0.0;
    double lastHighF    = 0.0;
    double detFs        = 0.0;

    float grLowDb  = 0.0f;
    float grHighDb = 0.0f;
};

} // namespace hanchi
