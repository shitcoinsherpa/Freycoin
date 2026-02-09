// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPAT_SOURCE_LOCATION_H
#define BITCOIN_COMPAT_SOURCE_LOCATION_H

// Provide std::source_location compatibility for compilers with incomplete C++20 support
// (e.g., mingw-w64 GCC which may lack std::source_location in libstdc++)

// Include <version> first to get feature test macros
#if __has_include(<version>)
#include <version>
#endif

#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L
#include <source_location>
namespace compat {
using source_location = std::source_location;
}
#else
// Fallback implementation using compiler builtins
namespace compat {
class source_location {
public:
    constexpr source_location() noexcept = default;

    static constexpr source_location current(
        const char* file = __builtin_FILE(),
        const char* func = __builtin_FUNCTION(),
        unsigned int line = __builtin_LINE(),
        unsigned int col = 0) noexcept
    {
        source_location loc;
        loc.m_file = file;
        loc.m_func = func;
        loc.m_line = line;
        loc.m_col = col;
        return loc;
    }

    constexpr const char* file_name() const noexcept { return m_file; }
    constexpr const char* function_name() const noexcept { return m_func; }
    constexpr unsigned int line() const noexcept { return m_line; }
    constexpr unsigned int column() const noexcept { return m_col; }

private:
    const char* m_file = "";
    const char* m_func = "";
    unsigned int m_line = 0;
    unsigned int m_col = 0;
};
} // namespace compat
#endif

#endif // BITCOIN_COMPAT_SOURCE_LOCATION_H
