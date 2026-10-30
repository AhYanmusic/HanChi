// ============================================================================
//  HanChi DSP self-test.
//
//  Runs the exact same engine code that the plugin ships with and verifies,
//  for every oversampling factor and for both precisions, that:
//    * a signal that should pass unchanged comes out at the same level
//      (this is the check that catches the "plugin is silent" class of bugs);
//    * a sibilant signal is actually attenuated, but not muted;
//    * silence stays silent and nothing ever becomes NaN / Inf;
//    * switching oversampling back and forth mid-stream is stable.
//
//  Exit code 0 = all checks passed. Non-zero = at least one failure.
// ============================================================================

#include "../Source/HanChiEngine.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>

namespace
{

constexpr double kPi = 3.14159265358979323846;

int g_failures = 0;

void check (bool ok, const std::string& name, const std::string& detail)
{
    if (ok)
        std::cout << "  PASS   " << name << "   (" << detail << ")\n";
    else
    {
        std::cout << "  FAIL   " << name << "   (" << detail << ")\n";
        ++g_failures;
    }
}

double toDb (double ratio)
{
    if (ratio <= 0.0)
        return -999.0;
    return 20.0 * std::log10 (ratio);
}

// Returns the RMS of the processed output, or -1.0 if any sample was non-finite.
template <typename T>
double runStream (hanchi::Engine<T>& eng,
                  const hanchi::Params& p,
                  double freqHz, double amp,
                  double fs, int blockSize, int numBlocks, int skipBlocks)
{
    juce::AudioBuffer<T> buf (2, blockSize);

    double sumSq = 0.0;
    int    count = 0;
    bool   bad   = false;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            T* w = buf.getWritePointer (ch);

            for (int i = 0; i < blockSize; ++i)
            {
                const double t = (double) (b * blockSize + i) / fs;
                w[i] = (T) (amp * std::sin (2.0 * kPi * freqHz * t));
            }
        }

        eng.process (buf, p);

        if (b < skipBlocks)
            continue;

        for (int ch = 0; ch < 2; ++ch)
        {
            const T* r = buf.getReadPointer (ch);

            for (int i = 0; i < blockSize; ++i)
            {
                const double v = (double) r[i];

                if (! std::isfinite (v))
                    bad = true;

                sumSq += v * v;
                ++count;
            }
        }
    }

    if (bad || count == 0)
        return -1.0;

    return std::sqrt (sumSq / (double) count);
}

double referenceRms (double amp)
{
    // RMS of a sine of the given amplitude.
    return amp / std::sqrt (2.0);
}

hanchi::Params unityParams (int factor)
{
    hanchi::Params p;
    p.splitMode = false;      // broadband: one gain for the whole signal
    p.lowEn     = true;
    p.highEn    = true;
    p.lowThr    = 0.0;        // 1 kHz test tone never reaches 0 dBFS band level
    p.highThr   = 0.0;
    p.lowStr    = 0.0;
    p.highStr   = 0.0;
    p.lookMs    = 0.0;
    p.osFactor  = factor;
    return p;
}

hanchi::Params deessParams (int factor)
{
    hanchi::Params p;
    p.splitMode = true;
    p.lowEn     = true;
    p.highEn    = true;
    p.lowFreq   = 6000.0;
    p.highFreq  = 9500.0;
    p.lowThr    = -40.0;      // an 8 kHz tone at -6 dBFS is far above this
    p.highThr   = -40.0;
    p.lowRatio  = 8.0;
    p.highRatio = 8.0;
    p.lowStr    = 1.0;
    p.highStr   = 1.0;
    p.lookMs    = 3.0;
    p.osFactor  = factor;
    return p;
}

