/**
 * @file xz.cpp
 * @brief LZMA / LZMA2 / .xz decoding. The LZMA part follows the
 *        structure of Igor Pavlov's public-domain LzmaSpec reference
 *        decoder; the container follows the .xz file format 1.0.4.
 */

#include "xz.h"

#include <algorithm>
#include <cstring>

namespace wbsh {

	namespace xz_detail {

		typedef std::uint16_t Probability;

		struct RangeDecoder {
			const std::uint8_t* data = nullptr;
			std::size_t size = 0;
			std::size_t position = 0;
			std::uint32_t range = 0xFFFFFFFFu;
			std::uint32_t code = 0;
			bool corrupted = false;

			std::uint8_t nextByte() {
				if (position < size) return data[position++];
				corrupted = true;
				return 0;
			}

			bool start() {
				range = 0xFFFFFFFFu;
				code = 0;
				const bool zero_first = nextByte() == 0;
				for (int k = 0; k < 4; ++k) code = (code << 8) | nextByte();
				return zero_first && code != range && !corrupted;
			}

			void normalize() {
				if (range >= (1u << 24)) return;
				range <<= 8;
				code = (code << 8) | nextByte();
			}

			unsigned bit(Probability& probability) {
				const std::uint32_t bound = (range >> 11) * probability;
				unsigned value = 0;
				if (code < bound) {
					const unsigned gain = (2048u - probability) >> 5;
					probability = static_cast<Probability>(probability + gain);
					range = bound;
				} else {
					probability = static_cast<Probability>(probability - (probability >> 5));
					code -= bound;
					range -= bound;
					value = 1;
				}

				normalize();
				return value;
			}

			std::uint32_t directBits(int count) {
				std::uint32_t result = 0;
				for (; count > 0; --count) {
					range >>= 1;
					code -= range;
					const std::uint32_t mask = 0u - (code >> 31);
					code += range & mask;
					if (code == range) corrupted = true;
					normalize();
					result = (result << 1) + (mask + 1);
				}

				return result;
			}
		};

		struct LengthDecoder {
			Probability choice = 1024;
			Probability choice2 = 1024;
			Probability low[16][8];
			Probability mid[16][8];
			Probability high[256];
		};

		struct LzmaProperties {
			unsigned lc = 3;
			unsigned lp = 0;
			unsigned pb = 2;
			std::uint32_t dictionary = 1u << 23;
		};

		struct LzmaState {
			LzmaProperties props;
			std::vector<Probability> literals;
			Probability is_match[192];
			Probability is_rep[12];
			Probability is_rep_g0[12];
			Probability is_rep_g1[12];
			Probability is_rep_g2[12];
			Probability is_rep0_long[192];
			Probability pos_slot[4][64];
			Probability pos_special[115];
			Probability align[16];
			LengthDecoder length;
			LengthDecoder rep_length;
			unsigned state = 0;
			std::uint32_t reps[4] = { 0, 0, 0, 0 };
		};

		struct Window {
			std::vector<std::uint8_t>* out = nullptr;
			std::size_t dictionary_start = 0;
			std::uint32_t dictionary_size = 0;

			std::size_t history() const {
				return out->size() - dictionary_start;
			}

			std::uint8_t back(std::uint32_t distance) const {
				return (*out)[out->size() - distance - 1];
			}
		};

		enum class DecodeEnd {
			Limit,
			Marker,
			Error,
		};

		struct BlockFilters {
			bool delta = false;
			unsigned delta_distance = 1;
			bool x86 = false;
			std::uint32_t dictionary = 0;
		};

	}  // namespace xz_detail

	using xz_detail::BlockFilters;
	using xz_detail::DecodeEnd;
	using xz_detail::LengthDecoder;
	using xz_detail::LzmaProperties;
	using xz_detail::LzmaState;
	using xz_detail::Probability;
	using xz_detail::RangeDecoder;
	using xz_detail::Window;

	static const Probability kProbabilityStart = 1024;
	static const unsigned kMatchMinLength = 2;
	static const std::uint32_t kEndMarker = 0xFFFFFFFFu;
	static const std::uint8_t kXzMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	static const std::size_t kStreamHeaderSize = 12;
	static const std::uint64_t kCrc64Polynomial = 0xC96C5795D7870F42ULL;

