#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace p2p::net::fec {

// Systematic Reed-Solomon erasure code over GF(2^8) with a Cauchy parity
// matrix: k data shards + m parity shards (k + m <= 256), all of equal
// length. Any k of the k + m shards rebuild the data.
constexpr int kMaxShards = 256;

// |data|: k shards of |len| bytes. Writes m parity shards of |len| bytes.
void encode(const std::vector<const uint8_t*>& data, size_t len, int m, std::vector<std::vector<uint8_t>>& parity);

// |shards|: k + m entries, index i < k is data i, k + j is parity j; an
// empty vector marks a missing shard, present ones are |len| bytes.
// Fills in the missing data shards (parity is left alone). Returns false
// if fewer than k shards are present.
bool decode(int k, int m, size_t len, std::vector<std::vector<uint8_t>>& shards);

} // namespace p2p::net::fec
