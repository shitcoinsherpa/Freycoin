// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPAT_BIT_H
#define BITCOIN_COMPAT_BIT_H

#include <cstring>
#include <type_traits>

// Provide std::bit_cast compatibility for compilers with incomplete C++20 support
// (e.g., mingw-w64 GCC 10 which lacks std::bit_cast in libstdc++)

#if __cpp_lib_bit_cast >= 201806L
#include <bit>
// Use standard library bit_cast when available
using std::bit_cast;
#else
// Fallback implementation using memcpy (constexpr since C++20 for trivially copyable types)
template <class To, class From>
constexpr To bit_cast(const From& src) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bit_cast requires source and destination to have the same size");
    static_assert(std::is_trivially_copyable_v<From>, "bit_cast requires the source type to be trivially copyable");
    static_assert(std::is_trivially_copyable_v<To>, "bit_cast requires the destination type to be trivially copyable");
    static_assert(std::is_trivially_constructible_v<To>, "bit_cast requires the destination type to be trivially constructible");
    To dst;
    std::memcpy(&dst, &src, sizeof(To));
    return dst;
}
#endif

#endif // BITCOIN_COMPAT_BIT_H
