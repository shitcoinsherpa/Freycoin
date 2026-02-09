# Copyright (c) 2025-present The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

#[=======================================================================[.rst:
FindMPFR
--------

Find the GNU MPFR library (Multiple Precision Floating-Point Reliable Library).

IMPORTED Targets
^^^^^^^^^^^^^^^^

This module defines the following :prop_tgt:`IMPORTED` targets:

``MPFR::MPFR``
  The MPFR library, if found.

Result Variables
^^^^^^^^^^^^^^^^

This module will set the following variables in your project:

``MPFR_FOUND``
  True if MPFR is found.
``MPFR_INCLUDE_DIR``
  Include directory for MPFR headers.
``MPFR_LIBRARY``
  The MPFR library.

#]=======================================================================]

find_path(MPFR_INCLUDE_DIR
  NAMES mpfr.h
)

find_library(MPFR_LIBRARY
  NAMES mpfr
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MPFR
  REQUIRED_VARS MPFR_LIBRARY MPFR_INCLUDE_DIR
)

if(MPFR_FOUND AND NOT TARGET MPFR::MPFR)
  add_library(MPFR::MPFR UNKNOWN IMPORTED)
  set_target_properties(MPFR::MPFR PROPERTIES
    IMPORTED_LOCATION "${MPFR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPFR_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  MPFR_INCLUDE_DIR
  MPFR_LIBRARY
)
