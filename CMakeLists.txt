cmake_minimum_required(VERSION 3.22)

project(
    Veloco
    VERSION 0.1.0
    LANGUAGES C
)

include(CTest)

option(VELOCO_ENABLE_URING "Build the io_uring backend" ON)
option(VELOCO_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)

find_package(Threads REQUIRED)

add_library(veloco_build_options INTERFACE)
target_compile_features(veloco_build_options INTERFACE c_std_11)
set_target_properties(veloco_build_options PROPERTIES C_EXTENSIONS ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        veloco_build_options
        INTERFACE -Wall -Wextra -Wpedantic -Werror
    )
endif()

if(VELOCO_ENABLE_SANITIZERS AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize=")
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require GCC or Clang")
    endif()
    target_compile_options(
        veloco_build_options
        INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(
        veloco_build_options
        INTERFACE -fsanitize=address,undefined
    )
endif()

if(VELOCO_ENABLE_URING)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
    endif()

    if(TARGET PkgConfig::LIBURING)
        set(VELOCO_URING_TARGET PkgConfig::LIBURING)
    else()
        find_path(VELOCO_LIBURING_INCLUDE_DIR liburing.h)
        find_library(VELOCO_LIBURING_LIBRARY uring)
        if(NOT VELOCO_LIBURING_INCLUDE_DIR OR NOT VELOCO_LIBURING_LIBRARY)
            message(
                FATAL_ERROR
                "VELOCO_ENABLE_URING=ON requires liburing development files"
            )
        endif()

        add_library(veloco_liburing UNKNOWN IMPORTED)
        set_target_properties(
            veloco_liburing
            PROPERTIES
                IMPORTED_LOCATION "${VELOCO_LIBURING_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${VELOCO_LIBURING_INCLUDE_DIR}"
        )
        set(VELOCO_URING_TARGET veloco_liburing)
    endif()
endif()

add_executable(veloco_tests tests/test_main.c)
target_include_directories(
    veloco_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(veloco_tests PRIVATE veloco_build_options Threads::Threads)

if(VELOCO_ENABLE_URING)
    target_link_libraries(veloco_tests PRIVATE "${VELOCO_URING_TARGET}")
    target_compile_definitions(veloco_tests PRIVATE VELOCO_HAS_URING=1)
endif()

if(BUILD_TESTING)
    add_test(NAME veloco.common COMMAND veloco_tests)
endif()

add_executable(veloco_bench_fiber benchmarks/bench_fiber.c)
target_include_directories(
    veloco_bench_fiber
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(veloco_bench_fiber PRIVATE veloco_build_options)
