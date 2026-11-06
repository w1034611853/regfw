cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()
project(register_framework_cpp VERSION 0.38.0 LANGUAGES C CXX)
# Implementation modules: public include/regfw/* paths remain unchanged.
add_library(regfw STATIC
  src/dsl/declarations.cpp
  src/layout/address_view.cpp
  src/layout/formula.cpp
  src/layout/instantiate.cpp
  src/runtime/access.cpp
  src/runtime/async.cpp
  src/runtime/behaviors.cpp
  src/runtime/bindings.cpp
  src/runtime/callbacks.cpp
  src/runtime/handles.cpp
  src/runtime/lifecycle.cpp
  src/runtime/storage.cpp
  src/snapshot/snapshot.cpp
  src/snapshot/state.cpp
  src/diagnostics/errors.cpp
  src/diagnostics/inspection.cpp
  src/diagnostics/layout_report.cpp
  src/diagnostics/logging.cpp
)
# Only first-party implementation files can see src/internal/*.
target_include_directories(regfw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(regfw PROPERTIES POSITION_INDEPENDENT_CODE ON INTERPROCEDURAL_OPTIMIZATION FALSE)
add_library(regfw::regfw ALIAS regfw)
# Single production backend: shared prepared EntityLayout Hash.
target_compile_features(regfw PUBLIC cxx_std_20)
# Native unions expose raw and bitfield references to the same object.
target_compile_options(regfw PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-strict-aliasing>)
target_include_directories(regfw PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(NOT HLog_ROOT AND NOT HLOG_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hlog/include/hlog.h")
  set(HLog_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hlog")
endif()
find_package(HLog REQUIRED MODULE)
target_link_libraries(regfw PUBLIC HLog::HLog)
find_package(Threads REQUIRED)
target_link_libraries(regfw PUBLIC Threads::Threads)
include(cmake/RegfwPch.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/regfwConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/regfwConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/regfw")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/regfwConfigVersion.cmake" VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(TARGETS regfw EXPORT regfwTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT regfwTargets NAMESPACE regfw:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/regfw)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/regfwConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/regfwConfigVersion.cmake" cmake/RegfwPch.cmake cmake/FindHLog.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/regfw)

option(REGFW_BUILD_EXAMPLES "Build quickstart and PF/VF examples" OFF)
set(_regfw_test_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(_regfw_test_default ON)
endif()
option(REGFW_BUILD_TESTS "Build and register regfw verification tests" ${_regfw_test_default})

if(REGFW_BUILD_EXAMPLES OR REGFW_BUILD_TESTS)
  foreach(example IN ITEMS quickstart pf_vf)
    add_executable(regfw_${example} examples/${example}.cpp)
    target_link_libraries(regfw_${example} PRIVATE regfw::regfw)
  endforeach()
endif()

if(REGFW_BUILD_TESTS)
  enable_testing()
  add_executable(regfw_async_test tests/async.cpp)
  target_link_libraries(regfw_async_test PRIVATE regfw::regfw)
  add_test(NAME async COMMAND regfw_async_test)
  add_test(NAME example_quickstart COMMAND regfw_quickstart)
  add_test(NAME example_pf_vf COMMAND regfw_pf_vf)
  add_test(NAME example_pf_vf_full COMMAND regfw_pf_vf --layout-full)
  add_test(NAME example_pf_vf_explain COMMAND regfw_pf_vf --explain)

  add_executable(regfw_layout_report_test tests/layout_report.cpp)
  target_include_directories(regfw_layout_report_test PRIVATE examples)
  target_link_libraries(regfw_layout_report_test PRIVATE regfw::regfw)
  add_test(NAME layout_report COMMAND regfw_layout_report_test)
  foreach(test IN ITEMS init_validation resolver_safety resolver_lifetime entity_view view_validation diagnostics read_callbacks index_mapping logging address_view view_pre_access fast_path dispatch_index static_layout symbolic_layout mapped_logging formula_diff formula_geometry formula_golden formula_fastpaths formula_regressions)
    add_executable(regfw_${test}_test tests/${test}.cpp)
    target_link_libraries(regfw_${test}_test PRIVATE regfw::regfw)
    add_test(NAME ${test} COMMAND regfw_${test}_test)
  endforeach()

  # Geometry errors formerly checked by the consteval compiler are now
  # validated by the native single-pass installer without changing DSL usage.
  add_executable(regfw_static_runtime_errors_test tests/static_runtime_errors.cpp)
  target_link_libraries(regfw_static_runtime_errors_test PRIVATE regfw::regfw)
  add_test(NAME static_runtime_errors COMMAND regfw_static_runtime_errors_test)

  add_executable(regfw_snapshot_test tests/snapshot.cpp)
  target_include_directories(regfw_snapshot_test PRIVATE extras tests/snapshot_backend)
  target_link_libraries(regfw_snapshot_test PRIVATE regfw::regfw)
  add_test(NAME snapshot COMMAND regfw_snapshot_test)

  add_executable(regfw_poe_compiled_test docs/snippets/poe_compiled_layout.cpp)
  target_link_libraries(regfw_poe_compiled_test PRIVATE regfw::regfw)
  add_test(NAME poe_compiled_example COMMAND regfw_poe_compiled_test)

  add_executable(regfw_symbolic_odr_test tests/symbolic_odr.cpp tests/symbolic_odr_other.cpp)
  target_link_libraries(regfw_symbolic_odr_test PRIVATE regfw::regfw)
  add_test(NAME symbolic_odr COMMAND regfw_symbolic_odr_test)

  add_executable(regfw_layout_separate_test tests/layout_separate.cpp tests/layout_separate_impl.cpp tests/layout_separate_other.cpp)
  target_link_libraries(regfw_layout_separate_test PRIVATE regfw::regfw)
  add_test(NAME layout_separate COMMAND regfw_layout_separate_test)

  # Light declaration entry: consumers include only <regfw/bundle.hpp>; the
  # layout implementation TU provides REG_LAYOUT_IMPL with the full entry.
  add_executable(regfw_bundle_light_test tests/bundle_light.cpp tests/bundle_light_impl.cpp tests/bundle_light_consumer.cpp)
  target_link_libraries(regfw_bundle_light_test PRIVATE regfw::regfw)
  add_test(NAME bundle_light COMMAND regfw_bundle_light_test)

  # End-to-end coverage for the provided PCH helper: the same device sources
  # compiled with the bundle header precompiled.
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RegfwPch.cmake)
  add_executable(regfw_bundle_pch_test tests/bundle_light.cpp tests/bundle_light_impl.cpp tests/bundle_light_consumer.cpp)
  target_link_libraries(regfw_bundle_pch_test PRIVATE regfw::regfw)
  regfw_precompile_bundle(regfw_bundle_pch_test tests/bundle_light.hpp)
  add_test(NAME bundle_pch COMMAND regfw_bundle_pch_test)

  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  add_test(NAME static_compile_fail COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/static_compile_fail.py ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CXX_COMPILER})
  add_test(NAME documentation COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/update_docs.py --check --build-dir ${CMAKE_CURRENT_BINARY_DIR})
  # These tests reconfigure the process-global hlog backend and inspect its
  # default log file; concurrent test processes must not truncate each other.
  set_tests_properties(layout_report logging mapped_logging fast_path resolver_safety address_view diagnostics PROPERTIES RUN_SERIAL TRUE)
endif()

# Optional formula-backend performance binaries.  Never built by the regular
# library/tests targets; opt in with -DREGFW_BUILD_BENCHMARKS=ON.
option(REGFW_BUILD_BENCHMARKS "Build formula backend microbenchmarks" OFF)
if(REGFW_BUILD_BENCHMARKS)
  add_executable(bench_init_formula benchmarks/init.cpp)
  add_executable(bench_mmio_dynamic benchmarks/mmio.cpp)
  add_executable(bench_mmio_formula benchmarks/mmio.cpp)
  add_executable(bench_scale_formula benchmarks/formula_scale.cpp)
  target_compile_definitions(bench_mmio_formula PRIVATE REGFW_STATIC_BENCH=1)
  foreach(bench IN ITEMS init_formula mmio_dynamic mmio_formula scale_formula)
    target_link_libraries(bench_${bench} PRIVATE regfw::regfw)
  endforeach()
endif()
