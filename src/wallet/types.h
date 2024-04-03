// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2013-present The Riecoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! @file Public type definitions that are used inside and outside of the wallet
//! (e.g. by src/wallet and src/interfaces and src/qt code).
//!
//! File is home for simple enum and struct definitions that don't deserve
//! separate header files. More complicated wallet public types like
//! CCoinControl that are used externally can have separate headers.

#ifndef BITCOIN_WALLET_TYPES_H
#define BITCOIN_WALLET_TYPES_H

#include <type_traits>

namespace wallet {
/**
 * IsMine() return codes, which depend on ScriptPubKeyMan implementation.
 * Not every ScriptPubKeyMan covers all types, please refer to
 * https://github.com/bitcoin/bitcoin/blob/master/doc/release-notes/release-notes-0.21.0.md#ismine-semantics
 * for better understanding.
 *
 * For DescriptorScriptPubKeyMan and future ScriptPubKeyMan,
 * ISMINE_NO: the scriptPubKey is not in the wallet;
 * ISMINE_SPENDABLE: the scriptPubKey matches a scriptPubKey in the wallet.
 * ISMINE_USED: the scriptPubKey corresponds to a used address owned by the wallet user.
 * ISMINE_ENUM_ELEMENTS: the number of isminetype enum elements.
 *
 */
enum isminetype : unsigned int {
    ISMINE_NO         = 0,
    ISMINE_SPENDABLE  = 1 << 0,
    ISMINE_USED       = 1 << 1,
    ISMINE_ENUM_ELEMENTS = 4,
};
/** used for bitflags of isminetype */
using isminefilter = std::underlying_type<isminetype>::type;

/**
 * Address purpose field that has been been stored with wallet sending and
 * receiving addresses since BIP70 payment protocol support was added in
 * https://github.com/bitcoin/bitcoin/pull/2539. This field is not currently
 * used for any logic inside the wallet, but it is still shown in RPC and GUI
 * interfaces and saved for new addresses. It is basically redundant with an
 * address's IsMine() result.
 */
enum class AddressPurpose {
    RECEIVE,
    SEND,
};
} // namespace wallet

#endif // BITCOIN_WALLET_TYPES_H
