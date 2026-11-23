#include "net/fec.h"

#include <array>
#include <cstring>

namespace p2p::net::fec {

namespace {

// GF(2^8) with the 0x11d polynomial; full multiplication table for speed.
struct Gf {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    std::array<std::array<uint8_t, 256>, 256> mul{};

    Gf()
    {
        unsigned x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[size_t(i)] = uint8_t(x);
            log[x] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; ++i) exp[size_t(i)] = exp[size_t(i - 255)];
        for (int a = 1; a < 256; ++a)
            for (int b = 1; b < 256; ++b) mul[size_t(a)][size_t(b)] = exp[size_t(log[size_t(a)] + log[size_t(b)])];
    }
    uint8_t inv(uint8_t a) const { return exp[size_t(255 - log[a])]; }
};

const Gf& gf()
{
    static const Gf g;
    return g;
}

// Cauchy element for parity row j, data column i: 1 / (x_j + y_i), with
// x_j = j and y_i = m + i so all points are distinct.
uint8_t cauchy(int m, int j, int i)
{
    return gf().inv(uint8_t(j ^ (m + i)));
}

// dst ^= c * src
void mulAdd(uint8_t* dst, const uint8_t* src, uint8_t c, size_t len)
{
    if (c == 0) return;
    if (c == 1) {
        for (size_t n = 0; n < len; ++n) dst[n] ^= src[n];
        return;
    }
    const auto& row = gf().mul[c];
    for (size_t n = 0; n < len; ++n) dst[n] ^= row[src[n]];
}

} // namespace

uint8_t gfMul(uint8_t a, uint8_t b) { return gf().mul[a][b]; }
uint8_t gfInv(uint8_t a) { return gf().inv(a); }
void gfMulAdd(uint8_t* dst, const uint8_t* src, uint8_t c, size_t len) { mulAdd(dst, src, c, len); }

void encode(const std::vector<const uint8_t*>& data, size_t len, int m, std::vector<std::vector<uint8_t>>& parity)
{
    const int k = int(data.size());
    parity.assign(size_t(m), std::vector<uint8_t>(len, 0));
    if (k + m > kMaxShards) return;
    for (int j = 0; j < m; ++j)
        for (int i = 0; i < k; ++i) mulAdd(parity[size_t(j)].data(), data[size_t(i)], cauchy(m, j, i), len);
}

bool decode(int k, int m, size_t len, std::vector<std::vector<uint8_t>>& shards)
{
    if (int(shards.size()) != k + m || k + m > kMaxShards) return false;
    std::vector<int> missing;
    for (int i = 0; i < k; ++i)
        if (shards[size_t(i)].size() != len) missing.push_back(i);
    if (missing.empty()) return true;

    // Use as many parity shards as there are missing data shards.
    std::vector<int> par;
    for (int j = 0; j < m && par.size() < missing.size(); ++j)
        if (shards[size_t(k + j)].size() == len) par.push_back(j);
    if (par.size() < missing.size()) return false;

    // For each used parity row: p_j - sum over present data = sum over
    // missing data of C[j][i] * d_i. Solve the e x e system.
    const size_t e = missing.size();
    std::vector<std::vector<uint8_t>> rhs(e);
    std::vector<std::vector<uint8_t>> a(e, std::vector<uint8_t>(e));
    for (size_t r = 0; r < e; ++r) {
        const int j = par[r];
        rhs[r] = shards[size_t(k + j)];
        for (int i = 0; i < k; ++i)
            if (shards[size_t(i)].size() == len) mulAdd(rhs[r].data(), shards[size_t(i)].data(), cauchy(m, j, i), len);
        for (size_t c = 0; c < e; ++c) a[r][c] = cauchy(m, j, missing[c]);
    }

    // Gauss-Jordan elimination (Cauchy submatrices are always invertible).
    const Gf& g = gf();
    for (size_t col = 0; col < e; ++col) {
        size_t piv = col;
        while (piv < e && a[piv][col] == 0) ++piv;
        if (piv == e) return false;
        std::swap(a[piv], a[col]);
        std::swap(rhs[piv], rhs[col]);
        const uint8_t inv = g.inv(a[col][col]);
        for (size_t c = 0; c < e; ++c) a[col][c] = g.mul[inv][a[col][c]];
        for (size_t n = 0; n < len; ++n) rhs[col][n] = g.mul[inv][rhs[col][n]];
        for (size_t r = 0; r < e; ++r) {
            if (r == col || a[r][col] == 0) continue;
            const uint8_t f = a[r][col];
            for (size_t c = 0; c < e; ++c) a[r][c] ^= g.mul[f][a[col][c]];
            mulAdd(rhs[r].data(), rhs[col].data(), f, len);
        }
    }
    for (size_t r = 0; r < e; ++r) shards[size_t(missing[r])] = std::move(rhs[r]);
    return true;
}

} // namespace p2p::net::fec
