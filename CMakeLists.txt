cmake_minimum_required(VERSION 3.16)

project(libat-c
    VERSION     0.1.0
    DESCRIPTION "Zero-malloc C11 AT command engine for embedded GSM/LTE modems"
    LANGUAGES   C
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LIBATC_BUILD_TESTS    "Build the test suite (requires host compiler)" ON)
option(LIBATC_BUILD_EXAMPLES "Build host simulation example"                  ON)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(atc
    src/at.c
    src/at_gsm.c
)
add_library(at::atc ALIAS atc)

target_include_directories(atc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(atc PUBLIC c_std_11)

target_compile_options(atc PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes
    >
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# ---------------------------------------------------------------------------
# Embedded integration hint
#
# For bare-metal targets, override these cache variables from your toolchain
# file or add_subdirectory call:
#
#   set(AT_CFG_RX_BUF_SIZE  256 CACHE STRING "")
#   set(AT_CFG_LOG_LEVEL      0 CACHE STRING "")
#
# They are forwarded as compile definitions so at_config.h picks them up
# without modifying the header.
# ---------------------------------------------------------------------------
foreach(_def
        AT_CFG_RX_BUF_SIZE
        AT_CFG_TX_BUF_SIZE
        AT_CFG_LINE_MAX
        AT_CFG_RESP_LINES_MAX
        AT_CFG_CMD_QUEUE_DEPTH
        AT_CFG_URC_TABLE_SIZE
        AT_CFG_DEFAULT_TIMEOUT_MS
        AT_CFG_PROMPT_TIMEOUT_MS
        AT_CFG_ECHO_CANCEL
        AT_CFG_LOG_LEVEL)
    if(DEFINED ${_def})
        target_compile_definitions(atc PUBLIC "${_def}=${${_def}}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Tests (host only — wraps engine in a POSIX thread simulation)
# ---------------------------------------------------------------------------
if(LIBATC_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        unity
        GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
        GIT_TAG        v2.6.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(unity)

    enable_testing()

    add_executable(test_engine   tests/test_engine.c)
    add_executable(test_gsm      tests/test_gsm.c)
    add_executable(test_fmt      tests/test_fmt.c)

    foreach(_t test_engine test_gsm test_fmt)
        target_link_libraries(${_t} PRIVATE at::atc unity)
        target_compile_features(${_t} PRIVATE c_std_11)
        target_compile_options(${_t} PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
                -Wall -Wextra -Wpedantic -Wshadow
            >
        )
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(LIBATC_BUILD_EXAMPLES AND NOT CMAKE_CROSSCOMPILING)
    add_executable(host_simulation examples/host_simulation.c)
    target_link_libraries(host_simulation PRIVATE at::atc)
    target_compile_features(host_simulation PRIVATE c_std_11)
    if(UNIX)
        target_link_libraries(host_simulation PRIVATE pthread)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS atc
    EXPORT  libatc-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(EXPORT libatc-targets
    FILE        libatc-targets.cmake
    NAMESPACE   at::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libatc
)
