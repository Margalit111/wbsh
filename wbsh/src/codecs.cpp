/**
 * @file codecs.cpp
 * @brief Format detection and the gzip (RFC 1952) member framing; the
 *        other formats delegate to their own decoders.
 */

#include "codecs.h"

#include <cstring>

#include "bzip2.h"
#include "deflate.h"
#include "inflate.h"
#include "lzw.h"
#include "xz.h"

namespace wbsh {

	static const std::uint8_t kGzipFlagCrc     = 0x02;
	static const std::uint8_t kGzipFlagExtra   = 0x04;
	static const std::uint8_t kGzipFlagName    = 0x08;
	static const std::uint8_t kGzipFlagComment = 0x10;
	static const std::size_t  kGzipHeaderSize  = 10;
	static const std::size_t  kGzipTrailerSize = 8;

	static std::uint32_t gzipCrc(const std::uint8_t* data, std::size_t size) {
		static std::uint32_t table[256];
		static bool built = false;
		if (!built) {
			for (std::uint32_t k = 0; k < 256; ++k) {
				std::uint32_t entry = k;
				for (int bit = 0; bit < 8; ++bit) {
					entry = (entry & 1) != 0 ? 0xEDB88320u ^ (entry >> 1) : entry >> 1;
				}

				table[k] = entry;
			}

			built = true;
		}

		std::uint32_t crc = 0xFFFFFFFFu;
		for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
		for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(value >> (8 * k)));
	}

	static bool skipZeroTerminated(const std::uint8_t* in, std::size_t size, std::size_t& at) {
		while (at < size && in[at] != 0) ++at;
		if (at >= size) return false;
		++at;
		return true;
	}

	static bool gzipHeaderEnd(const std::uint8_t* in, std::size_t size, std::size_t& at) {
		if (at + kGzipHeaderSize > size) return false;
		if (in[at] != 0x1F || in[at + 1] != 0x8B || in[at + 2] != 8) {
			return false;
		}

		const std::uint8_t flags = in[at + 3];
		at += kGzipHeaderSize;
		if ((flags & kGzipFlagExtra) != 0) {
			if (at + 2 > size) return false;
			const std::size_t extra = static_cast<std::size_t>(in[at])
				| (static_cast<std::size_t>(in[at + 1]) << 8);
			at += 2 + extra;
		}

		if ((flags & kGzipFlagName) != 0 && !skipZeroTerminated(in, size, at)) return false;
		if ((flags & kGzipFlagComment) != 0 && !skipZeroTerminated(in, size, at)) return false;
		if ((flags & kGzipFlagCrc) != 0) at += 2;
		return at <= size;
	}

	static bool inflateMember(const std::uint8_t* in, std::size_t size, std::size_t& at,
	                          std::vector<std::uint8_t>& out, std::string& error) {
		const std::size_t start = out.size();
		if (!inflateRawConsumed(in + at, size - at, out, at)) {
			error = "invalid compressed data";
			return false;
		}

		if (at + kGzipTrailerSize > size) {
			error = "unexpected end of file";
			return false;
		}

		const std::uint32_t crc = readLe32(in + at);
		const std::uint32_t length = readLe32(in + at + 4);
		at += kGzipTrailerSize;
		const std::size_t produced = out.size() - start;
		if (crc == gzipCrc(out.data() + start, produced)
		    && length == static_cast<std::uint32_t>(produced)) {
			return true;
		}

		error = "invalid compressed data--crc error";
		return false;
	}

	static bool gunzipData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                       std::string& error) {
		std::size_t at = 0;
		do {
			if (!gzipHeaderEnd(in, size, at)) {
				error = at == 0 ? "not in gzip format" : "trailing garbage ignored";
				return at != 0;
			}

			if (!inflateMember(in, size, at, out, error)) return false;
			while (at < size && in[at] == 0) ++at;
		} while (at < size);

		return true;
	}

	static void gzipData(const std::uint8_t* in, std::size_t size, int level,
	                     std::vector<std::uint8_t>& out) {
		static const std::uint8_t kHeader[kGzipHeaderSize] = {
			0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
		};
		out.insert(out.end(), kHeader, kHeader + kGzipHeaderSize);
		out[out.size() - 2] = level >= 9 ? 2 : (level <= 1 ? 4 : 0);
		deflateRaw(in, size, level, out);
		appendLe32(out, gzipCrc(in, size));
		appendLe32(out, static_cast<std::uint32_t>(size));
	}

	// .lzma has no magic number: accept the header xz and lzma-utils write,
	// a properties byte, a dictionary of 2^n or 2^n + 2^(n-1) bytes (4 KiB
	// or more), a size that is unknown or below 2^48, and the range coder's
	// mandatory zero first byte.
	static bool plausibleLzmaHeader(const std::uint8_t* data, std::size_t size) {
		if (size < 14 || data[0] >= 9 * 5 * 5) return false;

		const std::uint32_t dictionary = readLe32(data + 1);
		const std::uint32_t trimmed = dictionary & (dictionary - 1);
		const bool usual_dictionary = dictionary >= 4096
			&& (trimmed == 0 || (trimmed & (trimmed - 1)) == 0);

		bool unknown_size = true;
		for (int k = 5; k < 13; ++k) unknown_size = unknown_size && data[k] == 0xFF;
		const bool sane_size = unknown_size || (data[11] == 0 && data[12] == 0);
		return usual_dictionary && sane_size && data[13] == 0;
	}

	Codec detectCodec(const std::uint8_t* data, std::size_t size) {
		if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B) return Codec::Gzip;
		if (size >= 2 && data[0] == 0x1F && data[1] == 0x9D) return Codec::Compress;
		if (size >= 3 && std::memcmp(data, "BZh", 3) == 0) return Codec::Bzip2;
		if (size >= 6 && std::memcmp(data, "\xFD" "7zXZ", 6) == 0) return Codec::Xz;
		if (plausibleLzmaHeader(data, size)) return Codec::Lzma;
		return Codec::None;
	}

	static bool endsWith(const std::string& text, const char* suffix) {
		const std::size_t length = std::strlen(suffix);
		return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
	}

	Codec codecForSuffix(const std::string& path) {
		if (endsWith(path, ".gz") || endsWith(path, ".tgz") || endsWith(path, ".taz")) {
			return Codec::Gzip;
		}

		if (endsWith(path, ".bz2") || endsWith(path, ".tbz") || endsWith(path, ".tbz2")) {
			return Codec::Bzip2;
		}

		if (endsWith(path, ".xz") || endsWith(path, ".txz")) return Codec::Xz;
		if (endsWith(path, ".lzma") || endsWith(path, ".tlz")) return Codec::Lzma;
		if (endsWith(path, ".Z") || endsWith(path, ".taZ")) return Codec::Compress;
		return Codec::None;
	}

	bool decompressWith(Codec codec, const std::uint8_t* in, std::size_t size,
	                    std::vector<std::uint8_t>& out, std::string& error) {
		switch (codec) {
		case Codec::Gzip:     return gunzipData(in, size, out, error);
		case Codec::Bzip2:    return bunzip2Data(in, size, out, error);
		case Codec::Xz:       return unxzData(in, size, out, error);
		case Codec::Lzma:     return unlzmaData(in, size, out, error);
		case Codec::Compress: return uncompressData(in, size, out, error);
		case Codec::None:
		default:
			out.insert(out.end(), in, in + size);
			return true;
		}
	}

	bool compressWith(Codec codec, const std::uint8_t* in, std::size_t size, int level,
	                  std::vector<std::uint8_t>& out, std::string& error) {
		switch (codec) {
		case Codec::Gzip:
			gzipData(in, size, level, out);
			return true;
		case Codec::Bzip2:
			bzip2Data(in, size, level, out);
			return true;
		case Codec::None:
			out.insert(out.end(), in, in + size);
			return true;
		default:
			error = "compression to this format is not supported";
			return false;
		}
	}

}  // namespace wbsh
