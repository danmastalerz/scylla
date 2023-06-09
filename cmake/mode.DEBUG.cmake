if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  # -fasan -Og breaks some coroutines on aarch64, use -O0 instead
  set(default_Seastar_OptimizationLevel_DEBUG "g")
else()
  set(default_Seastar_OptimizationLevel_DEBUG "0")
endif()
set(Seastar_OptimizationLevel_DEBUG
  ${default_Seastar_OptimizationLevel_DEBUG}
  CACHE
  INTERNAL
  "")

set(Seastar_DEFINITIONS_DEBUG
  SCYLLA_BUILD_MODE=debug
  DEBUG
  SANITIZE
  DEBUG_LSA_SANITIZER
  SCYLLA_ENABLE_ERROR_INJECTION)

set(CMAKE_CXX_FLAGS_DEBUG
  " -O${Seastar_OptimizationLevel_DEBUG} -g -gz")
