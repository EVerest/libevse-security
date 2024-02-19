# Use FetchContent to download and build GoogleTest
include(FetchContent)
FetchContent_Declare(
        gtest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1   
)
FetchContent_MakeAvailable(gtest)

set(TEST_TARGET_NAME ${PROJECT_NAME}_tests)
add_executable(${TEST_TARGET_NAME})

target_include_directories(${TEST_TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${TEST_TARGET_NAME} PRIVATE
    tests.cpp
)

target_link_libraries(${TEST_TARGET_NAME} PRIVATE
    evse_security
    gtest_main
    gmock_main
)

if(LIBEVSE_CRYPTO_SUPPLIER_OPENSSL)
    add_compile_definitions(LIBEVSE_CRYPTO_SUPPLIER_OPENSSL)
endif()

add_test(${TEST_TARGET_NAME} ${TEST_TARGET_NAME})

install(
    PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/generate_test_certs.sh"
    DESTINATION "${CMAKE_BINARY_DIR}/tests"
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/configs"
    DESTINATION "${CMAKE_BINARY_DIR}/tests"
    FILES_MATCHING PATTERN "*.cnf"
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/future_leaf"
    DESTINATION "${CMAKE_BINARY_DIR}/tests"
    FILES_MATCHING PATTERN "*"
)

install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/expired_leaf"
        DESTINATION "${CMAKE_BINARY_DIR}/tests"
        FILES_MATCHING PATTERN "*"
)
