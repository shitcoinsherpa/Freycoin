# Copyright (c) 2024-present The Bitcoin Core developers
# Copyright (c) 2024-present The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_freycoin-qt
  apptests.cpp
  optiontests.cpp
  rpcnestedtests.cpp
  test_main.cpp
  uritests.cpp
  util.cpp
  ../../init/freycoin-qt.cpp
)

add_windows_application_manifest(test_freycoin-qt)

target_link_libraries(test_freycoin-qt
  core_interface
  freycoinqt
  test_util
  freycoin_node
  Boost::headers
  Qt6::Test
)

import_plugins(test_freycoin-qt)

if(ENABLE_WALLET)
  target_sources(test_freycoin-qt
    PRIVATE
      addressbooktests.cpp
      wallettests.cpp
      ../../wallet/test/wallet_test_fixture.cpp
  )
endif()

add_test(NAME test_freycoin-qt
  COMMAND test_freycoin-qt
)
if(WIN32 AND VCPKG_TARGET_TRIPLET)
  # On Windows, vcpkg configures Qt with `-opengl dynamic`, which makes
  # the "minimal" platform plugin unusable due to internal Qt bugs.
  set_tests_properties(test_freycoin-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows"
  )
endif()

install_binary_component(test_freycoin-qt INTERNAL)
