#pragma once

/**
 * @file devices.h
 * @brief The generator devices Windows lacks: /dev/zero, /dev/random,
 *        /dev/urandom and /dev/full.
 *
 * Reading one yields a pipe that a background thread keeps full until the
 * last reader closes it, so the fd works everywhere a file would: as a
 * redirection, as a builtin's input file, and inherited by a child.
 */

#include <cstdio>
#include <string>

namespace wbsh {

	bool isGeneratorDevice(const std::string& posix_path);

	/// A readable fd streaming the device's bytes; -1 for any other path.
	int openGeneratorDevice(const std::string& posix_path);

	/// openGeneratorDevice wrapped in a binary-mode FILE*; nullptr for any
	/// other path.
	std::FILE* fopenGeneratorDevice(const std::string& posix_path);

}  // namespace wbsh
