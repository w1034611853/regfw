# Reuse the project's native hlog target when one already exists.
if(TARGET HLog::HLog)
  set(HLog_FOUND TRUE)
  return()
endif()
foreach(_hlog_target hlog::hlog hlog)
  if(TARGET ${_hlog_target})
    add_library(HLog::HLog INTERFACE IMPORTED GLOBAL)
    set_property(TARGET HLog::HLog PROPERTY INTERFACE_LINK_LIBRARIES ${_hlog_target})
    set(HLog_FOUND TRUE)
    return()
  endif()
endforeach()

find_path(HLog_INCLUDE_DIR NAMES hlog.h
  HINTS ${HLog_ROOT} ${HLOG_ROOT} ENV HLOG_ROOT PATH_SUFFIXES include)
# A Rust HLog checkout keeps its freshly built shared library under
# target/release.  Prefer it over the vendored Linux libhlog.so (which is not
# linkable on macOS) when the caller points HLog_ROOT at the checkout.
if(HLog_ROOT)
  set(_hlog_release "${HLog_ROOT}/target/release/libhlog${CMAKE_SHARED_LIBRARY_SUFFIX}")
  if(EXISTS "${_hlog_release}")
    set(HLog_LIBRARY "${_hlog_release}" CACHE FILEPATH "HLog shared library")
  endif()
endif()
find_library(HLog_LIBRARY NAMES hlog
  HINTS ${HLog_ROOT} ${HLOG_ROOT} ENV HLOG_ROOT PATH_SUFFIXES target/release lib lib64)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(HLog REQUIRED_VARS HLog_INCLUDE_DIR HLog_LIBRARY)
if(HLog_FOUND)
  add_library(HLog::HLog UNKNOWN IMPORTED GLOBAL)
  set_target_properties(HLog::HLog PROPERTIES
    IMPORTED_LOCATION "${HLog_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${HLog_INCLUDE_DIR}")
endif()
mark_as_advanced(HLog_INCLUDE_DIR HLog_LIBRARY)
