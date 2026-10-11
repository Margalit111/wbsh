/**
 * @file coreutils_misc.cpp
 * @brief Small utilities: ts, chattr, lsattr, ipcalc, iconv, flock, dc.
 *
 * chattr / lsattr work on the Windows file attributes (read-only,
 * hidden, system, archive, not-indexed), the attributes busybox-w32
 * exposes under the same names.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32

	namespace fs = std::filesystem;

	namespace misc_detail {

		struct AttributeLetter {
			char letter;
			DWORD flag;
		};

		struct AttributeChange {
			DWORD add = 0;
			DWORD remove = 0;
			bool assign = false;
		};

		struct CodePageName {
			const char* name;
			UINT code_page;
		};

		enum class Utf16Order {
			None,
			Little,
			Big,
		};

		struct Encoding {
			UINT code_page = CP_UTF8;
			Utf16Order utf16 = Utf16Order::None;
		};

		struct IconvOptions {
			std::string from = "UTF-8";
			std::string to = "UTF-8";
			std::string output;
			bool omit_invalid = false;
			bool list = false;
			std::vector<std::string> files;
		};

		struct FlockOptions {
			DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
			double wait = -1.0;
			std::size_t first = 0;
		};

		struct DcValue {
			double number = 0.0;
			int scale = 0;
		};

		struct DcState {
			std::vector<DcValue> stack;
			int scale = 0;
			bool failed = false;
		};

	}  // namespace misc_detail

	using misc_detail::AttributeChange;
	using misc_detail::AttributeLetter;
	using misc_detail::CodePageName;
	using misc_detail::DcState;
	using misc_detail::DcValue;
	using misc_detail::Encoding;
	using misc_detail::FlockOptions;
	using misc_detail::IconvOptions;
	using misc_detail::Utf16Order;

	static const AttributeLetter kAttributeLetters[] = {
		{ 'r', FILE_ATTRIBUTE_READONLY },
		{ 'h', FILE_ATTRIBUTE_HIDDEN },
		{ 's', FILE_ATTRIBUTE_SYSTEM },
		{ 'a', FILE_ATTRIBUTE_ARCHIVE },
		{ 't', FILE_ATTRIBUTE_TEMPORARY },
		{ 'n', FILE_ATTRIBUTE_NOT_CONTENT_INDEXED },
		{ 'c', FILE_ATTRIBUTE_COMPRESSED },
		{ 'e', FILE_ATTRIBUTE_ENCRYPTED },
		{ 'o', FILE_ATTRIBUTE_OFFLINE },
		{ 'l', FILE_ATTRIBUTE_REPARSE_POINT },
	};

	static const DWORD kSettableAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN
		| FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY
		| FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_OFFLINE;

	static const CodePageName kCodePages[] = {
		{ "UTF8", CP_UTF8 },          { "ASCII", 20127 },         { "USASCII", 20127 },
		{ "LATIN1", 28591 },          { "ISO88591", 28591 },      { "ISO88592", 28592 },
		{ "ISO88595", 28595 },        { "ISO88597", 28597 },      { "ISO885915", 28605 },
		{ "CP1250", 1250 },           { "CP1251", 1251 },         { "CP1252", 1252 },
		{ "CP1253", 1253 },           { "CP1254", 1254 },         { "CP1255", 1255 },
		{ "CP1256", 1256 },           { "CP1257", 1257 },         { "CP1258", 1258 },
		{ "WINDOWS1250", 1250 },      { "WINDOWS1251", 1251 },    { "WINDOWS1252", 1252 },
		{ "CP437", 437 },             { "CP850", 850 },           { "CP852", 852 },
		{ "CP866", 866 },             { "KOI8R", 20866 },         { "KOI8U", 21866 },
		{ "SHIFTJIS", 932 },          { "SJIS", 932 },            { "CP932", 932 },
		{ "EUCJP", 20932 },           { "GBK", 936 },             { "GB2312", 936 },
		{ "GB18030", 54936 },         { "BIG5", 950 },            { "EUCKR", 51949 },
		{ "CP949", 949 },             { "MACINTOSH", 10000 },
	};

	static const int kFlockUsageStatus = 64;
	static const DWORD kFlockPollMs = 50;

	static std::tm timestampParts(bool relative, std::chrono::steady_clock::duration elapsed) {
		std::tm parts{};
		if (!relative) {
			const std::time_t now = std::time(nullptr);
			localtime_s(&parts, &now);
			return parts;
		}

		const std::time_t seconds = static_cast<std::time_t>(
			std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
		gmtime_s(&parts, &seconds);
		return parts;
	}

	static int builtin_ts(Executor&, const std::vector<std::string>& args) {
		bool incremental = false;
		bool since_start = false;
		std::string format;
		for (const std::string& arg : args) {
			if (arg == "-i")      incremental = true;
			else if (arg == "-s") since_start = true;
			else if (arg != "-m") format = arg;
		}

		const bool relative = incremental || since_start;
		if (format.empty()) format = relative ? "%H:%M:%S" : "%b %d %H:%M:%S";

		const auto started = std::chrono::steady_clock::now();
		auto previous = started;
		std::string line;
		while (std::getline(std::cin, line)) {
			const auto now = std::chrono::steady_clock::now();
			const auto since = incremental ? previous : started;
			const std::tm parts = timestampParts(relative, now - since);
			previous = now;

			char text[128];
			std::strftime(text, sizeof(text), format.c_str(), &parts);
			std::printf("%s %s\n", text, line.c_str());
			std::fflush(stdout);
		}

		return 0;
	}

	static std::string attributeText(DWORD attributes) {
		std::string text;
		for (const AttributeLetter& entry : kAttributeLetters) {
			text.push_back((attributes & entry.flag) != 0 ? entry.letter : '-');
		}

		return text;
	}

	static void listAttributes(Executor& exec, const std::string& path) {
		const std::wstring native = toNative(exec, path).wstring();
		const DWORD attributes = ::GetFileAttributesW(native.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			perr("lsattr", path + ": No such file or directory");
			return;
		}

		std::printf("%s %s\n", attributeText(attributes).c_str(), path.c_str());
	}

	static void listDirectoryAttributes(Executor& exec, const std::string& directory, bool all) {
		std::error_code ec;
		const fs::path native = toNative(exec, directory);
		for (fs::directory_iterator it(native, ec), end; !ec && it != end; it.increment(ec)) {
			const std::string name = pathToUtf8(it->path().filename());
			if (!all && !name.empty() && name[0] == '.') continue;
			listAttributes(exec, directory + "/" + name);
		}
	}

	static int builtin_lsattr(Executor& exec, const std::vector<std::string>& args) {
		bool all = false;
		bool directories_only = false;
		std::vector<std::string> paths;
		for (const std::string& arg : args) {
			if (arg == "-a")      all = true;
			else if (arg == "-d") directories_only = true;
			else                  paths.push_back(arg);
		}

		if (paths.empty()) {
			listDirectoryAttributes(exec, ".", all);
			return 0;
		}

		std::error_code ec;
		for (const std::string& path : paths) {
			const bool directory = fs::is_directory(toNative(exec, path), ec);
			if (directory && !directories_only) listDirectoryAttributes(exec, path, all);
			else                                listAttributes(exec, path);
		}

		return 0;
	}

	static bool settableFlag(char letter, DWORD& flag) {
		for (const AttributeLetter& entry : kAttributeLetters) {
			if (entry.letter != letter || (entry.flag & kSettableAttributes) == 0) continue;
			flag = entry.flag;
			return true;
		}

		return false;
	}

	static bool parseAttributeChange(const std::string& spec, AttributeChange& change) {
		DWORD flags = 0;
		for (std::size_t k = 1; k < spec.size(); ++k) {
			DWORD flag = 0;
			if (!settableFlag(spec[k], flag)) return false;
			flags |= flag;
		}

		switch (spec[0]) {
		case '+': change.add |= flags;     return true;
		case '-': change.remove |= flags;  return true;
		case '=':
			change.assign = true;
			change.add = flags;
			return true;
		default:
			return false;
		}
	}

	static bool applyAttributes(Executor& exec, const std::string& path,
	                            const AttributeChange& change) {
		const std::wstring native = toNative(exec, path).wstring();
		const DWORD current = ::GetFileAttributesW(native.c_str());
		if (current == INVALID_FILE_ATTRIBUTES) {
			perr("chattr", path + ": No such file or directory");
			return false;
		}

		const DWORD cleared = change.assign ? kSettableAttributes : change.remove;
		const DWORD kept = current & ~cleared;
		const DWORD wanted = (kept | change.add) & ~static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY);
		if (::SetFileAttributesW(native.c_str(), wanted) != 0) return true;

		perr("chattr", path + ": cannot set attributes");
		return false;
	}

	static int builtin_chattr(Executor& exec, const std::vector<std::string>& args) {
		AttributeChange change;
		std::vector<std::string> paths;
		for (const std::string& arg : args) {
			const bool is_change = arg.size() > 1 && std::strchr("+-=", arg[0]) != nullptr;
			if (!is_change) {
				paths.push_back(arg);
				continue;
			}

			if (!parseAttributeChange(arg, change)) {
				perr("chattr", "invalid attribute '" + arg + "' (use r h s a t n o)");
				return 1;
			}
		}

		int status = 0;
		for (const std::string& path : paths) {
			if (!applyAttributes(exec, path, change)) status = 1;
		}

		return status;
	}

	static bool parseOctet(const std::string& piece, std::uint32_t& address) {
		int value = 0;
		std::size_t used = 0;
		if (piece.empty() || !parseInt(piece, value, 10, &used)) return false;
		if (used != piece.size()) return false;
		if (value < 0 || value > 255) return false;

		address = (address << 8) | static_cast<std::uint32_t>(value);
		return true;
	}

	static bool parseIpv4(const std::string& text, std::uint32_t& address) {
		address = 0;
		int parts = 0;
		std::size_t start = 0;
		for (;;) {
			const std::size_t dot = text.find('.', start);
			const std::size_t length = dot == std::string::npos ? std::string::npos : dot - start;
			if (!parseOctet(text.substr(start, length), address)) return false;
			if (++parts == 4 || dot == std::string::npos) break;
			start = dot + 1;
		}

		return parts == 4 && text.find('.', start) == std::string::npos;
	}

	static std::string ipv4Text(std::uint32_t address) {
		char text[20];
		std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 255,
			(address >> 8) & 255, address & 255);
		return text;
	}

	static int prefixOfMask(std::uint32_t mask) {
		int prefix = 0;
		while (prefix < 32 && (mask & (0x80000000u >> prefix)) != 0) ++prefix;
		return prefix;
	}

	static std::uint32_t maskOfPrefix(int prefix) {
		return prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
	}

	static bool parseIpcalcTarget(const std::vector<std::string>& operands, std::uint32_t& address,
	                              std::uint32_t& mask) {
		if (operands.empty()) return false;

		std::string text = operands[0];
		const std::size_t slash = text.find('/');
		mask = 0xFFFFFF00u;
		if (slash == std::string::npos) {
			if (operands.size() > 1 && !parseIpv4(operands[1], mask)) return false;
			return parseIpv4(text, address);
		}

		int prefix = 0;
		if (!parseInt(text.substr(slash + 1), prefix) || prefix < 0 || prefix > 32) return false;
		mask = maskOfPrefix(prefix);
		return parseIpv4(text.substr(0, slash), address);
	}

	static int builtin_ipcalc(Executor&, const std::vector<std::string>& args) {
		std::string selected;
		std::vector<std::string> operands;
		for (const std::string& arg : args) {
			if (arg.size() > 1 && arg[0] == '-') selected += arg.substr(1);
			else                                 operands.push_back(arg);
		}

		std::uint32_t address = 0;
		std::uint32_t mask = 0;
		if (!parseIpcalcTarget(operands, address, mask)) {
			perr("ipcalc", "usage: ipcalc [-bmnp] ADDRESS[/PREFIX] [NETMASK]");
			return 1;
		}

		if (selected.empty()) selected = "mbnp";
		const std::uint32_t network = address & mask;
		const std::uint32_t broadcast = network | ~mask;
		for (const char flag : selected) {
			if (flag == 'm') std::printf("NETMASK=%s\n", ipv4Text(mask).c_str());
			if (flag == 'b') std::printf("BROADCAST=%s\n", ipv4Text(broadcast).c_str());
			if (flag == 'n') std::printf("NETWORK=%s\n", ipv4Text(network).c_str());
			if (flag == 'p') std::printf("PREFIX=%d\n", prefixOfMask(mask));
		}

		return 0;
	}

	static std::string normalizedEncodingName(const std::string& name) {
		std::string out;
		for (const char c : name) {
			if (c == '-' || c == '_' || c == ' ') continue;
			out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}

		const std::size_t suffix = out.find("//");
		if (suffix != std::string::npos) out.resize(suffix);
		return out;
	}

	static bool lookupNumberedCodePage(const std::string& key, Encoding& encoding) {
		unsigned long number = 0;
		std::size_t used = 0;
		const std::string digits = key.compare(0, 2, "CP") == 0 ? key.substr(2) : key;
		if (!parseUL(digits, number, 10, &used) || used != digits.size()) return false;

		encoding.code_page = static_cast<UINT>(number);
		return ::IsValidCodePage(encoding.code_page) != 0;
	}

	static bool lookupEncoding(const std::string& name, Encoding& encoding) {
		const std::string key = normalizedEncodingName(name);
		if (key == "UTF16" || key == "UTF16LE" || key == "UCS2" || key == "UCS2LE") {
			encoding.utf16 = Utf16Order::Little;
			return true;
		}

		if (key == "UTF16BE" || key == "UCS2BE") {
			encoding.utf16 = Utf16Order::Big;
			return true;
		}

		for (const CodePageName& entry : kCodePages) {
			if (key != entry.name) continue;
			encoding.code_page = entry.code_page;
			return true;
		}

		return lookupNumberedCodePage(key, encoding);
	}

	static std::wstring decodeUtf16(const std::string& bytes, Utf16Order order) {
		std::wstring text(bytes.size() / 2, L'\0');
		for (std::size_t k = 0; k < text.size(); ++k) {
			const unsigned first = static_cast<unsigned char>(bytes[2 * k]);
			const unsigned second = static_cast<unsigned char>(bytes[2 * k + 1]);
			const unsigned unit = order == Utf16Order::Little ? first | (second << 8)
				: (first << 8) | second;
			text[k] = static_cast<wchar_t>(unit);
		}

		if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
		return text;
	}

	static std::wstring decodeText(const std::string& bytes, const Encoding& encoding, bool& ok) {
		ok = true;
		if (encoding.utf16 != Utf16Order::None) return decodeUtf16(bytes, encoding.utf16);

		const int size = static_cast<int>(bytes.size());
		const int length = ::MultiByteToWideChar(encoding.code_page, 0, bytes.data(), size,
			nullptr, 0);
		if (length == 0 && !bytes.empty()) ok = false;

		std::wstring text(static_cast<std::size_t>(length), L'\0');
		::MultiByteToWideChar(encoding.code_page, 0, bytes.data(), size, text.data(), length);
		return text;
	}

	static std::string encodeUtf16(const std::wstring& text, Utf16Order order) {
		std::string bytes;
		for (const wchar_t unit : text) {
			const char low = static_cast<char>(unit & 0xFF);
			const char high = static_cast<char>((unit >> 8) & 0xFF);
			bytes.push_back(order == Utf16Order::Little ? low : high);
			bytes.push_back(order == Utf16Order::Little ? high : low);
		}

		return bytes;
	}

	// UTF-8 and GB18030 refuse the "used default char" out-parameter. Without
	// WC_NO_BEST_FIT_CHARS Windows quietly turns é into e and reports no loss.
	static std::string encodeText(const std::wstring& text, const Encoding& encoding, bool& lossy) {
		lossy = false;
		if (encoding.utf16 != Utf16Order::None) return encodeUtf16(text, encoding.utf16);

		BOOL used_default = FALSE;
		const bool reports_loss = encoding.code_page != CP_UTF8 && encoding.code_page != 54936;
		BOOL* loss = reports_loss ? &used_default : nullptr;
		const DWORD flags = reports_loss ? WC_NO_BEST_FIT_CHARS : 0;
		const int size = static_cast<int>(text.size());
		const int length = ::WideCharToMultiByte(encoding.code_page, flags, text.data(), size,
			nullptr, 0, nullptr, loss);
		std::string bytes(static_cast<std::size_t>(length), '\0');
		::WideCharToMultiByte(encoding.code_page, flags, text.data(), size, bytes.data(), length,
			nullptr, loss);
		lossy = used_default != FALSE;
		return bytes;
	}

	// iconv -c: code points the target cannot hold are dropped one by one
	// instead of turning into the code page's default character.
	static std::string encodeOmitting(const std::wstring& text, const Encoding& encoding) {
		std::string bytes;
		for (std::size_t k = 0; k < text.size(); ++k) {
			const bool pair = text[k] >= 0xD800 && text[k] <= 0xDBFF && k + 1 < text.size();
			const std::wstring unit = text.substr(k, pair ? 2 : 1);
			if (pair) ++k;

			bool lossy = false;
			const std::string encoded = encodeText(unit, encoding, lossy);
			if (!lossy) bytes += encoded;
		}

		return bytes;
	}

	static void listEncodings() {
		std::printf("UTF-8 UTF-16 UTF-16LE UTF-16BE");
		for (const CodePageName& entry : kCodePages) std::printf(" %s", entry.name);
		std::printf(" CPnnnn\n");
	}

	static void parseIconvArgs(const std::vector<std::string>& args, IconvOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-l" || arg == "--list")              options.list = true;
			else if (arg == "-f" && has_value)               options.from = args[++i];
			else if (arg == "-t" && has_value)               options.to = args[++i];
			else if (arg == "-o" && has_value)               options.output = args[++i];
			else if (arg == "-c")                            options.omit_invalid = true;
			else if (arg.compare(0, 12, "--from-code=") == 0) options.from = arg.substr(12);
			else if (arg.compare(0, 10, "--to-code=") == 0)   options.to = arg.substr(10);
			else                                             options.files.push_back(arg);
		}

		if (options.files.empty()) options.files.push_back("-");
	}

	static bool readIconvInput(Executor& exec, const std::vector<std::string>& files,
	                           std::string& bytes) {
		for (const std::string& file : files) {
			std::FILE* in = file == "-" ? stdin : fopenNative(exec, file, "rb");
			if (in == nullptr) {
				perr("iconv", file + ": " + std::strerror(errno));
				return false;
			}

			char chunk[65536];
			for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
				bytes.append(chunk, got);
			}

			if (in != stdin) std::fclose(in);
		}

		return true;
	}

	static bool writeIconvOutput(Executor& exec, const std::string& output,
	                             const std::string& converted) {
		std::FILE* out = output.empty() ? stdout : fopenNative(exec, output, "wb");
		if (out == nullptr) {
			perr("iconv", output + ": " + std::strerror(errno));
			return false;
		}

		std::fwrite(converted.data(), 1, converted.size(), out);
		if (out != stdout) std::fclose(out);
		std::fflush(stdout);
		return true;
	}

	static int builtin_iconv(Executor& exec, const std::vector<std::string>& args) {
		IconvOptions options;
		parseIconvArgs(args, options);
		if (options.list) {
			listEncodings();
			return 0;
		}

		Encoding source;
		Encoding target;
		if (!lookupEncoding(options.from, source) || !lookupEncoding(options.to, target)) {
			perr("iconv", "conversion from '" + options.from + "' to '" + options.to
				+ "' is not supported");
			return 1;
		}

		std::string bytes;
		if (!readIconvInput(exec, options.files, bytes)) return 1;

		bool decoded = true;
		bool lossy = false;
		const std::wstring text = decodeText(bytes, source, decoded);
		std::string converted = encodeText(text, target, lossy);
		if (lossy && options.omit_invalid) converted = encodeOmitting(text, target);
		if (!writeIconvOutput(exec, options.output, converted)) return 1;
		if (decoded && (!lossy || options.omit_invalid)) return 0;

		perr("iconv", "cannot convert some characters");
		return 1;
	}

	static bool applyFlockOption(const std::vector<std::string>& args, std::size_t& i,
	                             FlockOptions& options) {
		const std::string& arg = args[i];
		const DWORD exclusive = LOCKFILE_EXCLUSIVE_LOCK;
		if ((arg == "-w" || arg == "--timeout") && i + 1 < args.size()) {
			return parseDouble(args[++i], options.wait);
		}

		if (arg == "-s" || arg == "--shared")         options.flags &= ~exclusive;
		else if (arg == "-x" || arg == "--exclusive") options.flags |= exclusive;
		else if (arg == "-n" || arg == "--nonblock")  options.wait = 0.0;
		else return arg == "-o" || arg == "-u";
		return true;
	}

	static bool parseFlockArgs(const std::vector<std::string>& args, FlockOptions& options) {
		std::size_t i = 0;
		for (; i < args.size() && args[i].size() > 1 && args[i][0] == '-'; ++i) {
			if (!applyFlockOption(args, i, options)) return false;
		}

		options.first = i;
		return i + 1 < args.size();
	}

	static bool acquireLock(HANDLE file, const FlockOptions& options) {
		const long long wait_ms = static_cast<long long>((std::max)(options.wait, 0.0) * 1000.0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
		const DWORD mode = options.wait < 0.0 ? options.flags
			: options.flags | LOCKFILE_FAIL_IMMEDIATELY;
		for (;;) {
			OVERLAPPED overlapped{};
			if (::LockFileEx(file, mode, 0, MAXDWORD, MAXDWORD, &overlapped) != 0) return true;
			if (options.wait < 0.0 || std::chrono::steady_clock::now() >= deadline) return false;
			::Sleep(kFlockPollMs);
		}
	}

	static HANDLE openLockFile(Executor& exec, const std::string& path) {
		const std::wstring native = toNative(exec, path).wstring();
		return ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	static int builtin_flock(Executor& exec, const std::vector<std::string>& args) {
		FlockOptions options;
		if (!parseFlockArgs(args, options)) {
			perr("flock", "usage: flock [-sxn] [-w SECS] FILE COMMAND [ARG]... | -c COMMAND");
			return kFlockUsageStatus;
		}

		const HANDLE file = openLockFile(exec, args[options.first]);
		if (file == INVALID_HANDLE_VALUE) {
			perr("flock", args[options.first] + ": cannot open lock file");
			return 1;
		}

		if (!acquireLock(file, options)) {
			::CloseHandle(file);
			return 1;
		}

		std::vector<std::string> command(
			args.begin() + static_cast<std::ptrdiff_t>(options.first) + 1, args.end());
		const bool shell_string = command.size() == 2
			&& (command[0] == "-c" || command[0] == "--command");
		const int status = shell_string ? exec.executeText(command[1], "flock")
			: exec.runResolvedCommand(command, {});
		::CloseHandle(file);
		return status;
	}

	static bool dcPop(DcState& state, DcValue& value) {
		if (state.stack.empty()) {
			std::fprintf(stderr, "dc: stack empty\n");
			state.failed = true;
			return false;
		}

		value = state.stack.back();
		state.stack.pop_back();
		return true;
	}

	static void dcPrint(const DcValue& value) {
		std::printf("%.*f\n", value.scale, value.number);
	}

	// Result scales follow dc(1): sums keep the larger operand scale,
	// products are capped by the larger of the operands and k, quotients
	// and roots use k.
	static int dcResultScale(char op, const DcValue& left, const DcValue& right, int scale) {
		const int larger = (std::max)(left.scale, right.scale);
		const int exponent = static_cast<int>(right.number);
		switch (op) {
		case '+':
		case '-': return larger;
		case '*': return (std::min)(left.scale + right.scale, (std::max)(scale, larger));
		case '^': return (std::min)(left.scale * exponent, (std::max)(scale, left.scale));
		case '/':
		case '%': return scale;
		default:  return 0;
		}
	}

	static double dcTruncate(double value, int scale) {
		const double factor = std::pow(10.0, scale);
		return std::trunc(value * factor) / factor;
	}

	static double dcApply(char op, double left, double right) {
		switch (op) {
		case '+': return left + right;
		case '-': return left - right;
		case '*': return left * right;
		case '/': return left / right;
		case '%': return std::fmod(left, right);
		case '^': return std::pow(left, right);
		default:  return 0.0;
		}
	}

	static void dcBinary(DcState& state, char op) {
		DcValue right;
		DcValue left;
		if (!dcPop(state, right) || !dcPop(state, left)) return;
		if ((op == '/' || op == '%') && right.number == 0.0) {
			std::fprintf(stderr, "dc: divide by zero\n");
			state.stack.push_back(left);
			state.stack.push_back(right);
			return;
		}

		DcValue result;
		result.scale = (std::max)(0, dcResultScale(op, left, right, state.scale));
		result.number = dcTruncate(dcApply(op, left.number, right.number), result.scale);
		state.stack.push_back(result);
	}

	static void dcSwapTop(DcState& state) {
		if (state.stack.size() < 2) return;
		std::swap(state.stack.back(), state.stack[state.stack.size() - 2]);
	}

	static void dcPrintStack(const DcState& state) {
		for (auto it = state.stack.rbegin(); it != state.stack.rend(); ++it) dcPrint(*it);
	}

	static void dcSquareRoot(DcState& state) {
		DcValue value;
		if (!dcPop(state, value)) return;
		value.scale = (std::max)(state.scale, value.scale);
		value.number = dcTruncate(std::sqrt(value.number), value.scale);
		state.stack.push_back(value);
	}

	static void dcSetScale(DcState& state) {
		DcValue value;
		if (dcPop(state, value)) state.scale = static_cast<int>(value.number);
	}

	static void dcPushDepth(DcState& state) {
		DcValue depth;
		depth.number = static_cast<double>(state.stack.size());
		state.stack.push_back(depth);
	}

	static void dcStackCommand(DcState& state, char op) {
		DcValue value;
		switch (op) {
		case 'p': if (!state.stack.empty()) dcPrint(state.stack.back());               break;
		case 'n': if (dcPop(state, value)) std::printf("%.*f", value.scale, value.number); break;
		case 'f': dcPrintStack(state);                                                 break;
		case 'c': state.stack.clear();                                                 break;
		case 'd': if (!state.stack.empty()) state.stack.push_back(state.stack.back()); break;
		case 'r': dcSwapTop(state);                                                    break;
		case 'v': dcSquareRoot(state);                                                 break;
		case 'k': dcSetScale(state);                                                   break;
		case 'K': state.stack.push_back(DcValue{ static_cast<double>(state.scale), 0 }); break;
		case 'z': dcPushDepth(state);                                                  break;
		default:
			std::fprintf(stderr, "dc: '%c' (%#o) unimplemented\n", op, static_cast<unsigned>(op));
			break;
		}
	}

	static bool isDcNumberChar(char c) {
		return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.';
	}

	static std::size_t dcNumber(const std::string& text, std::size_t at, DcState& state) {
		std::size_t end = at;
		std::string digits;
		if (text[end] == '_') {
			digits.push_back('-');
			++end;
		}

		while (end < text.size() && isDcNumberChar(text[end])) digits.push_back(text[end++]);

		DcValue value;
		parseDouble(digits, value.number);
		const std::size_t dot = digits.find('.');
		value.scale = dot == std::string::npos ? 0 : static_cast<int>(digits.size() - dot - 1);
		state.stack.push_back(value);
		return end;
	}

	static void runDc(const std::string& program, DcState& state) {
		for (std::size_t at = 0; at < program.size() && !state.failed;) {
			const char c = program[at];
			if (isDcNumberChar(c) || c == '_') {
				at = dcNumber(program, at, state);
				continue;
			}

			++at;
			if (std::isspace(static_cast<unsigned char>(c)) != 0) continue;
			if (c == 'q') return;
			if (std::strchr("+-*/%^", c) != nullptr) dcBinary(state, c);
			else                                     dcStackCommand(state, c);
		}
	}

	static int builtin_dc(Executor& exec, const std::vector<std::string>& args) {
		DcState state;
		std::vector<std::string> sources;
		bool had_expression = false;
		for (std::size_t i = 0; i < args.size(); ++i) {
			if ((args[i] == "-e" || args[i] == "--expression") && i + 1 < args.size()) {
				runDc(args[++i], state);
				had_expression = true;
				continue;
			}

			sources.push_back(args[i]);
		}

		if (sources.empty() && !had_expression) sources.push_back("-");
		for (const std::string& source : sources) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, source, lines)) {
				perr("dc", source + ": " + std::strerror(errno));
				return 1;
			}

			for (const std::string& line : lines) runDc(line, state);
		}

		std::fflush(stdout);
		return state.failed ? 1 : 0;
	}

	void registerMiscBuiltins(Executor& exec) {
		exec.registerBuiltin("ts",     builtin_ts);
		exec.registerBuiltin("lsattr", builtin_lsattr);
		exec.registerBuiltin("chattr", builtin_chattr);
		exec.registerBuiltin("ipcalc", builtin_ipcalc);
		exec.registerBuiltin("iconv",  builtin_iconv);
		exec.registerBuiltin("flock",  builtin_flock);
		exec.registerBuiltin("dc",     builtin_dc);
	}

#else

	void registerMiscBuiltins(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh
