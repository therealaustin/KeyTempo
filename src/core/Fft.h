#pragma once

#include <complex>
#include <vector>

namespace kt
{
/** Small iterative radix-2 FFT. Not the fastest possible, but dependency-free
    and plenty for analysis that runs a few times a second off the audio thread. */
class Fft
{
public:
    explicit Fft (int size);

    int size() const noexcept { return n; }

    /** In-place forward transform. data.size() must equal size(). */
    void forward (std::vector<std::complex<float>>& data) const;

    /** Hann-windows `input` (size() samples) and writes |X[k]| for k in [0, size()/2]. */
    void magnitudeSpectrum (const float* input, std::vector<float>& magnitudes) const;

    static int nextPowerOfTwo (int value);

private:
    int n;
    std::vector<int> bitReverse;
    std::vector<std::complex<float>> twiddles;
    std::vector<float> window;
    mutable std::vector<std::complex<float>> scratch;
};
} // namespace kt
