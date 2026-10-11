#include "Fft.h"

#include <cassert>
#include <cmath>

namespace kt
{
static constexpr double twoPi = 6.283185307179586476925286766559;

namespace
{
    /** Iterative radix-2 complex FFT on separate re/im arrays. Plain arithmetic, no
        std::complex operator* (which carries slow NaN/Inf handling in most compilers). */
    void transform (float* re, float* im, int n, const std::vector<int>& bitReverse,
                    const std::vector<float>& twRe, const std::vector<float>& twIm, int twStride)
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = bitReverse[(size_t) i];
            if (j > i)
            {
                std::swap (re[i], re[j]);
                std::swap (im[i], im[j]);
            }
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2;
            const int step = (n / len) * twStride;
            for (int start = 0; start < n; start += len)
            {
                float* aRe = re + start;
                float* aIm = im + start;
                float* bRe = re + start + half;
                float* bIm = im + start + half;
                for (int k = 0; k < half; ++k)
                {
                    const float wr = twRe[(size_t) (k * step)], wi = twIm[(size_t) (k * step)];
                    const float tr = wr * bRe[k] - wi * bIm[k];
                    const float ti = wr * bIm[k] + wi * bRe[k];
                    bRe[k] = aRe[k] - tr;
                    bIm[k] = aIm[k] - ti;
                    aRe[k] += tr;
                    aIm[k] += ti;
                }
            }
        }
    }

    std::vector<int> makeBitReverse (int n)
    {
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        std::vector<int> r ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int v = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    v |= 1 << (bits - 1 - b);
            r[(size_t) i] = v;
        }
        return r;
    }
} // namespace

Fft::Fft (int size) : n (size)
{
    assert (size >= 4 && (size & (size - 1)) == 0);

    bitReverse = makeBitReverse (n);
    halfBitReverse = makeBitReverse (n / 2);

    // Twiddles for the full size; the half-size transform uses every second one.
    twRe.resize ((size_t) n / 2);
    twIm.resize ((size_t) n / 2);
    for (int i = 0; i < n / 2; ++i)
    {
        twRe[(size_t) i] = (float) std::cos (-twoPi * i / n);
        twIm[(size_t) i] = (float) std::sin (-twoPi * i / n);
    }

    window.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * i / n));

    scratchRe.resize ((size_t) n);
    scratchIm.resize ((size_t) n);
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
        scratchRe[(size_t) i] = data[(size_t) i].real();
        scratchIm[(size_t) i] = data[(size_t) i].imag();
    }
    transform (scratchRe.data(), scratchIm.data(), n, bitReverse, twRe, twIm, 1);
    for (int i = 0; i < n; ++i)
        data[(size_t) i] = { scratchRe[(size_t) i], scratchIm[(size_t) i] };
}

void Fft::magnitudeSpectrum (const float* input, std::vector<float>& magnitudes) const
{
    // Real-input FFT via a half-size complex FFT: z[m] = x[2m] + i x[2m+1].
    const int h = n / 2;
    float* zr = scratchRe.data();
    float* zi = scratchIm.data();
    for (int m = 0; m < h; ++m)
    {
        zr[m] = input[2 * m] * window[(size_t) (2 * m)];
        zi[m] = input[2 * m + 1] * window[(size_t) (2 * m + 1)];
    }
    transform (zr, zi, h, halfBitReverse, twRe, twIm, 2);

    // Untangle: X[k] = E[k] + W^k O[k], with E/O the spectra of the even/odd samples.
    magnitudes.resize ((size_t) h + 1);
    for (int k = 0; k <= h; ++k)
    {
        const int a = k % h, b = (h - k) % h;
        const float er = 0.5f * (zr[a] + zr[b]), ei = 0.5f * (zi[a] - zi[b]);
        const float orr = 0.5f * (zi[a] + zi[b]), oi = -0.5f * (zr[a] - zr[b]);
        float wr, wi;
        if (k < h)
        {
            wr = twRe[(size_t) k];
            wi = twIm[(size_t) k];
        }
        else
        {
            wr = -1.0f; // W^(N/2) = -1
            wi = 0.0f;
        }
        const float xr = er + (wr * orr - wi * oi);
        const float xi = ei + (wr * oi + wi * orr);
        magnitudes[(size_t) k] = std::sqrt (xr * xr + xi * xi);
    }
}
} // namespace kt
