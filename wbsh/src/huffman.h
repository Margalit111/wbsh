#pragma once

/**
 * @file huffman.h
 * @brief Length-limited Huffman code construction (package-merge),
 *        shared by the DEFLATE and bzip2 encoders.
 */

#include <cstdint>
#include <vector>

namespace wbsh {

	/// Optimal code lengths no longer than @p max_length for @p frequencies;
	/// a symbol with frequency 0 gets length 0. A lone used symbol gets
	/// length 1, so every alphabet with at least one symbol is decodable.
	std::vector<std::uint8_t> limitedCodeLengths(const std::vector<std::uint32_t>& frequencies,
	                                             int max_length);

	/// Canonical codes (as DEFLATE and bzip2 define them) for @p lengths.
	std::vector<std::uint32_t> canonicalCodes(const std::vector<std::uint8_t>& lengths);

}  // namespace wbsh
