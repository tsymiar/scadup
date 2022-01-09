cmake_minimum_required(VERSION 3.0)

project(test CXX)
set(CMAKE_CXX_STANDARD 11)

set(TARGET test)
file(GLOB TEST_FILE "*test*.*")

set(KAICS_DIR ${CMAKE_SOURCE_DIR}/src/kaics)
include_directories(${KAICS_DIR})
add_subdirectory(${KAICS_DIR} build)
find_library(KAICS_LIB NAMES kaics PATHS build ${CMAKE_SOURCE_DIR}/lib NOTFOUND)

if(EXISTS ${KAICS_LIB})
    add_executable(${TARGET} ${KAICS_LIB} ${TEST_FILE})
else()
    file(GLOB SRC_FILE "${KAICS_DIR}/*.c*")
    add_executable(${TARGET} ${SRC_FILE} ${TEST_FILE})
endif()
