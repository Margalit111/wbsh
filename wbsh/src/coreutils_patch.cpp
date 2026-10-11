/**
 * @file coreutils_patch.cpp
 * @brief patch: apply unified diffs (plain and git-style), with offset
 *        search, up to two lines of fuzz, reverse mode, rejects and
 *        CRLF-tolerant matching.
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace patch_detail {

		struct Hunk {
			long old_start = 0;
			long old_count = 0;
			long new_start = 0;
			long new_count = 0;
			std::vector<std::string> lines;
			bool old_missing_newline = false;
			bool new_missing_newline = false;
		};

		struct FilePatch {
			std::string old_name;
			std::string new_name;
			std::vector<Hunk> hunks;
		};

		struct PatchOptions {
			int strip = -1;
			bool reverse = false;
			bool forward = false;
			bool dry_run = false;
			bool silent = false;
			bool remove_empty = false;
			bool backup = false;
			std::string input;
			std::string output;
			std::string directory;
			std::string target;
		};

		struct TextFile {
			std::vector<std::string> lines;
			bool final_newline = true;
			bool crlf = false;
		};

		struct HunkSides {
			std::vector<std::string> before;
			std::vector<std::string> after;
		};

		struct Placement {
			long line = 0;
			int fuzz = 0;
		};

	}  // namespace patch_detail

	using patch_detail::FilePatch;
	using patch_detail::Hunk;
	using patch_detail::HunkSides;
	using patch_detail::PatchOptions;
	using patch_detail::Placement;
	using patch_detail::TextFile;

	static const int kMaxFuzz = 2;

	static bool startsWith(const std::string& text, const char* prefix) {
		return text.compare(0, std::strlen(prefix), prefix) == 0;
	}

	static std::string withoutCarriageReturn(std::string line) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		return line;
	}

	static std::string headerName(const std::string& line) {
		std::string name = line.substr(4);
		const std::size_t tab = name.find('\t');
		if (tab != std::string::npos) name.resize(tab);
		name = withoutCarriageReturn(name);
		if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
			name = name.substr(1, name.size() - 2);
		}

		return name;
	}

	// A range is "START" or "START,COUNT"; a missing count means one line.
	static bool parseRange(const std::string& text, long& start, long& count) {
		const std::size_t comma = text.find(',');
		long long parsed_start = 0;
		long long parsed_count = 1;
		if (!parseLL(text.substr(0, comma), parsed_start)) return false;
		const bool has_count = comma != std::string::npos;
		if (has_count && !parseLL(text.substr(comma + 1), parsed_count)) return false;

		start = static_cast<long>(parsed_start);
		count = static_cast<long>(parsed_count);
		return true;
	}

	static bool parseHunkHeader(const std::string& line, Hunk& hunk) {
		const std::size_t minus = line.find('-');
		const std::size_t plus = line.find(" +", minus);
		const std::size_t close = line.find(" @@", plus);
		if (minus == std::string::npos || plus == std::string::npos || close == std::string::npos) {
			return false;
		}

		const std::string old_range = line.substr(minus + 1, plus - minus - 1);
		const std::string new_range = line.substr(plus + 2, close - plus - 2);
		return parseRange(old_range, hunk.old_start, hunk.old_count)
			&& parseRange(new_range, hunk.new_start, hunk.new_count);
	}

	static void markMissingNewline(Hunk& hunk) {
		if (hunk.lines.empty()) return;
		const char kind = hunk.lines.back()[0];
		if (kind != '+') hunk.old_missing_newline = true;
		if (kind != '-') hunk.new_missing_newline = true;
	}

	static std::size_t readHunkBody(const std::vector<std::string>& lines, std::size_t at,
	                                Hunk& hunk) {
		long old_left = hunk.old_count;
		long new_left = hunk.new_count;
		while (at < lines.size() && (old_left > 0 || new_left > 0)) {
			std::string line = lines[at];
			if (line.empty() || line == "\r") line = " ";
			const char kind = line[0];
			if (kind == '\\') {
				markMissingNewline(hunk);
				++at;
				continue;
			}

			if (kind != ' ' && kind != '-' && kind != '+') break;
			if (kind != '+') --old_left;
			if (kind != '-') --new_left;
			hunk.lines.push_back(withoutCarriageReturn(line));
			++at;
		}

		if (at < lines.size() && startsWith(lines[at], "\\")) {
			markMissingNewline(hunk);
			++at;
		}

		return at;
	}

	static std::size_t readHunks(const std::vector<std::string>& lines, std::size_t at,
	                             FilePatch& patch) {
		while (at < lines.size() && startsWith(lines[at], "@@ ")) {
			Hunk hunk;
			if (!parseHunkHeader(lines[at], hunk)) break;
			at = readHunkBody(lines, at + 1, hunk);
			patch.hunks.push_back(hunk);
		}

		return at;
	}

	static std::vector<FilePatch> parsePatch(const std::vector<std::string>& lines) {
		std::vector<FilePatch> patches;
		for (std::size_t at = 0; at < lines.size();) {
			const bool header = startsWith(lines[at], "--- ") && at + 1 < lines.size()
				&& startsWith(lines[at + 1], "+++ ");
			if (!header) {
				++at;
				continue;
			}

			FilePatch patch;
			patch.old_name = headerName(lines[at]);
			patch.new_name = headerName(lines[at + 1]);
			at = readHunks(lines, at + 2, patch);
			patches.push_back(patch);
		}

		return patches;
	}

	static std::string stripComponents(const std::string& name, int strip) {
		if (strip < 0) {
			const std::size_t slash = name.find_last_of('/');
			return slash == std::string::npos ? name : name.substr(slash + 1);
		}

		std::string result = name;
		for (int k = 0; k < strip; ++k) {
			const std::size_t slash = result.find('/');
			if (slash == std::string::npos) return result;
			result.erase(0, slash + 1);
		}

		return result;
	}

	static bool isDevNull(const std::string& name) {
		return name == "/dev/null" || name == "NUL";
	}

	static bool fileExists(Executor& exec, const std::string& name) {
		std::error_code ec;
		return !name.empty() && fs::exists(toNative(exec, name), ec);
	}

	static std::string chooseTarget(Executor& exec, const FilePatch& patch,
	                                const PatchOptions& options) {
		if (!options.target.empty()) return options.target;

		const std::string old_name = isDevNull(patch.old_name) ? ""
			: stripComponents(patch.old_name, options.strip);
		const std::string new_name = isDevNull(patch.new_name) ? ""
			: stripComponents(patch.new_name, options.strip);
		if (new_name.empty()) return old_name;
		if (!fileExists(exec, new_name) && fileExists(exec, old_name)) return old_name;
		return new_name;
	}

	static bool readTextFile(Executor& exec, const std::string& name, TextFile& file) {
		std::FILE* in = fopenNative(exec, name, "rb");
		if (in == nullptr) return false;

		std::string content;
		char chunk[16384];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
			content.append(chunk, got);
		}

		std::fclose(in);
		file.final_newline = content.empty() || content.back() == '\n';
		file.crlf = content.find("\r\n") != std::string::npos;
		std::size_t start = 0;
		while (start < content.size()) {
			const std::size_t newline = content.find('\n', start);
			const std::size_t end = newline == std::string::npos ? content.size() : newline;
			file.lines.push_back(withoutCarriageReturn(content.substr(start, end - start)));
			start = end + 1;
		}

		return true;
	}

	static bool writeTextFile(Executor& exec, const std::string& name, const TextFile& file) {
		std::string content;
		const char* newline = file.crlf ? "\r\n" : "\n";
		for (std::size_t k = 0; k < file.lines.size(); ++k) {
			content += file.lines[k];
			if (k + 1 < file.lines.size() || file.final_newline) content += newline;
		}

		std::FILE* out = fopenNative(exec, name, "wb");
		if (out == nullptr) return false;
		const bool ok = std::fwrite(content.data(), 1, content.size(), out) == content.size();
		std::fclose(out);
		return ok;
	}

	static HunkSides splitHunk(const Hunk& hunk, bool reverse) {
		HunkSides sides;
		const char removed = reverse ? '+' : '-';
		const char added = reverse ? '-' : '+';
		for (const std::string& line : hunk.lines) {
			const std::string text = line.substr(1);
			if (line[0] != added) sides.before.push_back(text);
			if (line[0] != removed) sides.after.push_back(text);
		}

		return sides;
	}

	static std::size_t leadingContext(const Hunk& hunk) {
		std::size_t count = 0;
		while (count < hunk.lines.size() && hunk.lines[count][0] == ' ') ++count;
		return count;
	}

	static std::size_t trailingContext(const Hunk& hunk) {
		std::size_t count = 0;
		while (count < hunk.lines.size() && hunk.lines[hunk.lines.size() - 1 - count][0] == ' ') {
			++count;
		}

		return count;
	}

	static bool matchesAt(const TextFile& file, const std::vector<std::string>& before,
	                      long line, std::size_t skip_front, std::size_t skip_back) {
		const long needed = static_cast<long>(before.size() - skip_front - skip_back);
		if (line < 0 || line + needed > static_cast<long>(file.lines.size())) return false;

		for (long k = 0; k < needed; ++k) {
			const std::string& wanted = before[skip_front + static_cast<std::size_t>(k)];
			if (file.lines[static_cast<std::size_t>(line + k)] != wanted) return false;
		}

		return true;
	}

	static bool searchAround(const TextFile& file, const std::vector<std::string>& before,
	                         long expected, long floor, std::size_t skip_front,
	                         std::size_t skip_back, long& found) {
		const long limit = static_cast<long>(file.lines.size()) + 1;
		for (long distance = 0; distance <= limit; ++distance) {
			const long candidates[2] = { expected - distance, expected + distance };
			for (const long candidate : candidates) {
				if (candidate < floor || (distance == 0 && candidate != candidates[0])) continue;
				if (!matchesAt(file, before, candidate, skip_front, skip_back)) continue;
				found = candidate;
				return true;
			}
		}

		return false;
	}

	static bool placeHunk(const TextFile& file, const Hunk& hunk, const HunkSides& sides,
	                      long expected, long floor, Placement& placement) {
		const std::size_t front = leadingContext(hunk);
		const std::size_t back = trailingContext(hunk);
		for (int fuzz = 0; fuzz <= kMaxFuzz; ++fuzz) {
			const std::size_t skip_front = (std::min)(static_cast<std::size_t>(fuzz), front);
			const std::size_t skip_back = (std::min)(static_cast<std::size_t>(fuzz), back);
			if (skip_front + skip_back > sides.before.size()) break;

			long found = 0;
			const long shifted = expected + static_cast<long>(skip_front);
			if (!searchAround(file, sides.before, shifted, floor, skip_front, skip_back, found)) {
				continue;
			}

			placement.line = found - static_cast<long>(skip_front);
			placement.fuzz = fuzz;
			return true;
		}

		return false;
	}

	static void replaceLines(TextFile& file, const HunkSides& sides, long line,
	                         std::size_t skip_front, std::size_t skip_back) {
		const auto first = file.lines.begin() + line + static_cast<long>(skip_front);
		const auto last = first + static_cast<long>(sides.before.size() - skip_front - skip_back);
		const auto replacement_start = sides.after.begin() + static_cast<long>(skip_front);
		const std::vector<std::string> replacement(replacement_start,
			sides.after.end() - static_cast<long>(skip_back));
		file.lines.erase(first, last);
		file.lines.insert(file.lines.begin() + line + static_cast<long>(skip_front),
			replacement.begin(), replacement.end());
	}

	static std::string hunkText(const Hunk& hunk) {
		std::string text = "@@ -" + std::to_string(hunk.old_start) + ","
			+ std::to_string(hunk.old_count) + " +" + std::to_string(hunk.new_start) + ","
			+ std::to_string(hunk.new_count) + " @@\n";
		for (const std::string& line : hunk.lines) text += line + "\n";
		return text;
	}

	static void reportPlacement(const PatchOptions& options, std::size_t number, long line,
	                            long expected, int fuzz) {
		if (options.silent || (line == expected && fuzz == 0)) return;

		std::printf("Hunk #%zu succeeded at %ld", number, line + 1);
		if (fuzz > 0) std::printf(" with fuzz %d", fuzz);
		const long offset = line - expected;
		if (offset != 0) {
			std::printf(" (offset %ld line%s)", offset, (offset == 1 || offset == -1) ? "" : "s");
		}

		std::printf(".\n");
	}

	static bool applyHunk(TextFile& file, const Hunk& hunk, const PatchOptions& options,
	                      std::size_t number, long& offset, long& floor) {
		const HunkSides sides = splitHunk(hunk, options.reverse);
		const long start = options.reverse ? hunk.new_start : hunk.old_start;
		const long count = options.reverse ? hunk.new_count : hunk.old_count;
		const long expected = (count == 0 ? start : start - 1) + offset;

		Placement placement;
		if (!placeHunk(file, hunk, sides, expected, floor, placement)) return false;

		const std::size_t skip_front = (std::min)(static_cast<std::size_t>(placement.fuzz),
			leadingContext(hunk));
		const std::size_t skip_back = (std::min)(static_cast<std::size_t>(placement.fuzz),
			trailingContext(hunk));
		replaceLines(file, sides, placement.line, skip_front, skip_back);
		reportPlacement(options, number, placement.line, expected, placement.fuzz);

		offset += placement.line - expected + static_cast<long>(sides.after.size())
			- static_cast<long>(sides.before.size());
		floor = placement.line + static_cast<long>(sides.after.size());
		return true;
	}

	static bool alreadyApplied(const TextFile& file, const Hunk& hunk,
	                           const PatchOptions& options) {
		PatchOptions reversed = options;
		reversed.reverse = !options.reverse;
		const HunkSides sides = splitHunk(hunk, reversed.reverse);
		const long start = reversed.reverse ? hunk.new_start : hunk.old_start;
		Placement placement;
		return !sides.before.empty() && placeHunk(file, hunk, sides, start - 1, 0, placement);
	}

	static void saveRejects(Executor& exec, const std::string& target, std::size_t total,
	                        const std::vector<const Hunk*>& rejected, const PatchOptions& options) {
		const std::string name = target + ".rej";
		std::printf("%zu out of %zu hunk%s FAILED -- saving rejects to file %s\n", rejected.size(),
			total, total == 1 ? "" : "s", name.c_str());
		if (options.dry_run) return;

		std::FILE* out = fopenNative(exec, name, "wb");
		if (out == nullptr) return;
		std::fprintf(out, "--- %s\n+++ %s\n", target.c_str(), target.c_str());
		for (const Hunk* hunk : rejected) std::fputs(hunkText(*hunk).c_str(), out);
		std::fclose(out);
	}

	static void applyFinalNewline(TextFile& file, const FilePatch& patch,
	                              const PatchOptions& options) {
		if (patch.hunks.empty()) return;
		const Hunk& last = patch.hunks.back();
		const bool missing = options.reverse ? last.old_missing_newline : last.new_missing_newline;
		const bool was_missing = options.reverse ? last.new_missing_newline
			: last.old_missing_newline;
		if (missing) file.final_newline = false;
		else if (was_missing) file.final_newline = true;
	}

	static bool storeResult(Executor& exec, const std::string& target, const TextFile& file,
	                        const PatchOptions& options) {
		if (options.dry_run) return true;

		const std::string destination = options.output.empty() ? target : options.output;
		std::error_code ec;
		if (options.backup && fileExists(exec, target)) {
			fs::copy_file(toNative(exec, target), toNative(exec, target + ".orig"),
				fs::copy_options::overwrite_existing, ec);
		}

		const bool empty = file.lines.empty();
		if (empty && options.remove_empty && options.output.empty()) {
			fs::remove(toNative(exec, target), ec);
			return true;
		}

		const fs::path parent = toNative(exec, destination).parent_path();
		if (!parent.empty()) fs::create_directories(parent, ec);
		if (writeTextFile(exec, destination, file)) return true;

		perr("patch", destination + ": " + std::strerror(errno));
		return false;
	}

	static bool deletesFile(const FilePatch& patch, const PatchOptions& options) {
		return isDevNull(options.reverse ? patch.old_name : patch.new_name);
	}

	static int applyFilePatch(Executor& exec, const FilePatch& patch, const PatchOptions& options) {
		const std::string target = chooseTarget(exec, patch, options);
		if (!options.silent) {
			std::printf("%s file %s\n", options.dry_run ? "checking" : "patching", target.c_str());
		}

		TextFile file;
		const bool exists = readTextFile(exec, target, file);
		if (!exists) file.final_newline = true;

		long offset = 0;
		long floor = 0;
		std::vector<const Hunk*> rejected;
		for (std::size_t k = 0; k < patch.hunks.size(); ++k) {
			const Hunk& hunk = patch.hunks[k];
			if (applyHunk(file, hunk, options, k + 1, offset, floor)) continue;

			const bool applied = alreadyApplied(file, hunk, options);
			if (applied && options.forward) {
				if (!options.silent) std::printf("Hunk #%zu already applied; skipping.\n", k + 1);
				continue;
			}

			std::printf("Hunk #%zu FAILED at %ld%s.\n", k + 1, hunk.old_start,
				applied ? " (reversed or previously applied?)" : "");
			rejected.push_back(&hunk);
		}

		applyFinalNewline(file, patch, options);
		if (!rejected.empty()) saveRejects(exec, target, patch.hunks.size(), rejected, options);

		PatchOptions store = options;
		if (deletesFile(patch, options)) store.remove_empty = true;
		if (!storeResult(exec, target, file, store)) return 2;
		return rejected.empty() ? 0 : 1;
	}

	static bool isIgnoredPatchFlag(const std::string& arg) {
		return arg == "-t" || arg == "-f" || arg == "--batch" || arg == "--force"
			|| arg == "--binary" || arg == "-l" || arg == "-u" || arg == "--unified";
	}

	static bool applyPatchFlag(const std::string& arg, PatchOptions& options) {
		const bool silent = arg == "-s" || arg == "--silent" || arg == "--quiet";
		const bool remove_empty = arg == "-E" || arg == "--remove-empty-files";
		if (arg == "-R" || arg == "--reverse")      options.reverse = true;
		else if (arg == "-N" || arg == "--forward") options.forward = true;
		else if (arg == "--dry-run")                options.dry_run = true;
		else if (silent)                            options.silent = true;
		else if (remove_empty)                      options.remove_empty = true;
		else if (arg == "-b" || arg == "--backup")  options.backup = true;
		else return isIgnoredPatchFlag(arg);
		return true;
	}

	static bool takePatchValue(const std::vector<std::string>& args, std::size_t& i,
	                           const char* flag, std::string& value) {
		const std::string& arg = args[i];
		if (arg.size() > 2 && arg.compare(0, 2, flag) == 0) {
			value = arg.substr(arg[2] == '=' ? 3 : 2);
			return true;
		}

		if (arg != flag || i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool applyPatchValue(const std::vector<std::string>& args, std::size_t& i,
	                            PatchOptions& options, bool& valid) {
		std::string value;
		if (takePatchValue(args, i, "-p", value)) {
			valid = parseInt(value, options.strip) && options.strip >= 0;
			return true;
		}

		if (takePatchValue(args, i, "-i", value)) options.input = value;
		else if (takePatchValue(args, i, "-o", value)) options.output = value;
		else if (takePatchValue(args, i, "-d", value)) options.directory = value;
		else return false;
		return true;
	}

	static bool parsePatchArgs(const std::vector<std::string>& args, PatchOptions& options) {
		std::vector<std::string> operands;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() < 2 || arg[0] != '-') {
				operands.push_back(arg);
				continue;
			}

			bool valid = true;
			if (applyPatchValue(args, i, options, valid) && valid) continue;
			if (valid && applyPatchFlag(arg, options)) continue;
			perr("patch", "invalid option: " + arg);
			return false;
		}

		if (!operands.empty()) options.target = operands[0];
		if (operands.size() > 1) options.input = operands[1];
		return true;
	}

	static int applyAll(Executor& exec, const std::vector<FilePatch>& patches,
	                    const PatchOptions& options) {
		int status = 0;
		for (const FilePatch& patch : patches) {
			status = (std::max)(status, applyFilePatch(exec, patch, options));
		}

		std::fflush(stdout);
		return status;
	}

	// -d works in the named directory and then hands the shell its own
	// working directory (and $OLDPWD) back.
	static int applyInDirectory(Executor& exec, const std::vector<FilePatch>& patches,
	                            const PatchOptions& options) {
		const std::string home = exec.env().get("PWD");
		const std::string previous = exec.env().get("OLDPWD");
		std::string error;
		if (!changeDirectory(exec, options.directory, error)) {
			perr("patch", options.directory + ": " + error);
			return 2;
		}

		const int status = applyAll(exec, patches, options);
		changeDirectory(exec, home, error);
		exec.env().set("OLDPWD", previous);
		return status;
	}

	static int builtin_patch(Executor& exec, const std::vector<std::string>& args) {
		PatchOptions options;
		if (!parsePatchArgs(args, options)) return 2;

		std::vector<std::string> lines;
		const std::string input = options.input.empty() ? "-" : options.input;
		if (!readAllLines(exec, input, lines)) {
			perr("patch", input + ": " + std::strerror(errno));
			return 2;
		}

		const std::vector<FilePatch> patches = parsePatch(lines);
		if (patches.empty()) {
			perr("patch", "Only garbage was found in the patch input.");
			return 2;
		}

		if (options.directory.empty()) return applyAll(exec, patches, options);
		return applyInDirectory(exec, patches, options);
	}

	void registerPatchBuiltin(Executor& exec) {
		exec.registerBuiltin("patch", builtin_patch);
	}

}  // namespace wbsh
