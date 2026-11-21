#pragma once

#include <cstdint>
#include <vector>

namespace p2p::audio {

// Variable-ratio resampler for clock-drift compensation on the playout path.
//
// The sender's and receiver's audio clocks never match exactly (hundreds of
// ppm is common for USB codecs). Instead of dropping or inserting whole
// frames — audible as clicks — playout consumes input at |ratio| input
// samples per output sample (ratio > 1 drains a backlog, < 1 stretches),
// using 4-point Catmull-Rom interpolation. At ratio == 1 it passes samples through unchanged.
class DriftResampler {
public:
    DriftResampler();

    void push(const int16_t* pcm, int n);
    // Input samples that must be buffered before pull(n, ratio) can run.
    size_t required(int n, double ratio) const;
    size_t size() const { return buf_.size(); }
    // Unconsumed input, in samples (fractional position rounded down).
    double buffered() const { return double(buf_.size()) - pos_; }
    // Precondition: size() >= required(n, ratio).
    void pull(int16_t* out, int n, double ratio);

private:
    std::vector<float> buf_;
    double pos_ = 1.0; // index of the next output sample; buf_[pos-1] is history
};

} // namespace p2p::audio