// Direct measurement of the crossover tree at each oversampled rate.
// The three bands must sum back to the input (0 dB) - that is the property
// that makes the de-esser actually reduce level instead of leaking it through
// an un-attenuated band.
void testCrossoverResponse()
{
    std::cout << "\n[crossover tree] 8 kHz tone  |  LP4(6k) / LP4(9.5k)-LP4(6k) / in-LP4(9.5k)   [SUM must be 0 dB]\n";

    const int factors[4] = { 1, 2, 4, 8 };

    for (int f : factors)
    {
        const double fs    = 48000.0 * (double) f;
        const double amp   = 0.5;
        const int    total = (int) fs;          // one second at the oversampled rate
        const int    skip  = total / 2;

        hanchi::Crossover<double> x;
        x.set (fs, 6000.0, 9500.0);

        double sLow = 0.0, sMid = 0.0, sHigh = 0.0, sSum = 0.0;
        int    count = 0;

        for (int i = 0; i < total; ++i)
        {
            const double t  = (double) i / fs;
            const double in = amp * std::sin (2.0 * kPi * 8000.0 * t);
            const auto   o  = x.process (in);

            if (i >= skip)
            {
                sLow  += (double) o.low  * (double) o.low;
                sMid  += (double) o.mid  * (double) o.mid;
                sHigh += (double) o.high * (double) o.high;

                const double sum = (double) o.low + (double) o.mid + (double) o.high;
                sSum += sum * sum;
                ++count;
            }
        }

        const double ref = referenceRms (amp);
        const double n   = (double) count;

        std::cout << "  " << f << "x (fsOs=" << (int) fs << "): low "
                  << toDb (std::sqrt (sLow  / n) / ref) << " dB, mid "
                  << toDb (std::sqrt (sMid  / n) / ref) << " dB, high "
                  << toDb (std::sqrt (sHigh / n) / ref) << " dB, SUM "
                  << toDb (std::sqrt (sSum  / n) / ref) << " dB\n";
    }
}


// Isolates JUCE's oversampler: feed an 8 kHz tone at 48 kHz, look at what
// actually comes out of processSamplesUp().
void testOversamplerIsolation()
{
    std::cout << "\n[oversampler isolation] 8 kHz tone @ 48 kHz, upsampled\n";

    const int blockSize = 512;

    for (int f : { 2, 4, 8 })
    {
        juce::dsp::Oversampling<double> os ((size_t) 2, (size_t) f,
            juce::dsp::Oversampling<double>::filterHalfBandPolyphaseIIR, false);
        os.initProcessing ((size_t) blockSize);
        os.reset();

        juce::AudioBuffer<double> buf (2, blockSize);

        double sumSq = 0.0, diffSq = 0.0;
        long long n = 0;
        double prev = 0.0;
        bool havePrev = false;

        for (int b = 0; b < 120; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* w = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                {
                    const double t = (double) (b * blockSize + i) / 48000.0;
                    w[i] = 0.5 * std::sin (2.0 * kPi * 8000.0 * t);
                }
            }

            auto block = juce::dsp::AudioBlock<double> (buf);
            juce::dsp::AudioBlock<const double> cblock (block);
            auto up = os.processSamplesUp (cblock);

            if (b == 0)
                std::cout << "  " << f << "x: up.getNumSamples() = " << (int) up.getNumSamples()
                          << " (expected " << blockSize * f << ")\n";

            if (b >= 60)
            {
                const double* p = up.getChannelPointer (0);
                for (int i = 0; i < (int) up.getNumSamples(); ++i)
                {
                    const double v = p[i];
                    sumSq += v * v;
                    if (havePrev) { const double d = v - prev; diffSq += d * d; ++n; }
                    prev = v; havePrev = true;
                }
            }

            os.processSamplesDown (block);
        }

        const double rms  = std::sqrt (sumSq / (double) n);
        const double drms = std::sqrt (diffSq / (double) n);
        const double ref  = referenceRms (0.5);

        std::cout << "  " << f << "x: upsampled rms " << toDb (rms / ref)
                  << " dB vs input, diffRms/rms = " << (drms / rms)
                  << " (8 kHz at " << 48000 * f << " Hz expects "
                  << 2.0 * std::sin (kPi * 8000.0 / (48000.0 * f)) << ")\n";
    }
}

