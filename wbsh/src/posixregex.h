#pragma once

/**
 * @file posixregex.h
 * @brief POSIX basic / extended regular expressions (with the GNU
 *        escapes \w \s \b \< \> \n \t and back-references) on top of
 *        std::regex's ECMAScript engine.
 *
 * MSVC's std::regex::basic and ::extended grammars reject \n, \t, \w and
 * back-references, so patterns are translated to ECMAScript instead.
 * Two semantic gaps remain: alternation picks the first alternative
 * that matches rather than the longest, and a bracket expression is
 * matched by code unit.
 */

#include <regex>
#include <string>

namespace wbsh {

	/// ECMAScript text equivalent to the POSIX @p pattern.
	std::string posixToEcmascript(const std::string& pattern, bool extended);

	/// False (with @p out untouched) when the translated pattern is invalid.
	bool compilePosixRegex(std::regex& out, const std::string& pattern, bool extended,
	                       bool ignore_case);

}  // namespace wbsh
