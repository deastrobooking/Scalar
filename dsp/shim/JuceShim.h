#pragma once
// JuceShim.h — minimal JUCE compatibility layer for the GLIESE 710 DSP engines.
//
// Scalar ports the GLIESE_710 engine sources nearly verbatim so upstream fixes
// can be diffed in easily. This header provides just the juce:: surface those
// sources touch: clamping helpers, math constants, a tiny RNG, value smoothing,
// a TPT state-variable filter, a biquad IIR filter, and a radix-2 FFT (used
// only at init time for wavetable mipmap generation).
//
// No dynamic allocation happens in any per-sample path.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace juce
{
//==============================================================================
// Integer aliases
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using int8   = std::int8_t;
using int16  = std::int16_t;
using int32  = std::int32_t;
using int64  = std::int64_t;

using String = std::string;

//==============================================================================
// Math constants + range helpers
template <typename T>
struct MathConstants
{
    static constexpr T pi     = static_cast<T>(3.141592653589793238L);
    static constexpr T twoPi  = static_cast<T>(2.0L * 3.141592653589793238L);
    static constexpr T halfPi = static_cast<T>(0.5L * 3.141592653589793238L);
    static constexpr T sqrt2  = static_cast<T>(1.414213562373095049L);
    static constexpr T euler  = static_cast<T>(2.718281828459045235L);
};

template <typename T>
constexpr T jlimit(T lo, T hi, T v) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

template <typename T>
constexpr T jmax(T a, T b) noexcept { return a > b ? a : b; }

template <typename T>
constexpr T jmax(T a, T b, T c) noexcept { return jmax(jmax(a, b), c); }

template <typename T>
constexpr T jmin(T a, T b) noexcept { return a < b ? a : b; }

template <typename T>
constexpr T jmin(T a, T b, T c) noexcept { return jmin(jmin(a, b), c); }

inline int roundToInt(float v) noexcept  { return static_cast<int>(std::lround(v)); }
inline int roundToInt(double v) noexcept { return static_cast<int>(std::lround(v)); }

struct Decibels
{
    template <typename T>
    static T decibelsToGain(T dB, T minusInfinityDb = static_cast<T>(-100))
    {
        return dB > minusInfinityDb ? std::pow(static_cast<T>(10), dB * static_cast<T>(0.05))
                                    : T();
    }
};

//==============================================================================
// Random — xorshift32, matches juce::Random's float contract ([0, 1))
class Random
{
public:
    Random() : state(0x9e3779b9u) {}
    explicit Random(int64 seed) : state(static_cast<uint32>(seed) | 1u) {}

    float nextFloat() noexcept
    {
        return static_cast<float>(nextUint()) * (1.0f / 4294967296.0f);
    }

    int nextInt(int maxExclusive) noexcept
    {
        return maxExclusive > 0 ? static_cast<int>(nextUint() % static_cast<uint32>(maxExclusive)) : 0;
    }

private:
    uint32 state;

    uint32 nextUint() noexcept
    {
        uint32 x = state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        state = x;
        return x;
    }
};

//==============================================================================
// SmoothedValue — linear or multiplicative ramp over a fixed time
namespace ValueSmoothingTypes
{
    struct Linear {};
    struct Multiplicative {};
}

template <typename T, typename SmoothingType = ValueSmoothingTypes::Linear>
class SmoothedValue
{
public:
    void reset(double sampleRate, double rampSeconds) noexcept
    {
        rampSamples = jmax(1, static_cast<int>(sampleRate * rampSeconds));
        setCurrentAndTargetValue(target);
    }

    void setCurrentAndTargetValue(T v) noexcept
    {
        current = target = v;
        remaining = 0;
    }

    void setTargetValue(T v) noexcept
    {
        if (v == target)
            return;
        target = v;
        remaining = rampSamples;
        updateStep();
    }

    T getNextValue() noexcept
    {
        if (remaining <= 0)
            return current = target;
        --remaining;
        if constexpr (std::is_same_v<SmoothingType, ValueSmoothingTypes::Multiplicative>)
            current *= step;
        else
            current += step;
        if (remaining <= 0)
            current = target;
        return current;
    }

    T getCurrentValue() const noexcept { return current; }
    T getTargetValue() const noexcept  { return target; }
    bool isSmoothing() const noexcept  { return remaining > 0; }

private:
    T current {}, target {}, step {};
    int rampSamples = 1, remaining = 0;

    void updateStep() noexcept
    {
        if constexpr (std::is_same_v<SmoothingType, ValueSmoothingTypes::Multiplicative>)
        {
            // Guard against zero/negative endpoints: fall back to a jump.
            if (current <= T() || target <= T())
            {
                step = T(1);
                remaining = 0;
                current = target;
            }
            else
            {
                step = std::pow(target / current, T(1) / static_cast<T>(remaining));
            }
        }
        else
        {
            step = (target - current) / static_cast<T>(remaining);
        }
    }
};

//==============================================================================
namespace dsp
{
template <typename T>
using Complex = std::complex<T>;

struct ProcessSpec
{
    double sampleRate;
    uint32 maximumBlockSize;
    uint32 numChannels;
};

//==============================================================================
// StateVariableTPTFilter — Vadim Zavalishin's TPT SVF (same topology as JUCE's)
enum class StateVariableTPTFilterType { lowpass, bandpass, highpass };

template <typename T>
class StateVariableTPTFilter
{
public:
    using Type = StateVariableTPTFilterType;

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate > 0 ? spec.sampleRate : 48000.0;
        update();
        reset();
    }

    void reset() noexcept { ic1eq = ic2eq = T(); }

    void setType(Type t) noexcept { type = t; }

    void setCutoffFrequency(T hz) noexcept
    {
        cutoff = jlimit(static_cast<T>(10), static_cast<T>(sampleRate * 0.49), hz);
        update();
    }

    void setResonance(T q) noexcept
    {
        resonance = jmax(static_cast<T>(0.025), q);
        update();
    }

    T processSample(int /*channel*/, T x) noexcept
    {
        const T v3 = x - ic2eq;
        const T v1 = a1 * ic1eq + a2 * v3;
        const T v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = static_cast<T>(2) * v1 - ic1eq;
        ic2eq = static_cast<T>(2) * v2 - ic2eq;

        switch (type)
        {
            case Type::lowpass:  return v2;
            case Type::bandpass: return v1;
            case Type::highpass: return x - k * v1 - v2;
        }
        return v2;
    }

private:
    double sampleRate = 48000.0;
    T cutoff = static_cast<T>(1000), resonance = static_cast<T>(0.7071);
    T g {}, k {}, a1 {}, a2 {}, a3 {};
    T ic1eq {}, ic2eq {};
    Type type = Type::lowpass;

    void update() noexcept
    {
        g  = std::tan(MathConstants<T>::pi * cutoff / static_cast<T>(sampleRate));
        k  = static_cast<T>(1) / resonance;
        a1 = static_cast<T>(1) / (static_cast<T>(1) + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

//==============================================================================
// IIR — biquad with JUCE-compatible coefficient handling.
// Coefficient layout: { b0, b1, b2, a0, a1, a2 } (unnormalised, RBJ cookbook).
namespace IIR
{
template <typename T>
struct ArrayCoefficients
{
    static std::array<T, 6> makeLowPass(double sampleRate, T frequency,
                                        T Q = static_cast<T>(0.70710678))
    {
        const T w0 = MathConstants<T>::twoPi * frequency / static_cast<T>(sampleRate);
        const T cw = std::cos(w0), sw = std::sin(w0);
        const T alpha = sw / (static_cast<T>(2) * Q);
        return { (1 - cw) / 2, 1 - cw, (1 - cw) / 2,
                 1 + alpha, -2 * cw, 1 - alpha };
    }

    static std::array<T, 6> makePeakFilter(double sampleRate, T frequency, T Q, T gainFactor)
    {
        const T A = std::sqrt(jmax(static_cast<T>(1.0e-4), gainFactor));
        const T w0 = MathConstants<T>::twoPi * frequency / static_cast<T>(sampleRate);
        const T cw = std::cos(w0), sw = std::sin(w0);
        const T alpha = sw / (static_cast<T>(2) * Q);
        return { 1 + alpha * A, -2 * cw, 1 - alpha * A,
                 1 + alpha / A, -2 * cw, 1 - alpha / A };
    }

    static std::array<T, 6> makeLowShelf(double sampleRate, T frequency, T Q, T gainFactor)
    {
        return makeShelf(sampleRate, frequency, Q, gainFactor, true);
    }

    static std::array<T, 6> makeHighShelf(double sampleRate, T frequency, T Q, T gainFactor)
    {
        return makeShelf(sampleRate, frequency, Q, gainFactor, false);
    }

private:
    static std::array<T, 6> makeShelf(double sampleRate, T frequency, T Q, T gainFactor, bool low)
    {
        const T A = std::sqrt(jmax(static_cast<T>(1.0e-4), gainFactor));
        const T w0 = MathConstants<T>::twoPi * frequency / static_cast<T>(sampleRate);
        const T cw = std::cos(w0), sw = std::sin(w0);
        const T beta = sw * std::sqrt(A) / jmax(static_cast<T>(1.0e-6), Q);
        const T s = low ? static_cast<T>(1) : static_cast<T>(-1);

        return { A * ((A + 1) - s * (A - 1) * cw + beta),
                 s * 2 * A * ((A - 1) - s * (A + 1) * cw),
                 A * ((A + 1) - s * (A - 1) * cw - beta),
                 (A + 1) + s * (A - 1) * cw + beta,
                 s * -2 * ((A - 1) + s * (A + 1) * cw),
                 (A + 1) + s * (A - 1) * cw - beta };
    }
};

template <typename T>
struct Coefficients
{
    static std::array<T, 6> makeLowPass(double sampleRate, T frequency,
                                        T Q = static_cast<T>(0.70710678))
    {
        return ArrayCoefficients<T>::makeLowPass(sampleRate, frequency, Q);
    }
};

template <typename T>
class Filter
{
public:
    // Supports both `filter.coefficients = array` and `*filter.coefficients = array`.
    struct CoefficientHandle
    {
        std::array<T, 6> raw { 1, 0, 0, 1, 0, 0 };
        CoefficientHandle& operator=(const std::array<T, 6>& a) noexcept { raw = a; return *this; }
        CoefficientHandle& operator*() noexcept { return *this; }
    };

    CoefficientHandle coefficients;

    void prepare(const ProcessSpec&) noexcept { reset(); }
    void reset() noexcept { s1 = s2 = T(); }

    T processSample(T x) noexcept
    {
        const auto& c = coefficients.raw;
        const T a0 = c[3] != T() ? c[3] : static_cast<T>(1);
        const T b0 = c[0] / a0, b1 = c[1] / a0, b2 = c[2] / a0;
        const T a1 = c[4] / a0, a2 = c[5] / a0;

        const T y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }

private:
    T s1 {}, s2 {};
};
} // namespace IIR

//==============================================================================
// FFT — iterative radix-2 complex transform. Init-time only (wavetable mipmaps).
// Matches juce::dsp::FFT::perform(): the inverse pass scales by 1/N.
class FFT
{
public:
    explicit FFT(int orderIn) : order(orderIn), size(1 << orderIn) {}

    void perform(const Complex<float>* input, Complex<float>* output, bool inverse) const
    {
        const int n = size;
        for (int i = 0; i < n; ++i)
            output[bitReverse(i)] = input[i];

        for (int len = 2; len <= n; len <<= 1)
        {
            const float ang = (inverse ? 1.f : -1.f) * MathConstants<float>::twoPi / static_cast<float>(len);
            const Complex<float> wBase(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len)
            {
                Complex<float> w(1.f, 0.f);
                for (int j = 0; j < len / 2; ++j)
                {
                    const Complex<float> u = output[i + j];
                    const Complex<float> v = output[i + j + len / 2] * w;
                    output[i + j] = u + v;
                    output[i + j + len / 2] = u - v;
                    w *= wBase;
                }
            }
        }

        if (inverse)
        {
            const float scale = 1.f / static_cast<float>(n);
            for (int i = 0; i < n; ++i)
                output[i] *= scale;
        }
    }

    int getSize() const noexcept { return size; }

private:
    int order, size;

    int bitReverse(int x) const noexcept
    {
        int r = 0;
        for (int b = 0; b < order; ++b)
        {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }
        return r;
    }
};
} // namespace dsp
} // namespace juce
