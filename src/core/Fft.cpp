#include "Fft.h"

#include <cassert>
#include <cmath>

namespace kt
{
static constexpr double twoPi = 6.283185307179586476925286766559;

Fft::Fft (int size) : n (size)
{
    assert (size >= 2 && (size & (size - 1)) == 0);

    int bits = 0;
    while ((1 << bits) < n)
        ++bits;

    bitReverse.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        bitReverse[(size_t) i] = r;
    }

    twiddles.resize ((size_t) n / 2);
    for (int i = 0; i < n / 2; ++i)
        twiddles[(size_t) i] = std::polar (1.0f, (float) (-twoPi * i / n));

    window.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * i / n));

    scratch.resize ((size_t) n);
}

int Fft::nextPowerOfTwo (int value)
{
    int p = 1;
    while (p < value)
        p <<= 1;
    return p;
}

void Fft::forward (std::vector<std::complex<float>>& data) const
{
    assert ((int) data.size() == n);

    for (int i = 0; i < n; ++i)
    {
        const int j = bitReverse[(size_t) i];
        if (j > i)
            std::swap (data[(size_t) i], data[(size_t) j]);
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2;
        const int step = n / len;
        for (int start = 0; start < n; start += len)
        {
            for (int k = 0; k < half; ++k)
            {
                const auto w = twiddles[(size_t) (k * step)];
                auto& a = data[(size_t) (start + k)];
                auto& b = data[(size_t) (start + k + half)];
                const auto t = w * b;
                b = a - t;
                a = a + t;
            }
        }
    }
}

void Fft::magnitudeSpectrum (const float* input, std::vector<float>& magnitudes) const
{
    for (int i = 0; i < n; ++i)
        scratch[(size_t) i] = { input[i] * window[(size_t) i], 0.0f };

    forward (scratch);

    magnitudes.resize ((size_t) n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k)
        magnitudes[(size_t) k] = std::abs (scratch[(size_t) k]);
}
} // namespace kt
