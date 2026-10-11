#pragma once

/**
 * @file bzip2.h
 * @brief bzip2 stream decoder and encoder (Burrows-Wheeler, MTF and
 *        multi-table Huffman, as bzip2 1.0 writes them).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wbsh {

	/// Decodes every concatenated bzip2 stream in @p in, appending to @p out.
	/// On failure returns false and says why in @p error.
	bool bunzip2Data(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                 std::string& error);

	/// Appends one bzip2 stream for @p data; @p level 1-9 picks 100k-900k blocks.
	void bzip2Data(const std::uint8_t* data, std::size_t size, int level,
	               std::vector<std::uint8_t>& out);

}  // namespace wbsh
