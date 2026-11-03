# Cache the device's actual bundle/union declarations, not only the framework.
# Call after defining and linking the device target.
function(regfw_precompile_bundle target bundle_header)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "regfw_precompile_bundle: target '${target}' does not exist")
  endif()
  get_filename_component(_regfw_bundle_header "${bundle_header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  target_precompile_headers(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_regfw_bundle_header}>")
endfunction()
