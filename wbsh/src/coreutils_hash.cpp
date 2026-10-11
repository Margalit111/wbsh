/**
 * @file coreutils_hash.cpp
 * @brief Checksum and hash builtins: md5sum, sha1sum, sha256sum,
 *        sha384sum, sha512sum, sha3sum, cksum, sum, crc32.
 *
 * MD5 and the SHA-1 / SHA-2 family come from BCrypt; SHA-3 is a local
 * Keccak because BCrypt only gained it in recent Windows 11 builds.
 * Every *sum command shares the GNU output and `-c` verification format.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif

namespace wbsh {

	namespace hash_detail {

		typedef bool (*DigestFn)(std::FILE* stream, std::vector<unsigned char>& digest,
		                         int variant);

		struct HashCommand {
			const char* name;
			DigestFn digest;
			int variant;
		};

		struct HashOptions {
			bool check = false;
			bool status_only = false;
			bool quiet = false;
			bool warn = false;
			int variant = 0;
			std::vector<std::string> files;
		};

		struct CheckTally {
			int mismatched = 0;
			int unreadable = 0;
			int malformed = 0;
		};

		struct KeccakState {
			std::uint64_t lanes[25] = {};
			std::size_t rate = 0;
			std::size_t filled = 0;
		};

	}  // namespace hash_detail

	using hash_detail::CheckTally;
	using hash_detail::DigestFn;
	using hash_detail::HashCommand;
	using hash_detail::HashOptions;
	using hash_detail::KeccakState;

	static const std::size_t kHashChunk = 64 * 1024;
	static const int kSha3DefaultBits = 224;
	static const std::uint32_t kCksumPolynomial = 0x04C11DB7u;

	static const std::uint64_t kKeccakRoundConstants[24] = {
		0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
		0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
		0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
		0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
		0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
		0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
		0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
		0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
	};

	static const int kKeccakRotations[25] = {
		0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39,
		41, 45, 15, 21, 8, 18, 2, 61, 56, 14,
	};

	static bool readChunk(std::FILE* stream, std::vector<unsigned char>& chunk, std::size_t& got) {
		got = std::fread(chunk.data(), 1, chunk.size(), stream);
		return got != 0;
	}

#ifdef _WIN32
	static LPCWSTR bcryptAlgorithm(int variant) {
		switch (variant) {
		case 5:   return BCRYPT_MD5_ALGORITHM;
		case 1:   return BCRYPT_SHA1_ALGORITHM;
		case 256: return BCRYPT_SHA256_ALGORITHM;
		case 384: return BCRYPT_SHA384_ALGORITHM;
		case 512: return BCRYPT_SHA512_ALGORITHM;
		default:  return nullptr;
		}
	}

	static DWORD bcryptHashLength(BCRYPT_ALG_HANDLE algorithm) {
		DWORD length = 0;
		DWORD written = 0;
		::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0);
		return length;
	}

	static void bcryptHashStream(BCRYPT_HASH_HANDLE hash, std::FILE* stream) {
		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		while (readChunk(stream, chunk, got)) {
			::BCryptHashData(hash, chunk.data(), static_cast<ULONG>(got), 0);
		}
	}

	static bool bcryptDigest(std::FILE* stream, std::vector<unsigned char>& digest, int variant) {
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		const LPCWSTR algorithm_id = bcryptAlgorithm(variant);
		if (::BCryptOpenAlgorithmProvider(&algorithm, algorithm_id, nullptr, 0) != 0) return false;

		BCRYPT_HASH_HANDLE hash = nullptr;
		if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
			::BCryptCloseAlgorithmProvider(algorithm, 0);
			return false;
		}

		bcryptHashStream(hash, stream);
		digest.assign(bcryptHashLength(algorithm), 0);
		::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
		::BCryptDestroyHash(hash);
		::BCryptCloseAlgorithmProvider(algorithm, 0);
		return std::ferror(stream) == 0;
	}
#else
	static bool bcryptDigest(std::FILE*, std::vector<unsigned char>&, int) {
		return false;
	}
#endif /* _WIN32 */

	static std::uint64_t rotateLeft(std::uint64_t value, int shift) {
		if (shift == 0) return value;
		return (value << shift) | (value >> (64 - shift));
	}

	static void keccakThetaRhoPi(std::uint64_t (&lanes)[25], std::uint64_t (&moved)[25]) {
		std::uint64_t columns[5];
		for (int x = 0; x < 5; ++x) {
			columns[x] = lanes[x] ^ lanes[x + 5] ^ lanes[x + 10] ^ lanes[x + 15] ^ lanes[x + 20];
		}

		for (int x = 0; x < 5; ++x) {
			const std::uint64_t mix = columns[(x + 4) % 5] ^ rotateLeft(columns[(x + 1) % 5], 1);
			for (int y = 0; y < 25; y += 5) lanes[y + x] ^= mix;
		}

		for (int x = 0; x < 5; ++x) {
			for (int y = 0; y < 5; ++y) {
				const int from = x + 5 * y;
				const int to = y + 5 * ((2 * x + 3 * y) % 5);
				moved[to] = rotateLeft(lanes[from], kKeccakRotations[from]);
			}
		}
	}

	static void keccakPermute(std::uint64_t (&lanes)[25]) {
		std::uint64_t moved[25];
		for (const std::uint64_t round_constant : kKeccakRoundConstants) {
			keccakThetaRhoPi(lanes, moved);
			for (int y = 0; y < 25; y += 5) {
				for (int x = 0; x < 5; ++x) {
					const std::uint64_t next = moved[y + (x + 1) % 5];
					lanes[y + x] = moved[y + x] ^ (~next & moved[y + (x + 2) % 5]);
				}
			}

			lanes[0] ^= round_constant;
		}
	}

	static void keccakAbsorbByte(KeccakState& state, unsigned char byte) {
		const int shift = static_cast<int>(8 * (state.filled % 8));
		state.lanes[state.filled / 8] ^= static_cast<std::uint64_t>(byte) << shift;
		if (++state.filled < state.rate) return;

		keccakPermute(state.lanes);
		state.filled = 0;
	}

	static unsigned char keccakLaneByte(const KeccakState& state, std::size_t index) {
		return static_cast<unsigned char>(state.lanes[index / 8] >> (8 * (index % 8)));
	}

	static bool sha3Digest(std::FILE* stream, std::vector<unsigned char>& digest, int bits) {
		KeccakState state;
		const std::size_t digest_bytes = static_cast<std::size_t>(bits) / 8;
		state.rate = 200 - 2 * digest_bytes;

		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		while (readChunk(stream, chunk, got)) {
			for (std::size_t i = 0; i < got; ++i) keccakAbsorbByte(state, chunk[i]);
		}

		state.lanes[state.filled / 8] ^= 0x06ULL << (8 * (state.filled % 8));
		state.lanes[(state.rate - 1) / 8] ^= 0x80ULL << (8 * ((state.rate - 1) % 8));
		keccakPermute(state.lanes);

		digest.resize(digest_bytes);
		for (std::size_t i = 0; i < digest_bytes; ++i) digest[i] = keccakLaneByte(state, i);
		return std::ferror(stream) == 0;
	}

	static std::string toHex(const std::vector<unsigned char>& bytes) {
		static const char kDigits[] = "0123456789abcdef";
		std::string hex;
		hex.reserve(bytes.size() * 2);
		for (const unsigned char byte : bytes) {
			hex.push_back(kDigits[byte >> 4]);
			hex.push_back(kDigits[byte & 0x0F]);
		}

		return hex;
	}

	static std::FILE* openHashInput(Executor& exec, const std::string& file) {
		if (file == "-") return stdin;
		return fopenNative(exec, file, "rb");
	}

	static bool hashFileHex(Executor& exec, const std::string& file, const HashCommand& command,
	                        int variant, std::string& hex) {
		std::FILE* stream = openHashInput(exec, file);
		if (stream == nullptr) return false;

		std::vector<unsigned char> digest;
		const bool ok = command.digest(stream, digest, variant);
		if (stream != stdin) std::fclose(stream);
		if (ok) hex = toHex(digest);
		return ok;
	}

	static bool isIgnoredHashFlag(const std::string& arg) {
		return arg == "-b" || arg == "-t" || arg == "--binary" || arg == "--text"
			|| arg == "--strict";
	}

	static bool applyHashFlag(const std::vector<std::string>& args, std::size_t& i,
	                          HashOptions& options) {
		const std::string& arg = args[i];
		if (arg == "-a" && i + 1 < args.size()) return parseInt(args[++i], options.variant);
		if (arg.size() > 2 && arg.compare(0, 2, "-a") == 0) {
			return parseInt(arg.substr(2), options.variant);
		}

		if (arg == "-c" || arg == "--check")       options.check = true;
		else if (arg == "-s" || arg == "--status") options.status_only = true;
		else if (arg == "-q" || arg == "--quiet")  options.quiet = true;
		else if (arg == "-w" || arg == "--warn")   options.warn = true;
		else return isIgnoredHashFlag(arg);
		return true;
	}

	static bool parseHashArgs(const std::vector<std::string>& args, const char* cmd,
	                          HashOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			if (applyHashFlag(args, i, options)) continue;
			perr(cmd, "unknown option: " + arg);
			return false;
		}

		if (options.files.empty()) options.files.push_back("-");
		return true;
	}

	static bool splitCheckLine(const std::string& line, std::string& hex, std::string& file) {
		const std::size_t space = line.find(' ');
		if (space == std::string::npos || space == 0 || space + 1 >= line.size()) return false;

		hex = line.substr(0, space);
		std::size_t name_start = space + 1;
		if (line[name_start] == ' ' || line[name_start] == '*') ++name_start;
		file = line.substr(name_start);
		return !file.empty();
	}

	static bool sameHex(const std::string& actual, const std::string& expected) {
		if (actual.size() != expected.size()) return false;
		for (std::size_t i = 0; i < actual.size(); ++i) {
			const int wanted = std::tolower(static_cast<unsigned char>(expected[i]));
			if (actual[i] != wanted) return false;
		}

		return true;
	}

	static void verifyCheckLine(Executor& exec, const std::string& line,
	                            const HashCommand& command, const HashOptions& options,
	                            CheckTally& tally) {
		std::string expected;
		std::string file;
		if (!splitCheckLine(line, expected, file)) {
			++tally.malformed;
			return;
		}

		std::string actual;
		if (!hashFileHex(exec, file, command, options.variant, actual)) {
			++tally.unreadable;
			if (!options.status_only) std::printf("%s: FAILED open or read\n", file.c_str());
			return;
		}

		const bool match = sameHex(actual, expected);
		if (!match) ++tally.mismatched;
		if (options.status_only || (match && options.quiet)) return;
		std::printf("%s: %s\n", file.c_str(), match ? "OK" : "FAILED");
	}

	static void reportCheckTally(const char* cmd, const CheckTally& tally,
	                             const HashOptions& options) {
		if (options.status_only) return;

		std::fflush(stdout);
		if (tally.malformed > 0 && options.warn) {
			std::fprintf(stderr, "%s: WARNING: %d line%s improperly formatted\n", cmd,
				tally.malformed, tally.malformed == 1 ? " is" : "s are");
		}

		if (tally.unreadable > 0) {
			std::fprintf(stderr, "%s: WARNING: %d listed file%s could not be read\n", cmd,
				tally.unreadable, tally.unreadable == 1 ? "" : "s");
		}

		if (tally.mismatched > 0) {
			std::fprintf(stderr, "%s: WARNING: %d computed checksum%s did NOT match\n", cmd,
				tally.mismatched, tally.mismatched == 1 ? "" : "s");
		}
	}

	static int checkHashLists(Executor& exec, const HashCommand& command,
	                          const HashOptions& options) {
		CheckTally tally;
		for (const std::string& list : options.files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, list, lines)) {
				perr(command.name, list + ": " + std::strerror(errno));
				return 1;
			}

			for (const std::string& line : lines) {
				if (!line.empty()) verifyCheckLine(exec, line, command, options, tally);
			}
		}

		reportCheckTally(command.name, tally, options);
		std::fflush(stdout);
		return tally.mismatched + tally.unreadable == 0 ? 0 : 1;
	}

	static int printHashes(Executor& exec, const HashCommand& command,
	                       const HashOptions& options) {
		int status = 0;
		for (const std::string& file : options.files) {
			std::string hex;
			if (!hashFileHex(exec, file, command, options.variant, hex)) {
				perr(command.name, file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			std::printf("%s  %s\n", hex.c_str(), file.c_str());
		}

		std::fflush(stdout);
		return status;
	}

	static int runHashCommand(Executor& exec, const std::vector<std::string>& args,
	                          const HashCommand& command) {
		HashOptions options;
		options.variant = command.variant;
		if (!parseHashArgs(args, command.name, options)) return 1;

		const bool sha3 = command.digest == sha3Digest;
		const int bits = options.variant;
		if (sha3 && bits != 224 && bits != 256 && bits != 384 && bits != 512) {
			perr(command.name, "invalid number of bits: " + std::to_string(bits));
			return 1;
		}

		if (options.check) return checkHashLists(exec, command, options);
		return printHashes(exec, command, options);
	}

	static const HashCommand kMd5Command    = { "md5sum",    bcryptDigest, 5   };
	static const HashCommand kSha1Command   = { "sha1sum",   bcryptDigest, 1   };
	static const HashCommand kSha256Command = { "sha256sum", bcryptDigest, 256 };
	static const HashCommand kSha384Command = { "sha384sum", bcryptDigest, 384 };
	static const HashCommand kSha512Command = { "sha512sum", bcryptDigest, 512 };
	static const HashCommand kSha3Command   = { "sha3sum",   sha3Digest,   kSha3DefaultBits };

	static int builtin_md5sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kMd5Command);
	}

	static int builtin_sha1sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kSha1Command);
	}

	static int builtin_sha256sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kSha256Command);
	}

	static int builtin_sha384sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kSha384Command);
	}

	static int builtin_sha512sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kSha512Command);
	}

	static int builtin_sha3sum(Executor& exec, const std::vector<std::string>& args) {
		return runHashCommand(exec, args, kSha3Command);
	}

	static void buildCksumTable(std::uint32_t (&table)[256]) {
		for (std::uint32_t i = 0; i < 256; ++i) {
			std::uint32_t entry = i << 24;
			for (int k = 0; k < 8; ++k) {
				entry = (entry & 0x80000000u) != 0 ? (entry << 1) ^ kCksumPolynomial : entry << 1;
			}

			table[i] = entry;
		}
	}

	static std::uint32_t cksumUpdate(std::uint32_t crc, unsigned char byte) {
		static std::uint32_t table[256];
		static bool built = false;
		if (!built) {
			buildCksumTable(table);
			built = true;
		}

		return (crc << 8) ^ table[((crc >> 24) ^ byte) & 0xFF];
	}

	// POSIX cksum: the CRC runs over the data and then over the length,
	// least significant byte first, without its leading zero bytes.
	static std::uint32_t posixCksum(std::FILE* stream, unsigned long long& length) {
		std::uint32_t crc = 0;
		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		length = 0;
		while (readChunk(stream, chunk, got)) {
			for (std::size_t i = 0; i < got; ++i) crc = cksumUpdate(crc, chunk[i]);
			length += got;
		}

		for (unsigned long long remaining = length; remaining != 0; remaining >>= 8) {
			crc = cksumUpdate(crc, static_cast<unsigned char>(remaining & 0xFF));
		}

		return ~crc;
	}

	static std::uint32_t bsdSum(std::FILE* stream, unsigned long long& length) {
		std::uint32_t checksum = 0;
		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		length = 0;
		while (readChunk(stream, chunk, got)) {
			for (std::size_t i = 0; i < got; ++i) {
				checksum = (checksum >> 1) + ((checksum & 1) << 15);
				checksum = (checksum + chunk[i]) & 0xFFFF;
			}

			length += got;
		}

		return checksum;
	}

	static std::uint32_t sysvSum(std::FILE* stream, unsigned long long& length) {
		std::uint64_t total = 0;
		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		length = 0;
		while (readChunk(stream, chunk, got)) {
			for (std::size_t i = 0; i < got; ++i) total += chunk[i];
			length += got;
		}

		std::uint32_t folded = static_cast<std::uint32_t>((total & 0xFFFF) + (total >> 16));
		folded = (folded & 0xFFFF) + (folded >> 16);
		return folded;
	}

	static std::uint32_t zlibCrc32(std::FILE* stream, unsigned long long& length) {
		std::uint32_t crc = 0;
		std::vector<unsigned char> chunk(kHashChunk);
		std::size_t got = 0;
		length = 0;
		while (readChunk(stream, chunk, got)) {
			crc = crc32Update(crc, chunk.data(), got);
			length += got;
		}

		return crc;
	}

	static std::vector<std::string> plainOperands(const std::vector<std::string>& args,
	                                              bool& sysv) {
		std::vector<std::string> files;
		for (const std::string& arg : args) {
			if (arg == "-s" || arg == "--sysv") sysv = true;
			else if (arg == "-r")               sysv = false;
			else                                files.push_back(arg);
		}

		return files;
	}

	static int builtin_cksum(Executor& exec, const std::vector<std::string>& args) {
		bool ignored = false;
		std::vector<std::string> files = plainOperands(args, ignored);
		const bool named = !files.empty();
		if (!named) files.push_back("-");

		int status = 0;
		for (const std::string& file : files) {
			std::FILE* stream = openHashInput(exec, file);
			if (stream == nullptr) {
				perr("cksum", file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			unsigned long long length = 0;
			const std::uint32_t crc = posixCksum(stream, length);
			if (stream != stdin) std::fclose(stream);
			std::printf("%lu %llu", static_cast<unsigned long>(crc), length);
			if (named) std::printf(" %s", file.c_str());
			std::printf("\n");
		}

		std::fflush(stdout);
		return status;
	}

	static void printSumLine(std::uint32_t checksum, unsigned long long length, bool sysv,
	                         const std::string& file, bool named) {
		const unsigned long long block = sysv ? 512 : 1024;
		const unsigned long long blocks = (length + block - 1) / block;
		if (sysv) std::printf("%lu %llu", static_cast<unsigned long>(checksum), blocks);
		else      std::printf("%05lu %5llu", static_cast<unsigned long>(checksum), blocks);
		if (named) std::printf(" %s", file.c_str());
		std::printf("\n");
	}

	static int builtin_sum(Executor& exec, const std::vector<std::string>& args) {
		bool sysv = false;
		std::vector<std::string> files = plainOperands(args, sysv);
		const bool named = files.size() > 1 || (sysv && !files.empty());
		if (files.empty()) files.push_back("-");

		int status = 0;
		for (const std::string& file : files) {
			std::FILE* stream = openHashInput(exec, file);
			if (stream == nullptr) {
				perr("sum", file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			unsigned long long length = 0;
			const std::uint32_t checksum = sysv ? sysvSum(stream, length) : bsdSum(stream, length);
			if (stream != stdin) std::fclose(stream);
			printSumLine(checksum, length, sysv, file, named);
		}

		std::fflush(stdout);
		return status;
	}

	static int builtin_crc32(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> files = args;
		const bool named = files.size() > 1;
		if (files.empty()) files.push_back("-");

		int status = 0;
		for (const std::string& file : files) {
			std::FILE* stream = openHashInput(exec, file);
			if (stream == nullptr) {
				perr("crc32", file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			unsigned long long length = 0;
			const std::uint32_t crc = zlibCrc32(stream, length);
			if (stream != stdin) std::fclose(stream);
			if (named) std::printf("%08lx %s\n", static_cast<unsigned long>(crc), file.c_str());
			else       std::printf("%08lx\n", static_cast<unsigned long>(crc));
		}

		std::fflush(stdout);
		return status;
	}

	void registerHashBuiltins(Executor& exec) {
		exec.registerBuiltin("md5sum",    builtin_md5sum);
		exec.registerBuiltin("sha1sum",   builtin_sha1sum);
		exec.registerBuiltin("sha256sum", builtin_sha256sum);
		exec.registerBuiltin("sha384sum", builtin_sha384sum);
		exec.registerBuiltin("sha512sum", builtin_sha512sum);
		exec.registerBuiltin("sha3sum",   builtin_sha3sum);
		exec.registerBuiltin("cksum",     builtin_cksum);
		exec.registerBuiltin("sum",       builtin_sum);
		exec.registerBuiltin("crc32",     builtin_crc32);
	}

}  // namespace wbsh
