cmake_minimum_required(VERSION 3.20)

project(qstream LANGUAGES CXX)
find_package(Threads REQUIRED)

add_library(qstream_runtime STATIC
    runtime/qstream_runtime.cpp
    runtime/qstream_threaded_runtime.cpp
)

target_link_libraries(qstream_runtime PUBLIC
    Threads::Threads
)

target_compile_features(qstream_runtime PUBLIC cxx_std_17)

target_compile_options(qstream_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(qstream_runtime PUBLIC
    runtime
    include/uapi
    protocol
)

add_executable(qstream_ctl
    runtime/qstream_ctl.cpp
)

target_link_libraries(qstream_ctl PRIVATE
    qstream_runtime
)

function(add_qstream_test target source)
    add_executable(${target} ${source})

    target_compile_features(${target} PRIVATE cxx_std_17)

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endfunction()

add_qstream_test(qstream_overflow_test
    tests/qstream_overflow.cpp
)

add_qstream_test(qstream_reset_test
    tests/qstream_reset.cpp
)

add_qstream_test(qstream_stats_test
    tests/qstream_stats.cpp
)

add_qstream_test(qstream_runtime_test
    tests/qstream_runtime.cpp
)

target_link_libraries(qstream_runtime_test PRIVATE
    qstream_runtime
)

add_qstream_test(qstream_concurrent_test
    tests/qstream_concurrent.cpp
)

target_link_libraries(qstream_concurrent_test PRIVATE
    qstream_runtime
)