	static void fillProbabilities(Probability* first, std::size_t count) {
		std::fill(first, first + count, kProbabilityStart);
	}

	static void resetLength(LengthDecoder& decoder) {
		decoder.choice = kProbabilityStart;
		decoder.choice2 = kProbabilityStart;
		fillProbabilities(&decoder.low[0][0], 16 * 8);
		fillProbabilities(&decoder.mid[0][0], 16 * 8);
		fillProbabilities(decoder.high, 256);
	}

	static void resetLzmaState(LzmaState& lzma) {
		lzma.literals.assign(static_cast<std::size_t>(0x300) << (lzma.props.lc + lzma.props.lp),
			kProbabilityStart);
		fillProbabilities(lzma.is_match, 192);
		fillProbabilities(lzma.is_rep, 12);
		fillProbabilities(lzma.is_rep_g0, 12);
		fillProbabilities(lzma.is_rep_g1, 12);
		fillProbabilities(lzma.is_rep_g2, 12);
		fillProbabilities(lzma.is_rep0_long, 192);
		fillProbabilities(&lzma.pos_slot[0][0], 4 * 64);
		fillProbabilities(lzma.pos_special, 115);
		fillProbabilities(lzma.align, 16);
		resetLength(lzma.length);
		resetLength(lzma.rep_length);
		lzma.state = 0;
		std::fill(lzma.reps, lzma.reps + 4, 0u);
	}

	static unsigned bitTree(RangeDecoder& rc, Probability* probabilities, int bits) {
		unsigned node = 1;
		for (int k = 0; k < bits; ++k) node = (node << 1) + rc.bit(probabilities[node]);
		return node - (1u << bits);
	}

	static unsigned reverseBitTree(RangeDecoder& rc, Probability* probabilities, int bits) {
		unsigned node = 1;
		unsigned symbol = 0;
		for (int k = 0; k < bits; ++k) {
			const unsigned bit = rc.bit(probabilities[node]);
			node = (node << 1) + bit;
			symbol |= bit << k;
		}

		return symbol;
	}

	static unsigned decodeLength(RangeDecoder& rc, LengthDecoder& decoder, unsigned pos_state) {
		if (rc.bit(decoder.choice) == 0) return bitTree(rc, decoder.low[pos_state], 3);
		if (rc.bit(decoder.choice2) == 0) return 8 + bitTree(rc, decoder.mid[pos_state], 3);
		return 16 + bitTree(rc, decoder.high, 8);
	}

	static std::uint32_t decodeDistance(RangeDecoder& rc, LzmaState& lzma, unsigned length) {
		const unsigned length_state = (std::min)(length, 3u);
		const unsigned slot = bitTree(rc, lzma.pos_slot[length_state], 6);
		if (slot < 4) return slot;

		const int direct = static_cast<int>(slot >> 1) - 1;
		std::uint32_t distance = (2u | (slot & 1u)) << direct;
		if (slot < 14) {
			return distance + reverseBitTree(rc, lzma.pos_special + distance - slot, direct);
		}

		distance += rc.directBits(direct - 4) << 4;
		return distance + reverseBitTree(rc, lzma.align, 4);
	}

	static void decodeLiteral(RangeDecoder& rc, LzmaState& lzma, Window& window) {
		const std::size_t total = window.out->size();
		const unsigned previous = window.history() > 0 ? window.back(0) : 0;
		const unsigned lp_mask = (1u << lzma.props.lp) - 1;
		const std::size_t context = ((static_cast<unsigned>(total) & lp_mask) << lzma.props.lc)
			+ (previous >> (8 - lzma.props.lc));
		Probability* probabilities = lzma.literals.data() + 0x300 * context;

		unsigned symbol = 1;
		if (lzma.state >= 7) {
			unsigned match_byte = window.back(lzma.reps[0]);
			while (symbol < 0x100) {
				const unsigned match_bit = (match_byte >> 7) & 1;
				match_byte <<= 1;
				const unsigned bit = rc.bit(probabilities[((1 + match_bit) << 8) + symbol]);
				symbol = (symbol << 1) | bit;
				if (match_bit != bit) break;
			}
		}

		while (symbol < 0x100) symbol = (symbol << 1) | rc.bit(probabilities[symbol]);
		window.out->push_back(static_cast<std::uint8_t>(symbol - 0x100));
		lzma.state = lzma.state < 4 ? 0 : (lzma.state < 10 ? lzma.state - 3 : lzma.state - 6);
	}