template <typename T>
void testPrecision (const std::string& label, double fs, int blockSize)
{
    const int numBlocks  = 120;
    const int skipBlocks = 60;
    const double amp     = 0.5;

    const int factors[4] = { 1, 2, 4, 8 };

    std::cout << "\n[" << label << "] sample rate " << fs << " Hz, block " << blockSize << "\n";

    // ---- 1. unity: nothing should be gained or lost ----------------------
    for (int f : factors)
    {
        hanchi::Engine<T> eng;
        eng.prepare (fs, blockSize);

        const double out = runStream<T> (eng, unityParams (f), 1000.0, amp,
                                         fs, blockSize, numBlocks, skipBlocks);

        const std::string name = "unity 1 kHz sine, " + std::to_string (f) + "x";

        if (out < 0.0)
        {
            check (false, name, "non-finite sample");
            continue;
        }

        const double ratio = out / referenceRms (amp);
        const double db    = toDb (ratio);

        check (std::abs (db) < 0.1, name,
               std::to_string (db) + " dB vs input (must be within +/-0.1 dB)");
    }

    // ---- 1b. split mode must rebuild the input when not compressing -----
    // This is the gold test for the crossover: if the three bands do not sum
    // back to the input, un-attenuated energy leaks past the de-esser.
    for (int f : factors)
    {
        for (double freq : { 1000.0, 3000.0, 8000.0, 12000.0 })
        {
            hanchi::Engine<T> eng;
            eng.prepare (fs, blockSize);

            hanchi::Params p;
            p.splitMode = true;
            p.lowEn     = true;
            p.highEn    = true;
            p.lowFreq   = 6000.0;
            p.highFreq  = 9500.0;
            p.lowThr    = 20.0;      // never reached by a -6 dBFS tone
            p.highThr   = 20.0;
            p.lowStr    = 0.0;
            p.highStr   = 0.0;
            p.lookMs    = 0.0;
            p.osFactor  = f;

            const double out = runStream<T> (eng, p, freq, amp,
                                             fs, blockSize, numBlocks, skipBlocks);

            const std::string name = "split rebuild, " + std::to_string ((int) freq)
                                   + " Hz, " + std::to_string (f) + "x";

            if (out < 0.0)
            {
                check (false, name, "non-finite sample");
                continue;
            }

            const double db = toDb (out / referenceRms (amp));

            check (std::abs (db) < 0.3, name,
                   std::to_string (db) + " dB vs input (must be within +/-0.3 dB)");
        }
    }

    // ---- 2. de-essing must actually work, and must not mute -------------
    for (int f : factors)
    {
        hanchi::Engine<T> eng;
        eng.prepare (fs, blockSize);

        const double out = runStream<T> (eng, deessParams (f), 8000.0, amp,
                                         fs, blockSize, numBlocks, skipBlocks);

        const std::string name = "de-ess 8 kHz tone, " + std::to_string (f) + "x";

        if (out < 0.0)
        {
            check (false, name, "non-finite sample");
            continue;
        }

        const double ratio = out / referenceRms (amp);
        const double db    = toDb (ratio);

        check (db < -1.0 && db > -60.0, name,
               std::to_string (db) + " dB (must be between -1 and -60 dB)");
        std::cout << "         engine GR: low " << eng.getGrLowDb()
                  << " dB, high " << eng.getGrHighDb() << " dB\n";

        const auto& d = eng.diag;
        if (d.n > 0)
        {
            const double nn = (double) d.n;
            std::cout << "         |in| "   << toDb (std::sqrt (d.inSq   / nn) / referenceRms (amp))
                      << " dB, |low| " << toDb (std::sqrt (d.lowSq  / nn) / referenceRms (amp))
                      << " dB, |mid| " << toDb (std::sqrt (d.midSq  / nn) / referenceRms (amp))
                      << " dB, |high| " << toDb (std::sqrt (d.highSq / nn) / referenceRms (amp))
                      << " dB, |out| " << toDb (std::sqrt (d.outSq  / nn) / referenceRms (amp))
                      << " dB\n";
            {
                std::string w;
                for (int k = 0; k < 24; ++k)
                    w += (k ? " " : "") + std::to_string (d.first[k]).substr (0, 7);
                std::cout << "         first 24 samples: " << w << "\n";
            }
            std::cout << "         diffRms/inRms = "
                      << (std::sqrt (d.diffSq / nn) / std::sqrt (d.inSq / nn)) << "\n";
            std::cout << "         |low raw| (pre-delay) "
                      << toDb (std::sqrt (d.lowRawSq / nn) / referenceRms (amp)) << " dB"
                      << ", upsampled freq ~ " << (0.5 * (double) d.zeroCross / nn * 48000.0 * (double) f) / 1000.0
                      << " kHz\n";
            std::cout << "         xover: fs=" << eng.debugSetFs()
                      << " f1=" << eng.debugSetF1() << " f2=" << eng.debugSetF2()
                      << " g1=" << eng.debugG1() << " g2=" << eng.debugG2() << "\n";
            std::cout << "         gLow range [" << d.gLowMin << " .. " << d.gLowMax
                      << "], gHigh range [" << d.gHighMin << " .. " << d.gHighMax
                      << "], samples " << d.n << "\n";
        }
    }

    // ---- 3. silence in, silence out --------------------------------------
    for (int f : factors)
    {
        hanchi::Engine<T> eng;
        eng.prepare (fs, blockSize);

        const double out = runStream<T> (eng, deessParams (f), 8000.0, 0.0,
                                         fs, blockSize, numBlocks, skipBlocks);

        const std::string name = "silence stays silent, " + std::to_string (f) + "x";

        check (out >= 0.0 && out < 1e-6, name,
               out < 0.0 ? "non-finite sample" : "rms " + std::to_string (out));
    }

    // ---- 4. switching oversampling mid-stream ----------------------------
    {
        hanchi::Engine<T> eng;
        eng.prepare (fs, blockSize);

        juce::AudioBuffer<T> buf (2, blockSize);

        bool bad = false;
        double lastRms = 0.0;
        int blockIndex = 0;

        for (int f : { 1, 2, 4, 8, 1, 8, 4 })
        {
            hanchi::Params p = deessParams (f);

            for (int b = 0; b < 20; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    T* w = buf.getWritePointer (ch);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        const double t = (double) (blockIndex * blockSize + i) / fs;
                        w[i] = (T) (amp * std::sin (2.0 * kPi * 8000.0 * t));
                    }
                }

                eng.process (buf, p);
                ++blockIndex;

                double s = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const T* r = buf.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const double v = (double) r[i];
                        if (! std::isfinite (v))
                            bad = true;
                        s += v * v;
                    }
                }

                lastRms = std::sqrt (s / (double) (blockSize * 2));
            }
        }

        check (! bad && lastRms > 1e-5, "live oversampling switching 1x<->8x",
               bad ? "non-finite sample" : "final rms " + std::to_string (lastRms));
    }

    // ---- 5. mono buffer ---------------------------------------------------
    {
        hanchi::Engine<T> eng;
        eng.prepare (fs, blockSize);

        juce::AudioBuffer<T> buf (1, blockSize);

        bool bad = false;
        double rms = 0.0;

        for (int b = 0; b < 40; ++b)
        {
            T* w = buf.getWritePointer (0);

            for (int i = 0; i < blockSize; ++i)
            {
                const double t = (double) (b * blockSize + i) / fs;
                w[i] = (T) (amp * std::sin (2.0 * kPi * 1000.0 * t));
            }

            eng.process (buf, unityParams (4));

            if (b >= 20)
            {
                const T* r = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                {
                    const double v = (double) r[i];
                    if (! std::isfinite (v))
                        bad = true;
                    rms += v * v;
                }
            }
        }

        rms = std::sqrt (rms / (double) (20 * blockSize));
        const double db = toDb (rms / referenceRms (amp));

        check (! bad && std::abs (db) < 0.1, "mono buffer, 4x",
               std::to_string (db) + " dB vs input");
    }
}

} // namespace

int main()
{
    std::cout << "HanChi DSP self-test\n";
    std::cout << "====================\n";

    testCrossoverResponse();
    testOversamplerIsolation();

    testPrecision<float>  ("32-bit float", 48000.0, 512);
    testPrecision<double> ("64-bit float", 48000.0, 512);
    testPrecision<float>  ("32-bit float", 44100.0, 256);

    std::cout << "\n====================\n";

    if (g_failures == 0)
    {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }

    std::cout << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
