#pragma once

/**
 * @file codecs.h
 * @brief One entry point per direction for every compressed format the
 *        bundled tools read: gzip, bzip2, xz, lzma and compress (.Z).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wbsh {

	enum class Codec {
		None,
		Gzip,
		Bzip2,
		Xz,
		Lzma,
		Compress,
	};

	/// The format @p data starts with; Codec::None when nothing matches.
	/// LZMA has no magic, so it is only recognised from a plausible header.
	Codec detectCodec(const std::uint8_t* data, std::size_t size);

	/// Codec::None for a name the tools do not know.
	Codec codecForSuffix(const std::string& path);

	/// Decodes @p in as @p codec, appending to @p out; Codec::None copies.
	bool decompressWith(Codec codec, const std::uint8_t* in, std::size_t size,
	                    std::vector<std::uint8_t>& out, std::string& error);

	/// Only gzip and bzip2 can be written; the others report an error.
	bool compressWith(Codec codec, const std::uint8_t* in, std::size_t size, int level,
	                  std::vector<std::uint8_t>& out, std::string& error);

}  // namespace wbsh
