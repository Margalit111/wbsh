/**
 * @file coreutils_compress.cpp
 * @brief The single-file compressors, all sharing gzip's command-line
 *        behaviour: gzip / gunzip / zcat, bzip2 / bunzip2 / bzcat,
 *        xz / unxz / xzcat, lzma / unlzma / lzcat, uncompress.
 *
 * xz and lzma decompress only, as busybox's do; asking them to compress
 * says so instead of writing a file.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "codecs.h"
#include "executor.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace compress_detail {

		struct SuffixRule {
			const char* compressed;
			const char* plain;
		};

		struct CompressTool {
			const char* name;
			Codec codec;
			const SuffixRule* suffixes;
			bool decompress;
			bool to_stdout;
			int default_level;
		};

		struct CompressOptions {
			bool decompress = false;
			bool to_stdout = false;
			bool keep = false;
			bool force = false;
			bool test = false;
			bool quiet = false;
			bool verbose = false;
			int level = 6;
			std::string suffix;
			std::vector<std::string> files;
		};

	}  // namespace compress_detail

	using compress_detail::CompressOptions;
	using compress_detail::CompressTool;
	using compress_detail::SuffixRule;

	static const SuffixRule kGzipSuffixes[] = {
		{ ".gz", "" }, { ".tgz", ".tar" }, { ".taz", ".tar" }, { "-gz", "" }, { ".z", "" },
		{ "_z", "" }, { ".Z", "" }, { nullptr, nullptr },
	};

	static const SuffixRule kBzip2Suffixes[] = {
		{ ".bz2", "" }, { ".bz", "" }, { ".tbz2", ".tar" }, { ".tbz", ".tar" },
		{ nullptr, nullptr },
	};

	static const SuffixRule kXzSuffixes[] = {
		{ ".xz", "" }, { ".txz", ".tar" }, { ".lzma", "" }, { ".tlz", ".tar" },
		{ nullptr, nullptr },
	};

	static const SuffixRule kLzmaSuffixes[] = {
		{ ".lzma", "" }, { ".tlz", ".tar" }, { nullptr, nullptr },
	};

	static const SuffixRule kCompressSuffixes[] = {
		{ ".Z", "" }, { ".taZ", ".tar" }, { nullptr, nullptr },
	};

	static const CompressTool kTools[] = {
		{ "gzip",       Codec::Gzip,     kGzipSuffixes,     false, false, 6 },
		{ "gunzip",     Codec::Gzip,     kGzipSuffixes,     true,  false, 6 },
		{ "zcat",       Codec::Gzip,     kGzipSuffixes,     true,  true,  6 },
		{ "bzip2",      Codec::Bzip2,    kBzip2Suffixes,    false, false, 9 },
		{ "bunzip2",    Codec::Bzip2,    kBzip2Suffixes,    true,  false, 9 },
		{ "bzcat",      Codec::Bzip2,    kBzip2Suffixes,    true,  true,  9 },
		{ "xz",         Codec::Xz,       kXzSuffixes,       false, false, 6 },
		{ "unxz",       Codec::Xz,       kXzSuffixes,       true,  false, 6 },
		{ "xzcat",      Codec::Xz,       kXzSuffixes,       true,  true,  6 },
		{ "lzma",       Codec::Lzma,     kLzmaSuffixes,     false, false, 6 },
		{ "unlzma",     Codec::Lzma,     kLzmaSuffixes,     true,  false, 6 },
		{ "lzcat",      Codec::Lzma,     kLzmaSuffixes,     true,  true,  6 },
		{ "uncompress", Codec::Compress, kCompressSuffixes, true,  false, 6 },
	};

	static const std::size_t kReadChunk = 64 * 1024;

	static bool applyCompressFlag(char flag, CompressOptions& options) {
		if (flag >= '1' && flag <= '9') {
			options.level = flag - '0';
			return true;
		}

		switch (flag) {
		case 'd': options.decompress = true; return true;
		case 'c': options.to_stdout = true;  return true;
		case 'k': options.keep = true;       return true;
		case 'f': options.force = true;      return true;
		case 't': options.test = true;       return true;
		case 'q': options.quiet = true;      return true;
		case 'v': options.verbose = true;    return true;
		case 'n':
		case 'N':
		case 'z':
			return true;
		default:
			return false;
		}
	}

	static bool applyCompressCluster(const std::string& cluster, CompressOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (!applyCompressFlag(cluster[k], options)) return false;
		}

		return true;
	}

	static bool applyLongCompressOption(const std::string& arg, CompressOptions& options) {
		if (arg == "--decompress" || arg == "--uncompress") options.decompress = true;
		else if (arg == "--stdout" || arg == "--to-stdout")  options.to_stdout = true;
		else if (arg == "--keep")    options.keep = true;
		else if (arg == "--force")   options.force = true;
		else if (arg == "--test")    options.test = true;
		else if (arg == "--quiet")   options.quiet = true;
		else if (arg == "--verbose") options.verbose = true;
		else if (arg == "--fast")    options.level = 1;
		else if (arg == "--best")    options.level = 9;
		else if (arg.compare(0, 9, "--suffix=") == 0) options.suffix = arg.substr(9);
		else return arg == "--no-name" || arg == "--name";
		return true;
	}

	static bool parseCompressArgs(const CompressTool& tool, const std::vector<std::string>& args,
	                              CompressOptions& options) {
		options.decompress = tool.decompress;
		options.to_stdout = tool.to_stdout;
		options.level = tool.default_level;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-S" && i + 1 < args.size()) {
				options.suffix = args[++i];
				continue;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			const bool known = arg[1] == '-'
				? applyLongCompressOption(arg, options)
				: applyCompressCluster(arg, options);
			if (known) continue;

			perr(tool.name, "invalid option -- '" + arg + "'");
			return false;
		}

		if (options.test) options.decompress = true;
		return true;
	}

	static bool readStream(std::FILE* stream, std::vector<std::uint8_t>& bytes) {
		std::vector<std::uint8_t> chunk(kReadChunk);
		for (;;) {
			const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
			if (got == 0) return std::ferror(stream) == 0;
			const auto end = chunk.begin() + static_cast<std::ptrdiff_t>(got);
			bytes.insert(bytes.end(), chunk.begin(), end);
		}
	}

	static bool streamIsTerminal(std::FILE* stream) {
		return isConsoleStream(stream);
	}

	static bool acceptsCodec(const CompressTool& tool, Codec found) {
		if (found == tool.codec) return true;
		if (tool.codec == Codec::Gzip) return found == Codec::Compress;
		if (tool.codec == Codec::Xz) return found == Codec::Lzma;
		return false;
	}

	static const char* formatName(Codec codec) {
		switch (codec) {
		case Codec::Gzip:     return "gzip";
		case Codec::Bzip2:    return "bzip2";
		case Codec::Xz:       return "xz";
		case Codec::Lzma:     return "lzma";
		case Codec::Compress: return "compress";
		case Codec::None:
		default:              return "known";
		}
	}

	static bool decodeInput(const CompressTool& tool, const CompressOptions& options,
	                        const std::vector<std::uint8_t>& input,
	                        std::vector<std::uint8_t>& output,
	                        const std::string& name) {
		Codec codec = detectCodec(input.data(), input.size());
		if (codec == Codec::None && tool.codec == Codec::Lzma) codec = Codec::Lzma;
		if (!acceptsCodec(tool, codec)) {
			if (options.force && options.to_stdout) {
				output = input;
				return true;
			}

			perr(tool.name, name + ": not in " + formatName(tool.codec) + " format");
			return false;
		}

		std::string error;
		if (decompressWith(codec, input.data(), input.size(), output, error)) return true;

		perr(tool.name, name + ": " + error);
		return false;
	}

	static bool transformData(const CompressTool& tool, const CompressOptions& options,
	                          const std::vector<std::uint8_t>& input,
	                          std::vector<std::uint8_t>& output, const std::string& name) {
		if (options.decompress) return decodeInput(tool, options, input, output, name);

		std::string error;
		if (compressWith(tool.codec, input.data(), input.size(), options.level, output, error)) {
			return true;
		}

		perr(tool.name, error + "; use -d to decompress");
		return false;
	}

	static bool writeAll(std::FILE* out, const std::vector<std::uint8_t>& bytes) {
		return std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
	}

	static int compressStdin(const CompressTool& tool, const CompressOptions& options) {
		if (!options.decompress && !options.force && streamIsTerminal(stdout)) {
			perr(tool.name, "compressed data not written to a terminal. "
				"Use -f to force compression.");
			return 1;
		}

		if (options.decompress && !options.force && streamIsTerminal(stdin)) {
			perr(tool.name, "compressed data not read from a terminal. "
				"Use -f to force decompression.");
			return 1;
		}

		std::vector<std::uint8_t> input;
		std::vector<std::uint8_t> output;
		if (!readStream(stdin, input)) return 1;
		if (!transformData(tool, options, input, output, "stdin")) return 1;
		if (!options.test) writeAll(stdout, output);
		std::fflush(stdout);
		return 0;
	}

	static bool endsWith(const std::string& text, const std::string& suffix) {
		return !suffix.empty() && text.size() > suffix.size()
			&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	static bool plainName(const CompressTool& tool, const CompressOptions& options,
	                      const std::string& path, std::string& plain) {
		if (endsWith(path, options.suffix)) {
			plain = path.substr(0, path.size() - options.suffix.size());
			return true;
		}

		for (const SuffixRule* rule = tool.suffixes; rule->compressed != nullptr; ++rule) {
			if (!endsWith(path, rule->compressed)) continue;
			plain = path.substr(0, path.size() - std::strlen(rule->compressed)) + rule->plain;
			return true;
		}

		return false;
	}

	static bool outputName(const CompressTool& tool, const CompressOptions& options,
	                       const std::string& path, std::string& target) {
		if (options.decompress) {
			if (plainName(tool, options, path, target)) return true;
			if (!options.quiet) perr(tool.name, path + ": unknown suffix -- ignored");
			return false;
		}

		const std::string suffix = options.suffix.empty() ? tool.suffixes[0].compressed
			: options.suffix;
		std::string ignored;
		if (plainName(tool, options, path, ignored) && !options.force) {
			if (!options.quiet) {
				perr(tool.name, path + " already has " + suffix + " suffix -- unchanged");
			}

			return false;
		}

		target = path + suffix;
		return true;
	}

	static bool loadFile(Executor& exec, const CompressTool& tool, const std::string& path,
	                     std::vector<std::uint8_t>& input) {
		std::error_code ec;
		if (fs::is_directory(toNative(exec, path), ec)) {
			perr(tool.name, path + " is a directory -- ignored");
			return false;
		}

		std::FILE* in = fopenNative(exec, path, "rb");
		if (in == nullptr) {
			perr(tool.name, path + ": " + std::strerror(errno));
			return false;
		}

		const bool ok = readStream(in, input);
		std::fclose(in);
		if (!ok) perr(tool.name, path + ": read error");
		return ok;
	}

	static bool storeFile(Executor& exec, const CompressTool& tool, const CompressOptions& options,
	                      const std::string& target, const std::vector<std::uint8_t>& output) {
		std::error_code ec;
		if (!options.force && fs::exists(toNative(exec, target), ec)) {
			perr(tool.name, target + " already exists; not overwritten");
			return false;
		}

		std::FILE* out = fopenNative(exec, target, "wb");
		if (out == nullptr) {
			perr(tool.name, target + ": " + std::strerror(errno));
			return false;
		}

		const bool ok = writeAll(out, output);
		std::fclose(out);
		if (!ok) perr(tool.name, target + ": write error");
		return ok;
	}

	static void reportRatio(const std::string& path, const std::string& target,
	                        std::size_t before, std::size_t after, bool decompressed) {
		const std::size_t plain = decompressed ? after : before;
		const std::size_t packed = decompressed ? before : after;
		const double saved = plain == 0 ? 0.0
			: 100.0 * (static_cast<double>(plain) - static_cast<double>(packed))
				/ static_cast<double>(plain);
		std::fprintf(stderr, "%s:\t%5.1f%% -- replaced with %s\n", path.c_str(), saved,
			target.c_str());
	}

	static void finishInPlace(Executor& exec, const std::string& path, const std::string& target,
	                          const CompressOptions& options) {
		std::error_code ec;
		const fs::path source = toNative(exec, path);
		const fs::file_time_type stamp = fs::last_write_time(source, ec);
		if (!ec) fs::last_write_time(toNative(exec, target), stamp, ec);
		if (!options.keep) fs::remove(source, ec);
	}

	static int processFile(Executor& exec, const CompressTool& tool, const CompressOptions& options,
	                       const std::string& path) {
		std::vector<std::uint8_t> input;
		std::vector<std::uint8_t> output;
		std::string target;
		const bool in_place = !options.to_stdout && !options.test;
		if (in_place && !outputName(tool, options, path, target)) return options.quiet ? 0 : 1;
		if (!loadFile(exec, tool, path, input)) return 1;
		if (!transformData(tool, options, input, output, path)) return 1;

		if (options.test) {
			if (options.verbose) std::fprintf(stderr, "%s:\tOK\n", path.c_str());
			return 0;
		}

		if (!in_place) {
			writeAll(stdout, output);
			return 0;
		}

		if (!storeFile(exec, tool, options, target, output)) return 1;
		finishInPlace(exec, path, target, options);
		if (!options.verbose) return 0;

		reportRatio(path, target, input.size(), output.size(), options.decompress);
		return 0;
	}

	static int runCompressTool(Executor& exec, const CompressTool& tool,
	                           const std::vector<std::string>& args) {
		CompressOptions options;
		if (!parseCompressArgs(tool, args, options)) return 1;
		if (options.files.empty()) return compressStdin(tool, options);

		int status = 0;
		for (const std::string& file : options.files) {
			const int file_status = file == "-" ? compressStdin(tool, options)
				: processFile(exec, tool, options, file);
			if (file_status != 0) status = file_status;
		}

		std::fflush(stdout);
		return status;
	}

	static const CompressTool& toolNamed(const char* name) {
		for (const CompressTool& tool : kTools) {
			if (std::strcmp(tool.name, name) == 0) return tool;
		}

		return kTools[0];
	}

	static int builtin_gzip(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("gzip"), args);
	}

	static int builtin_gunzip(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("gunzip"), args);
	}

	static int builtin_zcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("zcat"), args);
	}

	static int builtin_bzip2(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bzip2"), args);
	}

	static int builtin_bunzip2(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bunzip2"), args);
	}

	static int builtin_bzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bzcat"), args);
	}

	static int builtin_xz(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("xz"), args);
	}

	static int builtin_unxz(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("unxz"), args);
	}

	static int builtin_xzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("xzcat"), args);
	}

	static int builtin_lzma(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("lzma"), args);
	}

	static int builtin_unlzma(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("unlzma"), args);
	}

	static int builtin_lzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("lzcat"), args);
	}

	static int builtin_uncompress(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("uncompress"), args);
	}

	void registerCompressBuiltins(Executor& exec) {
		exec.registerBuiltin("gzip",       builtin_gzip);
		exec.registerBuiltin("gunzip",     builtin_gunzip);
		exec.registerBuiltin("zcat",       builtin_zcat);
		exec.registerBuiltin("bzip2",      builtin_bzip2);
		exec.registerBuiltin("bunzip2",    builtin_bunzip2);
		exec.registerBuiltin("bzcat",      builtin_bzcat);
		exec.registerBuiltin("xz",         builtin_xz);
		exec.registerBuiltin("unxz",       builtin_unxz);
		exec.registerBuiltin("xzcat",      builtin_xzcat);
		exec.registerBuiltin("lzma",       builtin_lzma);
		exec.registerBuiltin("unlzma",     builtin_unlzma);
		exec.registerBuiltin("lzcat",      builtin_lzcat);
		exec.registerBuiltin("uncompress", builtin_uncompress);
	}

}  // namespace wbsh
