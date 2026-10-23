#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Exact complete SHA-256 K tables embedded among crypto instructions.
std::vector<std::pair<size_t, size_t>> find_sha256_tables(const uint8_t* bytes, size_t size);
// Exact complete audited literal pools. Never classify data by undecodable
// instructions, numeric-looking words, or a broad executable address range.
std::vector<std::pair<size_t, size_t>> find_verified_code_data(const uint8_t* bytes, size_t size, const char* bundle);
