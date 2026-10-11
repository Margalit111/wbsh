/**
 * @file coreutils_cpio.cpp
 * @brief cpio (read newc / crc / odc, write newc) and ar (read-only:
 *        list, extract, print; GNU and BSD long names).
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace cpio_detail {

		enum class CpioMode {
			None,
			Extract,
			Create,
			List,
		};

		struct CpioOptions {
			CpioMode mode = CpioMode::None;
			bool make_directories = false;
			bool verbose = false;
			bool unconditional = false;
			bool keep_mtime = false;
			std::string archive;
			std::vector<std::string> patterns;
		};

		struct CpioEntry {
			std::string name;
			std::uint32_t mode = 0;
			std::uint64_t size = 0;
			std::int64_t mtime = 0;
			const std::uint8_t* data = nullptr;
		};

		struct ArMember {
			std::string name;
			std::uint64_t size = 0;
			const std::uint8_t* data = nullptr;
		};

	}  // namespace cpio_detail

	using cpio_detail::ArMember;
	using cpio_detail::CpioEntry;
	using cpio_detail::CpioMode;
	using cpio_detail::CpioOptions;

	static const std::uint32_t kTypeMask      = 0170000;
	static const std::uint32_t kTypeDirectory = 0040000;
	static const std::uint32_t kTypeRegular   = 0100000;
	static const std::uint32_t kTypeSymlink   = 0120000;
	static const std::size_t kNewcHeader = 110;
	static const std::size_t kOdcHeader  = 76;
	static const std::size_t kArHeader   = 60;
	static const char kTrailer[] = "TRAILER!!!";

	static bool readWhole(Executor& exec, const std::string& path,
	                      std::vector<std::uint8_t>& bytes) {
		std::FILE* in = path.empty() || path == "-" ? stdin : fopenNative(exec, path, "rb");
		if (in == nullptr) return false;

		std::uint8_t chunk[65536];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
			bytes.insert(bytes.end(), chunk, chunk + got);
		}

		if (in != stdin) std::fclose(in);
		return true;
	}

	static std::uint64_t numberField(const std::uint8_t* field, std::size_t width, int base) {
		unsigned long long value = 0;
		parseULL(std::string(reinterpret_cast<const char*>(field), width), value, nullptr, base);
		return value;
	}

	static std::uint64_t hexField(const std::uint8_t* header, std::size_t index) {
		return numberField(header + 6 + index * 8, 8, 16);
	}

	static std::uint64_t octalField(const std::uint8_t* header, std::size_t offset,
	                                std::size_t width) {
		return numberField(header + offset, width, 8);
	}

	static std::size_t alignFour(std::size_t value) {
		return (value + 3) & ~static_cast<std::size_t>(3);
	}

	// newc / crc: 13 eight-digit hex fields; header plus name, and the data,
	// are each padded to a multiple of four bytes.
	static bool parseNewc(const std::vector<std::uint8_t>& archive, std::size_t& at,
	                      CpioEntry& entry) {
		if (at + kNewcHeader > archive.size()) return false;
		const std::uint8_t* header = archive.data() + at;
		entry.mode = static_cast<std::uint32_t>(hexField(header, 1));
		entry.mtime = static_cast<std::int64_t>(hexField(header, 5));
		entry.size = hexField(header, 6);
		const std::size_t name_size = static_cast<std::size_t>(hexField(header, 11));
		if (at + kNewcHeader + name_size > archive.size() || name_size == 0) return false;

		entry.name.assign(reinterpret_cast<const char*>(header + kNewcHeader), name_size - 1);
		const std::size_t data_at = alignFour(at + kNewcHeader + name_size);
		if (data_at + entry.size > archive.size()) return false;

		entry.data = archive.data() + data_at;
		at = alignFour(data_at + static_cast<std::size_t>(entry.size));
		return true;
	}

	// odc: fixed-width octal fields with no padding at all.
	static bool parseOdc(const std::vector<std::uint8_t>& archive, std::size_t& at,
	                     CpioEntry& entry) {
		if (at + kOdcHeader > archive.size()) return false;
		const std::uint8_t* header = archive.data() + at;
		entry.mode = static_cast<std::uint32_t>(octalField(header, 18, 6));
		entry.mtime = static_cast<std::int64_t>(octalField(header, 48, 11));
		const std::size_t name_size = static_cast<std::size_t>(octalField(header, 59, 6));
		entry.size = octalField(header, 65, 11);
		if (name_size == 0) return false;
		if (at + kOdcHeader + name_size + entry.size > archive.size()) return false;

		entry.name.assign(reinterpret_cast<const char*>(header + kOdcHeader), name_size - 1);
		entry.data = header + kOdcHeader + name_size;
		at += kOdcHeader + name_size + static_cast<std::size_t>(entry.size);
		return true;
	}

	static bool nextCpioEntry(const std::vector<std::uint8_t>& archive, std::size_t& at,
	                          CpioEntry& entry) {
		if (at + 6 > archive.size()) return false;
		const char* magic = reinterpret_cast<const char*>(archive.data() + at);
		if (std::strncmp(magic, "070701", 6) == 0 || std::strncmp(magic, "070702", 6) == 0) {
			return parseNewc(archive, at, entry);
		}

		if (std::strncmp(magic, "070707", 6) == 0) return parseOdc(archive, at, entry);
		return false;
	}

	static bool cpioSelected(const CpioOptions& options, const std::string& name) {
		if (options.patterns.empty()) return true;
		for (const std::string& pattern : options.patterns) {
			if (fnmatchFull(pattern, name)) return true;
		}

		return false;
	}

	static bool safeCpioName(const std::string& name) {
		if (name.empty() || name[0] == '/' || (name.size() > 1 && name[1] == ':')) return false;
		return name != ".." && name.compare(0, 3, "../") != 0
			&& name.find("/../") == std::string::npos
			&& (name.size() < 3 || name.compare(name.size() - 3, 3, "/..") != 0);
	}

	static bool extractCpioFile(Executor& exec, const CpioEntry& entry,
	                            const CpioOptions& options) {
		const fs::path path = toNative(exec, entry.name);
		std::error_code ec;
		if (!options.unconditional && fs::exists(path, ec)) {
			std::fprintf(stderr, "cpio: %s not created: newer or same age file exists\n",
				entry.name.c_str());
			return false;
		}

		if (options.make_directories) fs::create_directories(path.parent_path(), ec);
		std::FILE* out = fopenNative(exec, entry.name, "wb");
		if (out == nullptr) {
			std::fprintf(stderr, "cpio: %s: %s\n", entry.name.c_str(), std::strerror(errno));
			return false;
		}

		std::fwrite(entry.data, 1, static_cast<std::size_t>(entry.size), out);
		std::fclose(out);
		return true;
	}

	static bool extractCpioEntry(Executor& exec, const CpioEntry& entry,
	                             const CpioOptions& options) {
		if (!safeCpioName(entry.name)) {
			std::fprintf(stderr, "cpio: %s: refusing to write outside the current directory\n",
				entry.name.c_str());
			return false;
		}

		std::error_code ec;
		const std::uint32_t type = entry.mode & kTypeMask;
		if (type == kTypeDirectory) {
			fs::create_directories(toNative(exec, entry.name), ec);
			return true;
		}

		if (type == kTypeSymlink) {
			const std::string target(reinterpret_cast<const char*>(entry.data),
				static_cast<std::size_t>(entry.size));
			fs::create_symlink(utf8ToPath(target), toNative(exec, entry.name), ec);
			return !ec;
		}

		if (type != kTypeRegular) return true;
		return extractCpioFile(exec, entry, options);
	}

	static int readCpio(Executor& exec, const CpioOptions& options) {
		std::vector<std::uint8_t> archive;
		if (!readWhole(exec, options.archive, archive)) {
			perr("cpio", options.archive + ": " + std::strerror(errno));
			return 1;
		}

		int status = 0;
		std::uint64_t blocks = 0;
		std::size_t at = 0;
		CpioEntry entry;
		while (nextCpioEntry(archive, at, entry)) {
			if (entry.name == kTrailer) break;
			blocks += entry.size;
			if (!cpioSelected(options, entry.name)) continue;

			const bool list = options.mode == CpioMode::List;
			std::FILE* report = list ? stdout : stderr;
			if (list || options.verbose) std::fprintf(report, "%s\n", entry.name.c_str());
			if (!list && !extractCpioEntry(exec, entry, options)) status = 1;
		}

		if (at == 0) {
			perr("cpio", "unrecognized archive format");
			return 2;
		}

		std::fprintf(stderr, "%llu blocks\n", static_cast<unsigned long long>((at + 511) / 512));
		std::fflush(stdout);
		return status;
	}

	static void appendHexField(std::string& header, std::uint64_t value) {
		char text[16];
		const unsigned long long low = value & 0xFFFFFFFFULL;
		std::snprintf(text, sizeof(text), "%08llX", low);
		header += text;
	}

	static void appendNewcEntry(std::vector<std::uint8_t>& out, const std::string& name,
	                            std::uint32_t mode, std::int64_t mtime,
	                            const std::vector<std::uint8_t>& data) {
		static std::uint64_t inode = 1;
		std::string header = "070701";
		const std::uint64_t fields[13] = {
			inode++, mode, 0, 0, 1, static_cast<std::uint64_t>(mtime), data.size(), 0, 0, 0, 0,
			name.size() + 1, 0,
		};
		for (const std::uint64_t field : fields) appendHexField(header, field);

		out.insert(out.end(), header.begin(), header.end());
		out.insert(out.end(), name.begin(), name.end());
		out.push_back(0);
		while (out.size() % 4 != 0) out.push_back(0);
		out.insert(out.end(), data.begin(), data.end());
		while (out.size() % 4 != 0) out.push_back(0);
	}

	static std::int64_t mtimeOf(const fs::path& path) {
		std::error_code ec;
		const auto stamp = fs::last_write_time(path, ec);
		if (ec) return 0;
		return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count()
			- 11644473600LL;
	}

	static bool memberContents(Executor& exec, const std::string& name, fs::file_status status,
	                           std::uint32_t& mode, std::vector<std::uint8_t>& data) {
		if (fs::is_directory(status)) {
			mode = kTypeDirectory | 0755;
			return true;
		}

		if (!fs::is_symlink(status)) {
			mode = kTypeRegular | 0644;
			return readWhole(exec, name, data);
		}

		std::error_code ec;
		mode = kTypeSymlink | 0777;
		const std::string target = pathToUtf8(fs::read_symlink(toNative(exec, name), ec)
			.generic_wstring());
		data.assign(target.begin(), target.end());
		return true;
	}

	static bool addCpioMember(Executor& exec, const std::string& name,
	                          std::vector<std::uint8_t>& out) {
		const fs::path path = toNative(exec, name);
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(path, ec);
		if (ec || !fs::exists(status)) {
			std::fprintf(stderr, "cpio: %s: No such file or directory\n", name.c_str());
			return false;
		}

		std::vector<std::uint8_t> data;
		std::uint32_t mode = 0;
		if (!memberContents(exec, name, status, mode, data)) {
			std::fprintf(stderr, "cpio: %s: %s\n", name.c_str(), std::strerror(errno));
			return false;
		}

		appendNewcEntry(out, name, mode, mtimeOf(path), data);
		return true;
	}

	static int createCpio(Executor& exec, const CpioOptions& options) {
		std::vector<std::string> names;
		readAllLines(exec, "-", names);

		std::vector<std::uint8_t> out;
		int status = 0;
		for (const std::string& name : names) {
			if (name.empty()) continue;
			if (options.verbose) std::fprintf(stderr, "%s\n", name.c_str());
			if (!addCpioMember(exec, name, out)) status = 1;
		}

		appendNewcEntry(out, kTrailer, 0, 0, {});
		while (out.size() % 512 != 0) out.push_back(0);

		std::FILE* file = options.archive.empty() ? stdout
			: fopenNative(exec, options.archive, "wb");
		if (file == nullptr) {
			perr("cpio", options.archive + ": " + std::strerror(errno));
			return 1;
		}

		std::fwrite(out.data(), 1, out.size(), file);
		if (file != stdout) std::fclose(file);
		std::fflush(stdout);
		std::fprintf(stderr, "%llu blocks\n", static_cast<unsigned long long>(out.size() / 512));
		return status;
	}

	static bool applyCpioCluster(const std::string& cluster, CpioOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			switch (cluster[k]) {
			case 'i': options.mode = CpioMode::Extract;  break;
			case 'o': options.mode = CpioMode::Create;   break;
			case 't': options.mode = CpioMode::List;     break;
			case 'd': options.make_directories = true;  break;
			case 'v': options.verbose = true;           break;
			case 'u': options.unconditional = true;     break;
			case 'm': options.keep_mtime = true;        break;
			default:  return false;
			}
		}

		return true;
	}

	static bool parseCpioArgs(const std::vector<std::string>& args, CpioOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if ((arg == "-F" || arg == "--file") && has_value) {
				options.archive = args[++i];
				continue;
			}

			if (arg == "-H" && has_value) {
				++i;
				continue;
			}

			if (arg.compare(0, 7, "--file=") == 0) options.archive = arg.substr(7);
			else if (arg.size() < 2 || arg[0] != '-') options.patterns.push_back(arg);
			else if (!applyCpioCluster(arg, options)) return false;
		}

		if (options.mode == CpioMode::List && options.patterns.empty()) return true;
		return options.mode != CpioMode::None;
	}

	static int builtin_cpio(Executor& exec, const std::vector<std::string>& args) {
		CpioOptions options;
		if (!parseCpioArgs(args, options)) {
			perr("cpio", "usage: cpio -i|-o|-t [-dvu] [-F ARCHIVE] [PATTERN]...");
			return 2;
		}

		if (options.mode == CpioMode::Create) return createCpio(exec, options);
		return readCpio(exec, options);
	}

	static std::string arFieldText(const std::uint8_t* field, std::size_t width) {
		std::string text(reinterpret_cast<const char*>(field), width);
		while (!text.empty() && text.back() == ' ') text.pop_back();
		return text;
	}

	static std::string longArName(const std::string& table, std::size_t offset) {
		if (offset >= table.size()) return std::string();
		const std::size_t end = table.find_first_of("/\n", offset);
		return table.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
	}

	// GNU stores long names as "/OFFSET" into the "//" member; BSD as
	// "#1/LENGTH" with the name at the start of the member's data.
	static std::string resolveArName(std::string name, const std::string& long_names,
	                                 ArMember& member) {
		unsigned long long reference = 0;
		if (name.size() > 1 && name[0] == '/' && parseULL(name.substr(1), reference)) {
			return longArName(long_names, static_cast<std::size_t>(reference));
		}

		if (name.compare(0, 3, "#1/") == 0 && parseULL(name.substr(3), reference)) {
			const std::string bsd(reinterpret_cast<const char*>(member.data),
				static_cast<std::size_t>(reference));
			member.data += reference;
			member.size -= reference;
			return bsd;
		}

		if (!name.empty() && name.back() == '/') name.pop_back();
		return name;
	}

	static bool nextArMember(const std::vector<std::uint8_t>& archive, std::size_t& at,
	                         std::string& long_names, ArMember& member) {
		while (at + kArHeader <= archive.size()) {
			const std::uint8_t* header = archive.data() + at;
			std::string name = arFieldText(header, 16);
			unsigned long long size = 0;
			parseULL(arFieldText(header + 48, 10), size);
			const std::size_t data_at = at + kArHeader;
			if (data_at + size > archive.size()) return false;

			at = data_at + static_cast<std::size_t>(size) + (size % 2);
			if (name == "//") {
				const char* data = reinterpret_cast<const char*>(archive.data() + data_at);
				long_names.assign(data, static_cast<std::size_t>(size));
				continue;
			}

			if (name == "/" || name == "/SYM64/" || name == "__.SYMDEF") continue;

			member.data = archive.data() + data_at;
			member.size = size;
			member.name = resolveArName(name, long_names, member);
			return true;
		}

		return false;
	}

	static bool arSelected(const std::vector<std::string>& names, const std::string& name) {
		if (names.empty()) return true;
		for (const std::string& wanted : names) {
			if (wanted == name) return true;
		}

		return false;
	}

	static char arAction(const std::string& op) {
		for (const char action : { 'x', 'p', 't' }) {
			if (op.find(action) != std::string::npos) return action;
		}

		return '\0';
	}

	static bool loadArchive(Executor& exec, const std::string& path,
	                        std::vector<std::uint8_t>& archive) {
		if (readWhole(exec, path, archive) && archive.size() >= 8
		    && std::memcmp(archive.data(), "!<arch>\n", 8) == 0) {
			return true;
		}

		perr("ar", path + ": not an archive");
		return false;
	}

	static bool handleArMember(Executor& exec, const ArMember& member, char action, bool verbose) {
		const std::size_t size = static_cast<std::size_t>(member.size);
		if (action == 't' || verbose) {
			std::printf("%s%s\n", action == 'x' && verbose ? "x - " : "", member.name.c_str());
		}

		if (action == 'p') std::fwrite(member.data, 1, size, stdout);
		if (action != 'x') return true;

		std::FILE* out = fopenNative(exec, member.name, "wb");
		if (out == nullptr) {
			perr("ar", member.name + ": " + std::strerror(errno));
			return false;
		}

		std::fwrite(member.data, 1, size, out);
		std::fclose(out);
		return true;
	}

	static int builtin_ar(Executor& exec, const std::vector<std::string>& args) {
		const char action = args.size() >= 2 ? arAction(args[0]) : '\0';
		if (action == '\0') {
			perr("ar", "usage: ar -t|-x|-p [-v] ARCHIVE [MEMBER]... (read-only)");
			return 1;
		}

		std::vector<std::uint8_t> archive;
		if (!loadArchive(exec, args[1], archive)) return 1;

		const bool verbose = args[0].find('v') != std::string::npos;
		const std::vector<std::string> wanted(args.begin() + 2, args.end());
		std::size_t at = 8;
		std::string long_names;
		ArMember member;
		int status = 0;
		while (nextArMember(archive, at, long_names, member)) {
			if (!arSelected(wanted, member.name)) continue;
			if (!handleArMember(exec, member, action, verbose)) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

	void registerCpioBuiltins(Executor& exec) {
		exec.registerBuiltin("cpio", builtin_cpio);
		exec.registerBuiltin("ar",   builtin_ar);
	}

}  // namespace wbsh
