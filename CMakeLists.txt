cmake_minimum_required(VERSION 3.22)

project(
    Veloco
    VERSION 0.1.0
    LANGUAGES C ASM
)

include(CTest)

option(VELOCO_ENABLE_URING "Build the io_uring backend" ON)
option(VELOCO_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)
option(VELOCO_MEMORY_DEBUG "Enable allocator canaries, poison, and double-free checks" OFF)

find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(VELOCO_FIBER_ASM src/fiber/fiber_x86_64.S)
    set(VELOCO_FIBER_TEST_ASM tests/test_fiber_x86_64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(VELOCO_FIBER_ASM src/fiber/fiber_aarch64.S)
    set(VELOCO_FIBER_TEST_ASM tests/test_fiber_aarch64.S)
else()
    message(FATAL_ERROR "Unsupported fiber architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

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

set(VELOCO_NET_SOURCES
    src/net/backend.c
    src/net/epoll_backend.c
    src/net/socket.c
)
if(VELOCO_ENABLE_URING)
    list(APPEND VELOCO_NET_SOURCES
        src/net/uring_backend.c
        src/net/io_worker.c
    )
endif()

add_library(
    veloco
    STATIC
        src/fiber/fiber.c
        ${VELOCO_FIBER_ASM}
        src/runtime/task.c
        src/runtime/scheduler.c
        src/runtime/run_queue.c
        src/runtime/context.c
        src/runtime/worker.c
        src/sync/task_mutex.c
        src/sync/semaphore.c
        src/sync/wait_group.c
        src/sync/channel.c
        ${VELOCO_NET_SOURCES}
        src/memory/arena.c
        src/memory/debug_allocator.c
        src/memory/object_pool.c
        src/memory/page_heap.c
        src/memory/size_class.c
        src/memory/span.c
        src/memory/valloc.c
)
target_include_directories(
    veloco
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src/memory"
)
target_link_libraries(veloco PUBLIC veloco_build_options Threads::Threads)
if(VELOCO_ENABLE_URING)
    target_link_libraries(veloco PRIVATE "${VELOCO_URING_TARGET}")
    target_compile_definitions(veloco PRIVATE VELOCO_HAS_URING=1)
endif()
if(VELOCO_MEMORY_DEBUG)
    target_compile_definitions(veloco PUBLIC VELOCO_MEMORY_DEBUG=1)
endif()

add_executable(
    veloco_tests
    tests/test_main.c
    tests/test_fiber.c
    tests/test_memory.c
    tests/test_task.c
    tests/test_queue.c
    tests/test_io.c
    tests/test_sync.c
    ${VELOCO_FIBER_TEST_ASM}
)
target_include_directories(
    veloco_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/memory"
        "${PROJECT_SOURCE_DIR}/src/runtime"
        "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(veloco_tests PRIVATE veloco veloco_build_options Threads::Threads)

if(VELOCO_ENABLE_URING)
    add_executable(veloco_uring_tests tests/test_uring.c)
    target_include_directories(
        veloco_uring_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/include"
    )
    target_link_libraries(
        veloco_uring_tests
        PRIVATE veloco veloco_build_options Threads::Threads
    )
endif()

if(BUILD_TESTING)
    add_test(NAME veloco.common COMMAND veloco_tests --group common)
    add_test(NAME veloco.fiber COMMAND veloco_tests --group fiber)
    add_test(NAME veloco.memory COMMAND veloco_tests --group memory)
    add_test(NAME veloco.task COMMAND veloco_tests --group task)
    add_test(NAME veloco.queue COMMAND veloco_tests --group queue)
    add_test(NAME veloco.io COMMAND veloco_tests --group io)
    add_test(NAME veloco.sync COMMAND veloco_tests --group sync)
    if(VELOCO_ENABLE_URING)
        add_test(NAME veloco.uring COMMAND veloco_uring_tests)
        set_tests_properties(veloco.uring PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(veloco_bench_fiber benchmarks/bench_fiber.c)
target_include_directories(
    veloco_bench_fiber
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(veloco_bench_fiber PRIVATE veloco veloco_build_options)

add_executable(bench_alloc benchmarks/bench_alloc.c)
target_include_directories(bench_alloc PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(bench_alloc PRIVATE veloco veloco_build_options)
