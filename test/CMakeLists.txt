cmake_minimum_required(VERSION 3.0)

project(test CXX)
set(CMAKE_CXX_STANDARD 11)
set(OPTION "-fPIC -pthread")

set(TARGET test)
file(GLOB TEST_FILE "*test*.*")

set(SCADUP_DIR ${CMAKE_SOURCE_DIR}/src/Scadup)
include_directories(${SCADUP_DIR})
add_subdirectory(${SCADUP_DIR} build)
find_library(SCADUP_LIB NAMES scadup PATHS build ${CMAKE_SOURCE_DIR}/lib NOTFOUND)

if(EXISTS ${SCADUP_LIB})
    add_executable(${TARGET} ${SCADUP_LIB} ${TEST_FILE})
else()
    file(GLOB SRC_FILE "${SCADUP_DIR}/*.c*")
    add_executable(${TARGET} ${SRC_FILE} ${TEST_FILE})
endif()

target_link_libraries(${TARGET} ${OPTION})
