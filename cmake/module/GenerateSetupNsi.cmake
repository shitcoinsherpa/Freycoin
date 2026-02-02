# Copyright (c) 2023-present The Bitcoin Core developers
# Copyright (c) 2024-present The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "freycoin")
  set(BITCOIN_WRAPPER_NAME "freycoin")
  set(BITCOIN_GUI_NAME "freycoin-qt")
  set(BITCOIN_DAEMON_NAME "freycoind")
  set(BITCOIN_CLI_NAME "freycoin-cli")
  set(BITCOIN_TX_NAME "freycoin-tx")
  set(BITCOIN_WALLET_TOOL_NAME "freycoin-wallet")
  set(BITCOIN_TEST_NAME "test_freycoin")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/freycoin-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