	static bool copyMatch(Window& window, std::uint32_t distance, unsigned length) {
		if (distance >= window.dictionary_size || distance >= window.history()) return false;

		std::vector<std::uint8_t>& out = *window.out;
		for (unsigned k = 0; k < length; ++k) out.push_back(out[out.size() - distance - 1]);
		return true;
	}

	// Returns 0 for a short rep (one byte already written), otherwise the
	// match length; the distance is left in reps[0].
	static unsigned decodeRep(RangeDecoder& rc, LzmaState& lzma, Window& window,
	                          unsigned pos_state) {
		if (rc.bit(lzma.is_rep_g0[lzma.state]) == 0) {
			if (rc.bit(lzma.is_rep0_long[(lzma.state << 4) + pos_state]) == 0) {
				lzma.state = lzma.state < 7 ? 9 : 11;
				window.out->push_back(window.back(lzma.reps[0]));
				return 0;
			}
		} else {
			std::uint32_t distance = 0;
			if (rc.bit(lzma.is_rep_g1[lzma.state]) == 0) {
				distance = lzma.reps[1];
			} else {
				const bool third = rc.bit(lzma.is_rep_g2[lzma.state]) == 0;
				distance = third ? lzma.reps[2] : lzma.reps[3];
				if (!third) lzma.reps[3] = lzma.reps[2];
				lzma.reps[2] = lzma.reps[1];
			}

			lzma.reps[1] = lzma.reps[0];
			lzma.reps[0] = distance;
		}

		const unsigned length = decodeLength(rc, lzma.rep_length, pos_state);
		lzma.state = lzma.state < 7 ? 8 : 11;
		return length + kMatchMinLength;
	}

	static DecodeEnd decodeMatch(RangeDecoder& rc, LzmaState& lzma, unsigned pos_state,
	                             unsigned& length) {
		lzma.reps[3] = lzma.reps[2];
		lzma.reps[2] = lzma.reps[1];
		lzma.reps[1] = lzma.reps[0];
		length = decodeLength(rc, lzma.length, pos_state);
		lzma.state = lzma.state < 7 ? 7 : 10;
		lzma.reps[0] = decodeDistance(rc, lzma, length);
		length += kMatchMinLength;
		return lzma.reps[0] == kEndMarker ? DecodeEnd::Marker : DecodeEnd::Limit;
	}

	static DecodeEnd decodeSymbols(RangeDecoder& rc, LzmaState& lzma, Window& window,
	                               std::size_t limit) {
		const unsigned pb_mask = (1u << lzma.props.pb) - 1;
		while (window.out->size() < limit) {
			const unsigned pos_state = static_cast<unsigned>(window.out->size()) & pb_mask;
			if (rc.corrupted) return DecodeEnd::Error;
			if (rc.bit(lzma.is_match[(lzma.state << 4) + pos_state]) == 0) {
				decodeLiteral(rc, lzma, window);
				continue;
			}

			unsigned length = 0;
			if (rc.bit(lzma.is_rep[lzma.state]) != 0) {
				if (window.history() == 0) return DecodeEnd::Error;
				length = decodeRep(rc, lzma, window, pos_state);
				if (length == 0) continue;
			} else if (decodeMatch(rc, lzma, pos_state, length) == DecodeEnd::Marker) {
				return DecodeEnd::Marker;
			}

			if (window.out->size() + length > limit) return DecodeEnd::Error;
			if (!copyMatch(window, lzma.reps[0], length)) return DecodeEnd::Error;
		}

		return rc.corrupted ? DecodeEnd::Error : DecodeEnd::Limit;
	}

