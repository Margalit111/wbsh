/**
 * @file coreutils_fileops.cpp
 * @brief File-content utilities: dd, split, truncate, shred, link,
 *        unlink, sync, fsync, dos2unix, unix2dos.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace fileops_detail {

		enum class DdStatus {
			Default,
			None,
			NoTransfer,
			Progress,
		};

		struct DdOptions {
			std::string input = "-";
			std::string output = "-";
			unsigned long long input_block = 512;
			unsigned long long output_block = 512;
			unsigned long long count = ~0ULL;
			unsigned long long skip = 0;
			unsigned long long seek = 0;
			bool count_bytes = false;
			bool skip_bytes = false;
			bool seek_bytes = false;
			bool append = false;
			bool no_truncate = false;
			bool no_error = false;
			bool pad_blocks = false;
			bool upper_case = false;
			bool lower_case = false;
			bool sync_at_end = false;
			DdStatus status = DdStatus::Default;
		};

		struct DdTally {
			unsigned long long full_in = 0;
			unsigned long long partial_in = 0;
			unsigned long long full_out = 0;
			unsigned long long partial_out = 0;
			unsigned long long bytes = 0;
		};

		enum class SplitMode {
			Lines,
			Bytes,
			LineBytes,
			Chunks,
		};

		struct SplitOptions {
			SplitMode mode = SplitMode::Lines;
			unsigned long long amount = 1000;
			std::size_t suffix_length = 2;
			bool numeric = false;
			bool verbose = false;
			std::string extra_suffix;
			std::string input = "-";
			std::string prefix = "x";
		};

		struct SplitWriter {
			std::FILE* file = nullptr;
			unsigned long long index = 0;
			unsigned long long used = 0;
			bool failed = false;
		};

		enum class SizeAdjust {
			Set,
			Grow,
			Shrink,
			AtMost,
			AtLeast,
			RoundDown,
			RoundUp,
		};

		struct TruncateOptions {
			SizeAdjust adjust = SizeAdjust::Set;
			unsigned long long size = 0;
			bool have_size = false;
			bool no_create = false;
			std::string reference;
			std::vector<std::string> files;
		};

		struct ShredOptions {
			int passes = 3;
			bool zero_pass = false;
			bool remove = false;
			bool verbose = false;
			bool force = false;
			unsigned long long size = 0;
			bool have_size = false;
			std::vector<std::string> files;
		};

	}  // namespace fileops_detail

	using fileops_detail::DdOptions;
	using fileops_detail::DdStatus;
	using fileops_detail::DdTally;
	using fileops_detail::ShredOptions;
	using fileops_detail::SizeAdjust;
	using fileops_detail::SplitMode;
	using fileops_detail::SplitOptions;
	using fileops_detail::SplitWriter;
	using fileops_detail::TruncateOptions;

	static const std::size_t kCopyChunk      = 64 * 1024;
	static const std::size_t kShredChunk     = 64 * 1024;
	static const int         kDdUsageError   = 1;

	static std::string errnoText() {
		return std::strerror(errno);
	}

	static bool flushToDisk(std::FILE* file) {
		return ::FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)))) != 0;
	}

	static bool applyDdConversion(const std::string& name, DdOptions& options) {
		if (name == "notrunc")                       options.no_truncate = true;
		else if (name == "noerror")                  options.no_error = true;
		else if (name == "sync")                     options.pad_blocks = true;
		else if (name == "ucase")                    options.upper_case = true;
		else if (name == "lcase")                    options.lower_case = true;
		else if (name == "fsync" || name == "fdatasync") options.sync_at_end = true;
		else return name == "excl" || name == "nocreat";
		return true;
	}

	static bool applyDdFlag(const std::string& name, DdOptions& options, bool input_side) {
		if (input_side && name == "count_bytes")      options.count_bytes = true;
		else if (input_side && name == "skip_bytes")  options.skip_bytes = true;
		else if (!input_side && name == "seek_bytes") options.seek_bytes = true;
		else if (!input_side && name == "append")     options.append = true;
		else return name == "fullblock" || name == "binary" || name == "direct" || name == "sync";
		return true;
	}

	static bool applyDdList(const std::string& list, DdOptions& options, char kind) {
		std::size_t start = 0;
		for (;;) {
			const std::size_t comma = list.find(',', start);
			const std::string item = list.substr(start, comma == std::string::npos
				? std::string::npos : comma - start);
			const bool ok = kind == 'c' ? applyDdConversion(item, options)
				: applyDdFlag(item, options, kind == 'i');
			if (!ok) {
				perr("dd", "invalid flag: '" + item + "'");
				return false;
			}

			if (comma == std::string::npos) return true;
			start = comma + 1;
		}
	}

	static bool applyDdStatus(const std::string& value, DdOptions& options) {
		if (value == "none")          options.status = DdStatus::None;
		else if (value == "noxfer")   options.status = DdStatus::NoTransfer;
		else if (value == "progress") options.status = DdStatus::Progress;
		else return false;
		return true;
	}

	static bool applyDdSize(const std::string& key, const std::string& value, DdOptions& options) {
		unsigned long long amount = 0;
		if (!parseByteSize(value, amount)) return false;

		if (key == "bs") {
			options.input_block = amount;
			options.output_block = amount;
		}

		if (key == "ibs")   options.input_block = amount;
		if (key == "obs")   options.output_block = amount;
		if (key == "count") options.count = amount;
		if (key == "skip")  options.skip = amount;
		if (key == "seek")  options.seek = amount;
		return true;
	}

	static bool isDdSizeKey(const std::string& key) {
		return key == "bs" || key == "ibs" || key == "obs" || key == "count" || key == "skip"
			|| key == "seek" || key == "iseek" || key == "oseek";
	}

	static bool applyDdOperand(const std::string& key, const std::string& value,
	                           DdOptions& options) {
		if (key == "if")          options.input = value;
		else if (key == "of")     options.output = value;
		else if (key == "conv")   return applyDdList(value, options, 'c');
		else if (key == "iflag")  return applyDdList(value, options, 'i');
		else if (key == "oflag")  return applyDdList(value, options, 'o');
		else if (key == "status") return applyDdStatus(value, options);
		else if (key == "iseek")  return applyDdSize("skip", value, options);
		else if (key == "oseek")  return applyDdSize("seek", value, options);
		else if (isDdSizeKey(key)) return applyDdSize(key, value, options);
		else return false;
		return true;
	}

	static bool parseDdArgs(const std::vector<std::string>& args, DdOptions& options) {
		for (const std::string& arg : args) {
			const std::size_t eq = arg.find('=');
			const bool applied = eq != std::string::npos
				&& applyDdOperand(arg.substr(0, eq), arg.substr(eq + 1), options);
			if (applied) continue;

			perr("dd", "invalid operand '" + arg + "'");
			return false;
		}

		if (options.input_block != 0 && options.output_block != 0) return true;
		perr("dd", "invalid block size");
		return false;
	}

	static std::size_t chunkFor(unsigned long long remaining, std::size_t chunk) {
		return remaining < chunk ? static_cast<std::size_t>(remaining) : chunk;
	}

	static void discardBytes(std::FILE* stream, unsigned long long count) {
		if (count == 0) return;

		const long long offset = static_cast<long long>(count);
		if (stream != stdin && _fseeki64(stream, offset, SEEK_CUR) == 0) return;

		std::vector<char> chunk(kCopyChunk);
		while (count > 0) {
			const std::size_t want = chunkFor(count, chunk.size());
			const std::size_t got = std::fread(chunk.data(), 1, want, stream);
			if (got == 0) return;
			count -= got;
		}
	}

	static std::FILE* openDdOutput(Executor& exec, const DdOptions& options,
	                               unsigned long long offset) {
		if (options.output == "-") return stdout;
		if (options.append) return fopenNative(exec, options.output, "ab");

		const bool keep_existing = options.no_truncate || offset > 0;
		std::FILE* file = keep_existing ? fopenNative(exec, options.output, "r+b") : nullptr;
		if (file == nullptr) file = fopenNative(exec, options.output, "wb");
		if (file == nullptr) return nullptr;

		if (offset > 0) _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
		if (offset > 0 && !options.no_truncate) {
			_chsize_s(_fileno(file), static_cast<long long>(offset));
		}

		return file;
	}

	static void convertDdBlock(std::vector<char>& block, std::size_t size,
							   const DdOptions& options) {
		for (std::size_t i = 0; i < size; ++i) {
			const unsigned char byte = static_cast<unsigned char>(block[i]);
			if (options.upper_case) block[i] = static_cast<char>(std::toupper(byte));
			if (options.lower_case) block[i] = static_cast<char>(std::tolower(byte));
		}
	}

	static std::string humanRate(double bytes_per_second) {
		static const char* const kUnits[] = { "B", "kB", "MB", "GB", "TB" };
		std::size_t unit = 0;
		while (bytes_per_second >= 1000.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
			bytes_per_second /= 1000.0;
			++unit;
		}

		char text[32];
		std::snprintf(text, sizeof(text), "%.1f %s", bytes_per_second, kUnits[unit]);
		return text;
	}

	static void reportDdTransfer(const DdTally& tally, double seconds, const char* end) {
		const double rate = seconds > 0.0 ? static_cast<double>(tally.bytes) / seconds : 0.0;
		std::fprintf(stderr, "%llu bytes copied, %.6f s, %s/s%s", tally.bytes, seconds,
			humanRate(rate).c_str(), end);
	}

	static void reportDdTally(const DdTally& tally, const DdOptions& options, double seconds) {
		if (options.status == DdStatus::None) return;
		if (options.status == DdStatus::Progress) std::fputc('\n', stderr);

		std::fprintf(stderr, "%llu+%llu records in\n%llu+%llu records out\n",
			tally.full_in, tally.partial_in, tally.full_out, tally.partial_out);
		if (options.status != DdStatus::NoTransfer) reportDdTransfer(tally, seconds, "\n");
	}

	static std::size_t nextDdReadSize(const DdOptions& options, const DdTally& tally) {
		if (!options.count_bytes) return static_cast<std::size_t>(options.input_block);
		const unsigned long long left = options.count - tally.bytes;
		return static_cast<std::size_t>(std::min(left, options.input_block));
	}

	static bool ddFinished(const DdOptions& options, const DdTally& tally) {
		if (options.count_bytes) return tally.bytes >= options.count;
		return tally.full_in + tally.partial_in >= options.count;
	}

	static void countDdRecords(DdTally& tally, std::size_t got, std::size_t written,
	                           const DdOptions& options) {
		if (got == options.input_block) ++tally.full_in;
		else                            ++tally.partial_in;

		tally.full_out += written / options.output_block;
		if (written % options.output_block != 0) ++tally.partial_out;
		tally.bytes += written;
	}

	using SteadyTime = std::chrono::steady_clock::time_point;

	static void reportProgressIfDue(const DdTally& tally, SteadyTime started, SteadyTime& last) {
		const SteadyTime now = std::chrono::steady_clock::now();
		if (now - last < std::chrono::seconds(1)) return;

		reportDdTransfer(tally, std::chrono::duration<double>(now - started).count(), "\r");
		last = now;
	}

	static bool copyDdBlocks(std::FILE* in, std::FILE* out, const DdOptions& options,
	                         DdTally& tally) {
		std::vector<char> block(static_cast<std::size_t>(options.input_block));
		const auto started = std::chrono::steady_clock::now();
		auto last_report = started;
		while (!ddFinished(options, tally)) {
			const std::size_t got = std::fread(block.data(), 1, nextDdReadSize(options, tally), in);
			if (got == 0 && std::ferror(in) != 0 && options.no_error) {
				std::clearerr(in);
				continue;
			}

			if (got == 0) break;

			std::size_t size = got;
			if (options.pad_blocks && size < block.size()) {
				std::fill(block.begin() + static_cast<std::ptrdiff_t>(size), block.end(), '\0');
				size = block.size();
			}

			convertDdBlock(block, size, options);
			if (std::fwrite(block.data(), 1, size, out) != size) return false;
			countDdRecords(tally, got, size, options);

			if (options.status != DdStatus::Progress) continue;
			reportProgressIfDue(tally, started, last_report);
		}

		return true;
	}

	static int runDd(Executor& exec, const DdOptions& options) {
		std::FILE* in = options.input == "-" ? stdin : fopenNative(exec, options.input, "rb");
		if (in == nullptr) {
			perr("dd", "failed to open '" + options.input + "': " + errnoText());
			return 1;
		}

		discardBytes(in, options.skip_bytes ? options.skip : options.skip * options.input_block);
		const unsigned long long offset = options.seek_bytes
			? options.seek : options.seek * options.output_block;
		std::FILE* out = openDdOutput(exec, options, offset);
		if (out == nullptr) {
			perr("dd", "failed to open '" + options.output + "': " + errnoText());
			if (in != stdin) std::fclose(in);
			return 1;
		}

		DdTally tally;
		const auto started = std::chrono::steady_clock::now();
		const bool ok = copyDdBlocks(in, out, options, tally);
		std::fflush(out);
		if (options.sync_at_end && out != stdout) flushToDisk(out);

		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - started).count();
		if (in != stdin) std::fclose(in);
		if (out != stdout) std::fclose(out);

		reportDdTally(tally, options, seconds);
		if (!ok) perr("dd", "error writing '" + options.output + "'");
		return ok ? 0 : 1;
	}

	static int builtin_dd(Executor& exec, const std::vector<std::string>& args) {
		DdOptions options;
		if (!parseDdArgs(args, options)) return kDdUsageError;
		return runDd(exec, options);
	}

	static bool readWholeStream(std::FILE* stream, std::string& text) {
		std::vector<char> chunk(kCopyChunk);
		for (;;) {
			const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
			if (got == 0) return std::ferror(stream) == 0;
			text.append(chunk.data(), got);
		}
	}

	static bool parseSplitAmount(const std::string& value, SplitMode mode, SplitOptions& options) {
		options.mode = mode;
		return parseByteSize(value, options.amount) && options.amount > 0;
	}

	static bool takeSplitValue(const std::vector<std::string>& args, std::size_t& i,
	                           const char* flag, std::string& value) {
		const std::string& arg = args[i];
		if (arg.size() > 2 && arg.compare(0, 2, flag) == 0) {
			value = arg.substr(2);
			return true;
		}

		if (arg != flag || i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool applySplitValueOption(const std::vector<std::string>& args, std::size_t& i,
	                                  SplitOptions& options, bool& valid) {
		static const char* const kAmountFlags[] = { "-l", "-b", "-C", "-n" };
		static const SplitMode kAmountModes[] = {
			SplitMode::Lines, SplitMode::Bytes, SplitMode::LineBytes, SplitMode::Chunks,
		};

		std::string value;
		for (std::size_t k = 0; k < sizeof(kAmountFlags) / sizeof(kAmountFlags[0]); ++k) {
			if (!takeSplitValue(args, i, kAmountFlags[k], value)) continue;
			valid = parseSplitAmount(value, kAmountModes[k], options);
			return true;
		}

		if (!takeSplitValue(args, i, "-a", value)) return false;

		unsigned long long length = 0;
		valid = parseULL(value, length) && length > 0 && length < 64;
		options.suffix_length = static_cast<std::size_t>(length);
		return true;
	}

	static bool applySplitFlag(const std::string& arg, SplitOptions& options) {
		const std::string extra = "--additional-suffix=";
		const bool has_extra = arg.compare(0, extra.size(), extra) == 0;
		const bool numeric = arg == "-d" || arg == "--numeric-suffixes";
		if (numeric)                 options.numeric = true;
		else if (arg == "--verbose") options.verbose = true;
		else if (has_extra)          options.extra_suffix = arg.substr(extra.size());
		else return false;
		return true;
	}

	static bool parseSplitArgs(const std::vector<std::string>& args, SplitOptions& options) {
		std::vector<std::string> operands;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() < 2 || arg[0] != '-') {
				operands.push_back(arg);
				continue;
			}

			bool valid = true;
			if (applySplitValueOption(args, i, options, valid)) {
				if (valid) continue;
				perr("split", "invalid argument: '" + arg + "'");
				return false;
			}

			if (applySplitFlag(arg, options)) continue;
			perr("split", "unknown option: " + arg);
			return false;
		}

		if (operands.size() > 2) {
			perr("split", "extra operand '" + operands[2] + "'");
			return false;
		}

		if (!operands.empty()) options.input = operands[0];
		if (operands.size() == 2) options.prefix = operands[1];
		return true;
	}

	static bool splitSuffix(unsigned long long index, const SplitOptions& options,
	                        std::string& suffix) {
		const unsigned long long radix = options.numeric ? 10 : 26;
		suffix.assign(options.suffix_length, options.numeric ? '0' : 'a');
		for (std::size_t k = options.suffix_length; k > 0; --k) {
			const unsigned long long digit = index % radix;
			suffix[k - 1] = static_cast<char>((options.numeric ? '0' : 'a') + digit);
			index /= radix;
		}

		return index == 0;
	}

	static bool openNextPiece(Executor& exec, const SplitOptions& options, SplitWriter& writer) {
		if (writer.file != nullptr) std::fclose(writer.file);
		writer.file = nullptr;
		writer.used = 0;

		std::string suffix;
		if (!splitSuffix(writer.index++, options, suffix)) {
			perr("split", "output file suffixes exhausted");
			writer.failed = true;
			return false;
		}

		const std::string name = options.prefix + suffix + options.extra_suffix;
		if (options.verbose) std::printf("creating file '%s'\n", name.c_str());
		writer.file = fopenNative(exec, name, "wb");
		if (writer.file != nullptr) return true;

		perr("split", name + ": " + errnoText());
		writer.failed = true;
		return false;
	}

	static bool writePiece(Executor& exec, const SplitOptions& options, SplitWriter& writer,
	                       const char* data, std::size_t size) {
		if (writer.file == nullptr && !openNextPiece(exec, options, writer)) return false;
		std::fwrite(data, 1, size, writer.file);
		writer.used += size;
		return true;
	}

	static void splitByBytes(Executor& exec, std::FILE* in, const SplitOptions& options,
	                         SplitWriter& writer) {
		std::vector<char> chunk(kCopyChunk);
		for (;;) {
			const unsigned long long room = options.amount - writer.used;
			const std::size_t got = std::fread(chunk.data(), 1, chunkFor(room, chunk.size()), in);
			if (got == 0) return;
			if (!writePiece(exec, options, writer, chunk.data(), got)) return;
			if (writer.used < options.amount) continue;
			std::fclose(writer.file);
			writer.file = nullptr;
			writer.used = 0;
		}
	}

	static bool readSplitLine(std::FILE* in, std::string& line) {
		line.clear();
		for (int c = std::fgetc(in); c != EOF; c = std::fgetc(in)) {
			line.push_back(static_cast<char>(c));
			if (c == '\n') return true;
		}

		return !line.empty();
	}

	static void closePiece(SplitWriter& writer) {
		if (writer.file != nullptr) std::fclose(writer.file);
		writer.file = nullptr;
		writer.used = 0;
	}

	static void splitByLines(Executor& exec, std::FILE* in, const SplitOptions& options,
	                         SplitWriter& writer) {
		std::string line;
		unsigned long long lines = 0;
		while (readSplitLine(in, line)) {
			if (!writePiece(exec, options, writer, line.data(), line.size())) return;
			if (++lines < options.amount) continue;
			closePiece(writer);
			lines = 0;
		}
	}

	static void splitByLineBytes(Executor& exec, std::FILE* in, const SplitOptions& options,
	                             SplitWriter& writer) {
		std::string line;
		while (readSplitLine(in, line)) {
			const bool overflows = writer.used + line.size() > options.amount;
			if (writer.file != nullptr && overflows) closePiece(writer);

			std::size_t offset = 0;
			while (line.size() - offset > options.amount) {
				const std::size_t piece = static_cast<std::size_t>(options.amount);
				if (!writePiece(exec, options, writer, line.data() + offset, piece)) return;
				closePiece(writer);
				offset += piece;
			}

			const std::size_t rest = line.size() - offset;
			if (!writePiece(exec, options, writer, line.data() + offset, rest)) return;
		}
	}

	static void splitIntoChunks(Executor& exec, std::FILE* in, const SplitOptions& options,
	                            SplitWriter& writer) {
		std::string all;
		readWholeStream(in, all);

		const unsigned long long pieces = options.amount;
		const std::size_t size = static_cast<std::size_t>(all.size() / pieces);
		for (unsigned long long k = 0; k < pieces; ++k) {
			const std::size_t start = static_cast<std::size_t>(k) * size;
			const std::size_t length = k + 1 == pieces ? all.size() - start : size;
			if (!openNextPiece(exec, options, writer)) return;
			std::fwrite(all.data() + start, 1, length, writer.file);
		}
	}

	static int builtin_split(Executor& exec, const std::vector<std::string>& args) {
		SplitOptions options;
		if (!parseSplitArgs(args, options)) return 1;

		std::FILE* in = options.input == "-" ? stdin : fopenNative(exec, options.input, "rb");
		if (in == nullptr) {
			perr("split", options.input + ": " + errnoText());
			return 1;
		}

		SplitWriter writer;
		switch (options.mode) {
		case SplitMode::Lines:     splitByLines(exec, in, options, writer);     break;
		case SplitMode::Bytes:     splitByBytes(exec, in, options, writer);     break;
		case SplitMode::LineBytes: splitByLineBytes(exec, in, options, writer); break;
		case SplitMode::Chunks:    splitIntoChunks(exec, in, options, writer);  break;
		default:                   break;
		}

		closePiece(writer);
		if (in != stdin) std::fclose(in);
		std::fflush(stdout);
		return writer.failed ? 1 : 0;
	}

	static bool parseTruncateSize(const std::string& text, TruncateOptions& options) {
		static const char kPrefixes[] = "+-<>/%";
		static const SizeAdjust kAdjusts[] = {
			SizeAdjust::Grow, SizeAdjust::Shrink, SizeAdjust::AtMost,
			SizeAdjust::AtLeast, SizeAdjust::RoundDown, SizeAdjust::RoundUp,
		};

		std::string digits = text;
		const char* prefix = digits.empty() ? nullptr : std::strchr(kPrefixes, digits[0]);
		if (prefix != nullptr && *prefix != '\0') {
			options.adjust = kAdjusts[prefix - kPrefixes];
			digits.erase(0, 1);
		}

		options.have_size = parseByteSize(digits, options.size);
		const bool divides = options.adjust == SizeAdjust::RoundDown
			|| options.adjust == SizeAdjust::RoundUp;
		return options.have_size && (!divides || options.size != 0);
	}

	static bool takeTruncateSize(const std::vector<std::string>& args, std::size_t& i,
								 std::string& value) {
		const std::string& arg = args[i];
		if ((arg == "-s" || arg == "--size") && i + 1 < args.size()) {
			value = args[++i];
			return true;
		}

		if (arg.compare(0, 7, "--size=") == 0) {
			value = arg.substr(7);
			return true;
		}

		if (arg.size() <= 2 || arg.compare(0, 2, "-s") != 0) return false;
		value = arg.substr(2);
		return true;
	}

	static bool parseTruncateArgs(const std::vector<std::string>& args, TruncateOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			std::string size;
			if (takeTruncateSize(args, i, size)) {
				if (!parseTruncateSize(size, options)) return false;
				continue;
			}

			if (arg == "-c" || arg == "--no-create") {
				options.no_create = true;
				continue;
			}

			if ((arg == "-r" || arg == "--reference") && i + 1 < args.size()) {
				options.reference = args[++i];
				continue;
			}

			options.files.push_back(arg);
		}

		return (options.have_size || !options.reference.empty()) && !options.files.empty();
	}

	static unsigned long long adjustedSize(unsigned long long current,
										   const TruncateOptions& options) {
		const unsigned long long size = options.size;
		switch (options.adjust) {
		case SizeAdjust::Grow:      return current + size;
		case SizeAdjust::Shrink:    return current > size ? current - size : 0;
		case SizeAdjust::AtMost:    return std::min(current, size);
		case SizeAdjust::AtLeast:   return std::max(current, size);
		case SizeAdjust::RoundDown: return current / size * size;
		case SizeAdjust::RoundUp:   return (current + size - 1) / size * size;
		case SizeAdjust::Set:
		default:                    return size;
		}
	}

	static bool truncateOne(Executor& exec, const std::string& file,
							const TruncateOptions& options) {
		const fs::path path = toNative(exec, file);
		std::error_code ec;
		const bool exists = fs::exists(path, ec);
		if (!exists && options.no_create) return true;
		if (!exists) {
			std::FILE* created = fopenNative(exec, file, "wb");
			if (created == nullptr) {
				perr("truncate", file + ": " + errnoText());
				return false;
			}

			std::fclose(created);
		}

		const unsigned long long current = fs::file_size(path, ec);
		fs::resize_file(path, adjustedSize(ec ? 0 : current, options), ec);
		if (!ec) return true;

		perr("truncate", file, ec);
		return false;
	}

	static int builtin_truncate(Executor& exec, const std::vector<std::string>& args) {
		TruncateOptions options;
		if (!parseTruncateArgs(args, options)) {
			perr("truncate", "usage: truncate [-c] -s [+|-|<|>|/|%]SIZE | -r RFILE FILE...");
			return 1;
		}

		if (!options.reference.empty()) {
			std::error_code ec;
			const fs::path reference = toNative(exec, options.reference);
			const unsigned long long reference_size = fs::file_size(reference, ec);
			if (ec) {
				perr("truncate", options.reference, ec);
				return 1;
			}

			if (!options.have_size) options.size = reference_size;
		}

		int status = 0;
		for (const std::string& file : options.files) {
			if (!truncateOne(exec, file, options)) status = 1;
		}

		return status;
	}

	static bool applyShredFlags(const std::string& cluster, ShredOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char flag = cluster[k];
			if (flag == 'z')      options.zero_pass = true;
			else if (flag == 'u') options.remove = true;
			else if (flag == 'v') options.verbose = true;
			else if (flag == 'f') options.force = true;
			else if (flag != 'x') return false;
		}

		return true;
	}

	static bool parseShredArgs(const std::vector<std::string>& args, ShredOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-n" && i + 1 < args.size()) {
				if (!parseInt(args[++i], options.passes) || options.passes < 0) return false;
				continue;
			}

			if (arg == "-s" && i + 1 < args.size()) {
				options.have_size = parseByteSize(args[++i], options.size);
				if (!options.have_size) return false;
				continue;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			if (!applyShredFlags(arg, options)) return false;
		}

		return !options.files.empty();
	}

	static bool overwritePass(std::FILE* file, unsigned long long size, bool zeros,
	                          std::mt19937_64& generator) {
		std::vector<unsigned char> chunk(kShredChunk, 0);
		_fseeki64(file, 0, SEEK_SET);
		for (unsigned long long left = size; left > 0;) {
			const std::size_t want = chunkFor(left, chunk.size());
			if (!zeros) {
				for (unsigned char& byte : chunk) byte = static_cast<unsigned char>(generator());
			}

			if (std::fwrite(chunk.data(), 1, want, file) != want) return false;
			left -= want;
		}

		std::fflush(file);
		return flushToDisk(file);
	}

	static bool shredOne(Executor& exec, const std::string& name, const ShredOptions& options) {
		const fs::path path = toNative(exec, name);
		std::error_code ec;
		if (options.force) fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ec);

		const unsigned long long size = options.have_size ? options.size : fs::file_size(path, ec);
		std::FILE* file = fopenNative(exec, name, "r+b");
		if (file == nullptr) {
			perr("shred", name + ": " + errnoText());
			return false;
		}

		std::random_device seed;
		std::mt19937_64 generator((static_cast<unsigned long long>(seed()) << 32) ^ seed());
		const int total = options.passes + (options.zero_pass ? 1 : 0);
		bool ok = true;
		for (int pass = 1; pass <= total && ok; ++pass) {
			const bool zeros = options.zero_pass && pass == total;
			if (options.verbose) {
				std::fprintf(stderr, "shred: %s: pass %d/%d (%s)...\n", name.c_str(), pass, total,
					zeros ? "000000" : "random");
			}

			ok = overwritePass(file, size, zeros, generator);
		}

		std::fclose(file);
		if (ok && options.remove) fs::remove(path, ec);
		if (!ok) perr("shred", name + ": write failed");
		return ok;
	}

	static int builtin_shred(Executor& exec, const std::vector<std::string>& args) {
		ShredOptions options;
		if (!parseShredArgs(args, options)) {
			perr("shred", "usage: shred [-fuvzx] [-n N] [-s SIZE] FILE...");
			return 1;
		}

		int status = 0;
		for (const std::string& file : options.files) {
			if (!shredOne(exec, file, options)) status = 1;
		}

		return status;
	}

	static int builtin_link(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() != 2) {
			perr("link", "usage: link FILE1 FILE2");
			return 1;
		}

		std::error_code ec;
		fs::create_hard_link(toNative(exec, args[0]), toNative(exec, args[1]), ec);
		if (!ec) return 0;

		perr("link", "cannot create link '" + args[1] + "' to '" + args[0] + "'", ec);
		return 1;
	}

	static int builtin_unlink(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() != 1) {
			perr("unlink", "usage: unlink FILE");
			return 1;
		}

		const fs::path path = toNative(exec, args[0]);
		std::error_code ec;
		if (fs::is_directory(fs::symlink_status(path, ec))) {
			perr("unlink", "cannot unlink '" + args[0] + "': Is a directory");
			return 1;
		}

		if (fs::remove(path, ec)) return 0;
		if (!ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
		perr("unlink", "cannot unlink '" + args[0] + "'", ec);
		return 1;
	}

	// Windows has no user-level "flush every dirty buffer"; files are
	// flushed one at a time by fsync, and plain sync succeeds as a no-op.
	static int builtin_sync(Executor& exec, const std::vector<std::string>& args) {
		int status = 0;
		for (const std::string& file : args) {
			if (file.size() > 1 && file[0] == '-') continue;
			const std::wstring native = toNative(exec, file).wstring();
			const HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (handle == INVALID_HANDLE_VALUE || ::FlushFileBuffers(handle) == 0) {
				perr("sync", "error syncing '" + file + "'");
				status = 1;
			}

			if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
		}

		return status;
	}

	static std::string toUnixLineEnds(const std::string& text) {
		std::string out;
		out.reserve(text.size());
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
			out.push_back(text[i]);
		}

		return out;
	}

	static std::string toDosLineEnds(const std::string& text) {
		std::string out;
		out.reserve(text.size() + text.size() / 16);
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) out.push_back('\r');
			out.push_back(text[i]);
		}

		return out;
	}

	static bool convertFileInPlace(Executor& exec, const std::string& name, bool to_dos,
	                               const char* cmd) {
		std::FILE* in = fopenNative(exec, name, "rb");
		std::string text;
		if (in == nullptr || !readWholeStream(in, text)) {
			perr(cmd, name + ": " + errnoText());
			if (in != nullptr) std::fclose(in);
			return false;
		}

		std::fclose(in);
		const std::string converted = to_dos ? toDosLineEnds(text) : toUnixLineEnds(text);
		if (converted == text) return true;

		std::FILE* out = fopenNative(exec, name, "wb");
		if (out == nullptr) {
			perr(cmd, name + ": " + errnoText());
			return false;
		}

		const bool ok = std::fwrite(converted.data(), 1, converted.size(), out) == converted.size();
		std::fclose(out);
		return ok;
	}

	static int convertLineEnds(Executor& exec, const std::vector<std::string>& args, bool to_dos,
	                           const char* cmd) {
		std::vector<std::string> files;
		for (const std::string& arg : args) {
			if (arg == "-u")      to_dos = false;
			else if (arg == "-d") to_dos = true;
			else if (arg != "-n" && arg != "-o") files.push_back(arg);
		}

		if (files.empty()) {
			std::string text;
			readWholeStream(stdin, text);
			const std::string converted = to_dos ? toDosLineEnds(text) : toUnixLineEnds(text);
			std::fwrite(converted.data(), 1, converted.size(), stdout);
			std::fflush(stdout);
			return 0;
		}

		int status = 0;
		for (const std::string& file : files) {
			if (!convertFileInPlace(exec, file, to_dos, cmd)) status = 1;
		}

		return status;
	}

	static int builtin_dos2unix(Executor& exec, const std::vector<std::string>& args) {
		return convertLineEnds(exec, args, false, "dos2unix");
	}

	static int builtin_unix2dos(Executor& exec, const std::vector<std::string>& args) {
		return convertLineEnds(exec, args, true, "unix2dos");
	}

	void registerFileOpsBuiltins(Executor& exec) {
		exec.registerBuiltin("dd",       builtin_dd);
		exec.registerBuiltin("split",    builtin_split);
		exec.registerBuiltin("truncate", builtin_truncate);
		exec.registerBuiltin("shred",    builtin_shred);
		exec.registerBuiltin("link",     builtin_link);
		exec.registerBuiltin("unlink",   builtin_unlink);
		exec.registerBuiltin("sync",     builtin_sync);
		exec.registerBuiltin("fsync",    builtin_sync);
		exec.registerBuiltin("dos2unix", builtin_dos2unix);
		exec.registerBuiltin("unix2dos", builtin_unix2dos);
	}

}  // namespace wbsh
