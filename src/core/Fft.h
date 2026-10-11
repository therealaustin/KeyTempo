#pragma once

#include <complex>
#include <vector>

namespace kt
{
/** Small iterative radix-2 FFT. Dependency-free; the real-input path (used for all
    analysis) runs a half-size complex transform. Not thread-safe per instance. */
class Fft
{
public:
    explicit Fft (int size);

    int size() const noexcept { return n; }

    /** In-place complex forward transform. data.size() must equal size(). */
    void forward (std::vector<std::complex<float>>& data) const;

    /** Hann-windows `input` (size() samples) and writes |X[k]| for k in [0, size()/2]. */
    void magnitudeSpectrum (const float* input, std::vector<float>& magnitudes) const;

    static int nextPowerOfTwo (int value);

private:
    int n;
    std::vector<int> bitReverse, halfBitReverse;
    std::vector<float> twRe, twIm; // e^{-2 pi i k / n}, k < n/2
    std::vector<float> window;
    mutable std::vector<float> scratchRe, scratchIm;
};
} // namespace kt
