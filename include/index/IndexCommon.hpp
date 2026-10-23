#pragma once
// IndexCommon.hpp - helpers shared by the index headers.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bpt {

// Prints the message (and errno, if set) and aborts. Used for failed syscalls and broken invariants.
[[noreturn]] inline void fatal(const std::string& msg) {
    int e = errno;
    if (e != 0) {
        std::fprintf(stderr, "Fatal (index): %s (errno %d: %s). Aborting.\n", msg.c_str(), e, std::strerror(e));
    } else {
        std::fprintf(stderr, "Fatal (index): %s. Aborting.\n", msg.c_str());
    }
    std::abort();
}

// Directory part of a path ("." if there is no slash).
inline std::string parentDir(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

} // namespace bpt