	static bool decodePropertiesByte(std::uint8_t byte, LzmaProperties& props) {
		if (byte >= 9 * 5 * 5) return false;
		props.lc = byte % 9;
		byte = static_cast<std::uint8_t>(byte / 9);
		props.lp = byte % 5;
		props.pb = byte / 5u;
		return true;
	}

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	bool unlzmaData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                std::string& error) {
		LzmaState lzma;
		if (size < 13 || !decodePropertiesByte(in[0], lzma.props)) {
			error = "not in lzma format";
			return false;
		}

		lzma.props.dictionary = (std::max)(readLe32(in + 1), 4096u);
		std::uint64_t unpacked = 0;
		for (int k = 7; k >= 0; --k) unpacked = (unpacked << 8) | in[5 + k];
		const bool known = unpacked != ~0ULL;

		RangeDecoder rc;
		rc.data = in + 13;
		rc.size = size - 13;
		Window window;
		window.out = &out;
		window.dictionary_start = out.size();
		window.dictionary_size = lzma.props.dictionary;
		resetLzmaState(lzma);
		if (!rc.start()) {
			error = "data is corrupt";
			return false;
		}

		const std::size_t unbounded = ~static_cast<std::size_t>(0);
		const std::size_t expected = out.size() + static_cast<std::size_t>(unpacked);
		const std::size_t limit = known ? expected : unbounded;
		const DecodeEnd end = decodeSymbols(rc, lzma, window, limit);
		if (end == DecodeEnd::Marker || (end == DecodeEnd::Limit && known)) return true;

		error = "data is corrupt";
		return false;
	}

	template <typename Word>
	static Word reflectedTableEntry(Word index, Word polynomial) {
		Word entry = index;
		for (int bit = 0; bit < 8; ++bit) {
			entry = (entry & 1) != 0 ? polynomial ^ (entry >> 1) : entry >> 1;
		}

		return entry;
	}

	static std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size) {
		static std::uint32_t table[256];
		static bool built = false;
		if (!built) {
			for (std::uint32_t k = 0; k < 256; ++k) table[k] = reflectedTableEntry(k, 0xEDB88320u);
			built = true;
		}

		std::uint32_t crc = 0xFFFFFFFFu;
		for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	static std::uint64_t crc64Of(const std::uint8_t* data, std::size_t size) {
		static std::uint64_t table[256];
		static bool built = false;
		if (!built) {
			for (std::uint64_t k = 0; k < 256; ++k) {
				table[k] = reflectedTableEntry(k, kCrc64Polynomial);
			}

			built = true;
		}

		std::uint64_t crc = ~0ULL;
		for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	static bool readVli(const std::uint8_t* data, std::size_t size, std::size_t& at,
	                    std::uint64_t& value) {
		value = 0;
		for (int shift = 0; shift < 63; shift += 7) {
			if (at >= size) return false;
			const std::uint8_t byte = data[at++];
			value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) return true;
		}

		return false;
	}

	static std::uint32_t lzma2DictionarySize(std::uint8_t bits) {
		if (bits >= 40) return 0xFFFFFFFFu;
		return (2u | (bits & 1u)) << (bits / 2 + 11);
	}

	static bool parseFilter(std::uint64_t id, const std::uint8_t* props, std::uint64_t props_size,
	                        BlockFilters& filters) {
		if (id == 0x21 && props_size == 1 && props[0] <= 40) {
			filters.dictionary = lzma2DictionarySize(props[0]);
			return true;
		}

		if (id == 0x03 && props_size == 1) {
			filters.delta = true;
			filters.delta_distance = props[0] + 1u;
			return true;
		}

		if (id == 0x04 && props_size == 0) {
			filters.x86 = true;
			return true;
		}

		return false;
	}

	static bool parseBlockHeader(const std::uint8_t* header, std::size_t header_size,
	                             BlockFilters& filters, std::string& error) {
		if (crc32Of(header, header_size - 4) != readLe32(header + header_size - 4)) {
			error = "block header checksum mismatch";
			return false;
		}

		const std::uint8_t flags = header[1];
		std::size_t at = 2;
		std::uint64_t ignored = 0;
		if ((flags & 0x40) != 0 && !readVli(header, header_size, at, ignored)) return false;
		if ((flags & 0x80) != 0 && !readVli(header, header_size, at, ignored)) return false;

		const int filter_count = (flags & 3) + 1;
		for (int k = 0; k < filter_count; ++k) {
			std::uint64_t id = 0;
			std::uint64_t props_size = 0;
			const bool parsed = readVli(header, header_size, at, id)
				&& readVli(header, header_size, at, props_size)
				&& at + props_size <= header_size - 4
				&& parseFilter(id, header + at, props_size, filters);
			if (!parsed) {
				error = "unsupported filter chain";
				return false;
			}

			at += static_cast<std::size_t>(props_size);
		}

		return filters.dictionary != 0;
	}

	static void undoDelta(std::uint8_t* data, std::size_t size, unsigned distance) {
		for (std::size_t k = distance; k < size; ++k) {
			data[k] = static_cast<std::uint8_t>(data[k] + data[k - distance]);
		}
	}

	static bool isX86PrefixByte(std::uint8_t byte) {
		return byte == 0x00 || byte == 0xFF;
	}

	static std::uint32_t x86Destination(std::uint32_t source, std::size_t position,
	                                    std::uint32_t mask) {
		static const unsigned kMaskToBits[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
		for (;;) {
			const std::uint32_t destination = source - static_cast<std::uint32_t>(position + 5);
			if (mask == 0) return destination;

			const unsigned shift = kMaskToBits[mask] * 8;
			if (!isX86PrefixByte(static_cast<std::uint8_t>(destination >> (24 - shift)))) {
				return destination;
			}

			source = destination ^ ((1u << (32 - shift)) - 1);
		}
	}

	static bool x86MaskRejects(std::uint32_t mask, const std::uint8_t* call) {
		static const bool kAllowed[8] = { true, true, true, false, true, false, false, false };
		static const unsigned kMaskToBits[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
		if (mask == 0) return false;
		return !kAllowed[mask] || isX86PrefixByte(call[4 - kMaskToBits[mask]]);
	}

	// The BCJ x86 filter turned relative CALL / JMP targets into absolute
	// ones; this mirrors the reference decoder (xz-embedded's bcj_x86),
	// whose previous-position bookkeeping starts at "minus one".
	static void undoX86(std::uint8_t* data, std::size_t size) {
		std::uint32_t mask = 0;
		std::size_t previous = ~static_cast<std::size_t>(0);
		for (std::size_t k = 0; k + 4 < size; ++k) {
			if ((data[k] & 0xFE) != 0xE8) continue;

			const std::size_t gap = k - previous;
			previous = k;
			mask = gap > 3 ? 0 : (mask << (gap - 1)) & 7;
			if (x86MaskRejects(mask, data + k) || !isX86PrefixByte(data[k + 4])) {
				mask = (mask << 1) | 1;
				continue;
			}

			std::uint32_t destination = x86Destination(readLe32(data + k + 1), k, mask);
			destination &= 0x01FFFFFF;
			destination |= 0u - (destination & 0x01000000);
			for (int byte = 0; byte < 4; ++byte) {
				data[k + 1 + byte] = static_cast<std::uint8_t>(destination >> (8 * byte));
			}

			k += 4;
		}
	}

	static bool lzma2Chunk(const std::uint8_t* in, std::size_t size, std::size_t& at,
	                       LzmaState& lzma, Window& window, std::uint8_t control) {
		if (at + 4 > size) return false;
		const std::size_t unpacked = ((static_cast<std::size_t>(control & 0x1F) << 16)
			| (static_cast<std::size_t>(in[at]) << 8) | in[at + 1]) + 1;
		const std::size_t packed = ((static_cast<std::size_t>(in[at + 2]) << 8) | in[at + 3]) + 1;
		at += 4;

		const unsigned reset = (control >> 5) & 3;
		if (reset >= 2) {
			if (at >= size || !decodePropertiesByte(in[at++], lzma.props)) return false;
			if (lzma.props.lc + lzma.props.lp > 4) return false;
		}

		if (reset >= 1) resetLzmaState(lzma);
		if (at + packed > size) return false;

		RangeDecoder rc;
		rc.data = in + at;
		rc.size = packed;
		if (!rc.start()) return false;

		const DecodeEnd end = decodeSymbols(rc, lzma, window, window.out->size() + unpacked);
		at += packed;
		return end == DecodeEnd::Limit;
	}

	static bool decodeLzma2(const std::uint8_t* in, std::size_t size, std::size_t& at,
	                        std::uint32_t dictionary, std::vector<std::uint8_t>& out) {
		LzmaState lzma;
		Window window;
		window.out = &out;
		window.dictionary_start = out.size();
		window.dictionary_size = dictionary;
		bool need_properties = true;
		for (;;) {
			if (at >= size) return false;
			const std::uint8_t control = in[at++];
			if (control == 0x00) return true;
			if (control == 0x01 || control >= 0xE0) window.dictionary_start = out.size();

			if (control < 0x80) {
				if (control > 0x02 || at + 2 > size) return false;
				const std::size_t high = static_cast<std::size_t>(in[at]) << 8;
				const std::size_t length = (high | in[at + 1]) + 1;
				at += 2;
				if (at + length > size) return false;
				out.insert(out.end(), in + at, in + at + length);
				at += length;
				continue;
			}

			if (need_properties && control < 0xC0) return false;
			need_properties = false;
			if (!lzma2Chunk(in, size, at, lzma, window, control)) return false;
		}
	}

	static std::size_t checkSize(unsigned check) {
		static const std::size_t kSizes[16] = {
			0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64,
		};
		return kSizes[check & 15];
	}

	static bool verifyCheck(unsigned check, const std::uint8_t* data, std::size_t size,
	                        const std::uint8_t* stored) {
		if (check == 1) return crc32Of(data, size) == readLe32(stored);
		if (check != 4) return true;

		std::uint64_t expected = 0;
		for (int k = 7; k >= 0; --k) expected = (expected << 8) | stored[k];
		return crc64Of(data, size) == expected;
	}

	static bool decodeBlock(const std::uint8_t* in, std::size_t size, std::size_t& at,
	                        unsigned check,
	                        std::vector<std::uint8_t>& out, std::string& error) {
		const std::size_t header_size = (static_cast<std::size_t>(in[at]) + 1) * 4;
		if (at + header_size > size) return false;

		BlockFilters filters;
		if (!parseBlockHeader(in + at, header_size, filters, error)) return false;
		at += header_size;

		const std::size_t start = out.size();
		const std::size_t data_start = at;
		if (!decodeLzma2(in, size, at, filters.dictionary, out)) {
			error = "data is corrupt";
			return false;
		}

		std::uint8_t* produced = out.data() + start;
		const std::size_t produced_size = out.size() - start;
		if (filters.delta) undoDelta(produced, produced_size, filters.delta_distance);
		if (filters.x86) undoX86(out.data() + start, out.size() - start);

		while ((at - data_start) % 4 != 0) {
			if (at >= size || in[at++] != 0) return false;
		}

		const std::size_t check_bytes = checkSize(check);
		if (at + check_bytes > size) return false;
		if (!verifyCheck(check, out.data() + start, out.size() - start, in + at)) {
			error = "checksum mismatch";
			return false;
		}

		at += check_bytes;
		return true;
	}

	static bool skipIndexAndFooter(const std::uint8_t* in, std::size_t size, std::size_t& at) {
		const std::size_t index_start = at++;
		std::uint64_t records = 0;
		if (!readVli(in, size, at, records)) return false;
		for (std::uint64_t k = 0; k < records * 2; ++k) {
			std::uint64_t ignored = 0;
			if (!readVli(in, size, at, ignored)) return false;
		}

		while ((at - index_start) % 4 != 0) ++at;
		at += 4;
		if (at + 12 > size || in[at + 10] != 'Y' || in[at + 11] != 'Z') return false;
		at += 12;
		return true;
	}

	static bool decodeStream(const std::uint8_t* in, std::size_t size, std::size_t& at,
	                         std::vector<std::uint8_t>& out, std::string& error) {
		if (at + kStreamHeaderSize > size || std::memcmp(in + at, kXzMagic, 6) != 0) {
			error = "not in xz format";
			return false;
		}

		if (crc32Of(in + at + 6, 2) != readLe32(in + at + 8)) {
			error = "stream header checksum mismatch";
			return false;
		}

		const unsigned check = in[at + 7] & 0x0F;
		at += kStreamHeaderSize;
		while (at < size && in[at] != 0x00) {
			if (!decodeBlock(in, size, at, check, out, error)) {
				if (error.empty()) error = "data is corrupt";
				return false;
			}
		}

		if (at < size && skipIndexAndFooter(in, size, at)) return true;
		error = "truncated or corrupt index";
		return false;
	}

	bool unxzData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	              std::string& error) {
		std::size_t at = 0;
		do {
			if (!decodeStream(in, size, at, out, error)) return false;
			while (at + 4 <= size && readLe32(in + at) == 0) at += 4;
		} while (at < size);

		return true;
	}

}  // namespace wbsh
