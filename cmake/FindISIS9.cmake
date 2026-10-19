# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Locate and validate the external ISIS 9 runtime used by the renderer.

if(NOT DEFINED ISISROOT OR "${ISISROOT}" STREQUAL "")
  if(DEFINED ENV{ISISROOT} AND NOT "$ENV{ISISROOT}" STREQUAL "")
    set(ISISROOT "$ENV{ISISROOT}")
  else()
    message(FATAL_ERROR "ISISROOT must point to an ISIS 9 installation")
  endif()
endif()
file(REAL_PATH "${ISISROOT}" ISIS9_ROOT_DIR)
if(NOT EXISTS "${ISIS9_ROOT_DIR}/isis_version.txt")
  message(FATAL_ERROR "ISIS 9 version file is missing: ${ISIS9_ROOT_DIR}/isis_version.txt")
endif()
file(STRINGS "${ISIS9_ROOT_DIR}/isis_version.txt" _isis_version LIMIT_COUNT 1)
string(REGEX MATCH "^[^ \\t#]+" ISIS9_VERSION "${_isis_version}")
if(NOT ISIS9_VERSION STREQUAL "9.0.0")
  message(FATAL_ERROR "Expected ISIS 9.0.0, found ${ISIS9_VERSION}")
endif()
set(ISIS9_ROOT_DIR "${ISIS9_ROOT_DIR}" CACHE PATH "Validated ISIS root" FORCE)
add_library(ISIS9::isis SHARED IMPORTED)
set_target_properties(ISIS9::isis PROPERTIES
  IMPORTED_LOCATION "${ISIS9_ROOT_DIR}/lib/libisis.so"
  INTERFACE_INCLUDE_DIRECTORIES
    "${ISIS9_ROOT_DIR}/include/isis;${ISIS9_ROOT_DIR}/include;${ISIS9_ROOT_DIR}/include/cspice;${ISIS9_ROOT_DIR}/include/qt;${ISIS9_ROOT_DIR}/include/qt/QtCore"
  INTERFACE_LINK_LIBRARIES
    "${ISIS9_ROOT_DIR}/lib/libQt5Core.so;${ISIS9_ROOT_DIR}/lib/libcspice.so")
set(ISIS9_FOUND TRUE)
