# PyroWave: the codec and its C API from the Lavagnou/pyrowave submodule, built as a
# static library. The same commit is pinned by the LavArtemis clients, since the
# PyroWave bitstream has no version field (third-party/pyrowave/lavartemis/README.md).

if(SUNSHINE_ENABLE_PYROWAVE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave/lavartemis" "${CMAKE_BINARY_DIR}/third-party/pyrowave")
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES pyrowave::capi)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_PYROWAVE=1)
endif()
