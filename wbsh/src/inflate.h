#pragma once

/**
 * @file inflate.h
 * @brief Minimal RFC 1951 (DEFLATE) decoder, puff-style: correctness over throughput.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

	/// Decodes a raw DEFLATE bitstream (RFC 1951, not the gzip / zlib
	/// wrapper) and appends to `out`. False on malformed input.
	bool inflateRaw(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>& out);

	/// inflateRaw that also adds the number of input bytes the stream took,
	/// through its final block's last partial byte, to @p consumed.
	bool inflateRawConsumed(const std::uint8_t* in, std::size_t in_len,
	                        std::vector<std::uint8_t>& out, std::size_t& consumed);

}  // namespace wbsh
