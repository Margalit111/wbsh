#pragma once

/**
 * @file deflate.h
 * @brief RFC 1951 (DEFLATE) encoder: hash-chain LZ77 with lazy matching,
 *        each block written as whichever of dynamic Huffman, fixed Huffman
 *        or stored is smallest.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

	/// Appends the raw DEFLATE stream (no gzip / zlib wrapper) for @p data
	/// to @p out. @p level runs 1 (fastest) to 9 (smallest); 0 stores.
	void deflateRaw(const std::uint8_t* data, std::size_t size, int level,
	                std::vector<std::uint8_t>& out);

}  // namespace wbsh
