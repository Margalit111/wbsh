/**
 * @file devices.cpp
 * @brief Pipe-backed /dev/zero, /dev/random, /dev/urandom and /dev/full.
 */

#include "devices.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#  include <fcntl.h>
#  include <io.h>
#  pragma comment(lib, "bcrypt.lib")
#endif /* _WIN32 */

#include <thread>
#include <vector>

namespace wbsh {

	static const std::size_t kGeneratorChunk = 64 * 1024;

	bool isGeneratorDevice(const std::string& posix_path) {
		return posix_path == "/dev/zero" || posix_path == "/dev/full"
			|| posix_path == "/dev/random" || posix_path == "/dev/urandom";
	}

#ifdef _WIN32

	static void fillChunk(std::vector<unsigned char>& chunk, bool random) {
		if (!random) return;
		::BCryptGenRandom(nullptr, chunk.data(), static_cast<ULONG>(chunk.size()),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	}

	// Runs until WriteFile fails, which is what happens once every reader
	// has closed its end of the pipe.
	static void feedPipe(HANDLE write_end, bool random) {
		std::vector<unsigned char> chunk(kGeneratorChunk, 0);
		for (;;) {
			fillChunk(chunk, random);
			DWORD written = 0;
			if (::WriteFile(write_end, chunk.data(), static_cast<DWORD>(chunk.size()), &written,
			                nullptr) == 0) {
				break;
			}
		}

		::CloseHandle(write_end);
	}

	int openGeneratorDevice(const std::string& posix_path) {
		if (!isGeneratorDevice(posix_path)) return -1;

		HANDLE read_end = nullptr;
		HANDLE write_end = nullptr;
		const DWORD pipe_size = static_cast<DWORD>(kGeneratorChunk);
		if (::CreatePipe(&read_end, &write_end, nullptr, pipe_size) == 0) return -1;

		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_end), _O_RDONLY | _O_BINARY);
		if (fd < 0) {
			::CloseHandle(read_end);
			::CloseHandle(write_end);
			return -1;
		}

		const bool random = posix_path == "/dev/random" || posix_path == "/dev/urandom";
		std::thread(feedPipe, write_end, random).detach();
		return fd;
	}

#else

	int openGeneratorDevice(const std::string&) {
		return -1;
	}

#endif /* _WIN32 */

	std::FILE* fopenGeneratorDevice(const std::string& posix_path) {
		const int fd = openGeneratorDevice(posix_path);
		if (fd < 0) return nullptr;

		std::FILE* stream = _fdopen(fd, "rb");
		if (stream == nullptr) _close(fd);
		return stream;
	}

}  // namespace wbsh
