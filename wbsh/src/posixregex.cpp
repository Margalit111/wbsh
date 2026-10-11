/**
 * @file posixregex.cpp
 * @brief The POSIX-to-ECMAScript pattern translator.
 */

#include "posixregex.h"

#include <cctype>
#include <cstring>

#include "regexutil.h"

namespace wbsh {

	namespace posixregex_detail {

		struct Translator {
			const std::string& in;
			bool extended;
			std::size_t at = 0;
			std::string out;
			bool atom_start = true;
		};

	}  // namespace posixregex_detail

	using posixregex_detail::Translator;

	static const char kEcmaSpecials[] = "\\^$.|?*+()[]{}/";

	static void appendLiteral(Translator& t, char c) {
		if (std::strchr(kEcmaSpecials, c) != nullptr) t.out.push_back('\\');
		t.out.push_back(c);
	}

	// Inside a POSIX bracket a backslash is an ordinary character and a
	// leading ']' is a member, both of which ECMAScript reads differently.
	static void translateBracket(Translator& t) {
		t.out.push_back('[');
		++t.at;
		if (t.at < t.in.size() && t.in[t.at] == '^') {
			t.out.push_back('^');
			++t.at;
		}

		if (t.at < t.in.size() && t.in[t.at] == ']') {
			t.out += "\\]";
			++t.at;
		}

		while (t.at < t.in.size() && t.in[t.at] != ']') {
			const char c = t.in[t.at];
			const bool named_class = c == '[' && t.at + 1 < t.in.size()
				&& std::strchr(":=.", t.in[t.at + 1]) != nullptr;
			if (named_class) {
				const std::size_t close = t.in.find(std::string(1, t.in[t.at + 1]) + "]", t.at + 2);
				const std::size_t end = close == std::string::npos ? t.in.size() : close + 2;
				t.out.append(t.in, t.at, end - t.at);
				t.at = end;
				continue;
			}

			if (c == '\\' || c == '[') t.out.push_back('\\');
			t.out.push_back(c);
			++t.at;
		}

		t.out.push_back(']');
		if (t.at < t.in.size()) ++t.at;
	}

	static bool translateGnuEscape(Translator& t, char c) {
		switch (c) {
		case 'n': t.out += "\\n";  return true;
		case 't': t.out += "\\t";  return true;
		case 'w': t.out += "\\w";  return true;
		case 'W': t.out += "\\W";  return true;
		case 's': t.out += "\\s";  return true;
		case 'S': t.out += "\\S";  return true;
		case 'b': t.out += "\\b";  return true;
		case 'B': t.out += "\\B";  return true;
		case '<':
		case '>': t.out += "\\b";  return true;
		case '`': t.out += "^";    return true;
		case '\'': t.out += "$";   return true;
		default:  return false;
		}
	}

	static bool translateBreOperatorEscape(Translator& t, char c) {
		if (t.extended || std::strchr("(){}|+?", c) == nullptr) return false;
		t.out.push_back(c);
		t.atom_start = c == '(' || c == '|';
		return true;
	}

	static void translateEscape(Translator& t) {
		++t.at;
		if (t.at >= t.in.size()) {
			t.out += "\\\\";
			return;
		}

		const char c = t.in[t.at++];
		t.atom_start = false;
		if (translateBreOperatorEscape(t, c)) return;
		if (c >= '1' && c <= '9') {
			t.out.push_back('\\');
			t.out.push_back(c);
			return;
		}

		if (translateGnuEscape(t, c)) return;
		if (std::isalnum(static_cast<unsigned char>(c)) != 0) t.out.push_back(c);
		else                                                 appendLiteral(t, c);
	}

	static bool breAnchorIsLiteral(const Translator& t, char c) {
		if (t.extended) return false;
		if (c == '^') return !t.atom_start;

		const std::size_t next = t.at + 1;
		const bool at_end = next >= t.in.size();
		const bool before_group_end = next + 1 < t.in.size() && t.in[next] == '\\'
			&& (t.in[next + 1] == ')' || t.in[next + 1] == '|');
		return !at_end && !before_group_end;
	}

	static void translatePlain(Translator& t, char c) {
		++t.at;
		const bool bre_literal = !t.extended && std::strchr("(){}|+?", c) != nullptr;
		const bool leading_star = c == '*' && t.atom_start;
		if (bre_literal || leading_star) {
			appendLiteral(t, c);
			t.atom_start = false;
			return;
		}

		if (c == '.') t.out += "[\\s\\S]";
		else          t.out.push_back(c);
		t.atom_start = c == '(' || c == '|' || c == '^';
	}

	static void translateNext(Translator& t) {
		const char c = t.in[t.at];
		if (c == '[') {
			translateBracket(t);
			t.atom_start = false;
			return;
		}

		if (c == '\\') {
			translateEscape(t);
			return;
		}

		if ((c == '^' || c == '$') && breAnchorIsLiteral(t, c)) {
			appendLiteral(t, c);
			++t.at;
			t.atom_start = false;
			return;
		}

		translatePlain(t, c);
	}

	std::string posixToEcmascript(const std::string& pattern, bool extended) {
		Translator t{ pattern, extended };
		while (t.at < pattern.size()) translateNext(t);
		return t.out;
	}

	bool compilePosixRegex(std::regex& out, const std::string& pattern, bool extended,
	                       bool ignore_case) {
		std::regex::flag_type flags = std::regex::ECMAScript;
		if (ignore_case) flags |= std::regex::icase;
		return compileRegex(out, posixToEcmascript(pattern, extended), flags);
	}

}  // namespace wbsh
