cmake_minimum_required(VERSION 2.8...3.16)

set(TARGET scadup)
if (NOT WIN32)
    set(OPTION "-fPIC -g -ggdb")
    if (NOT "${ANDROID}" EQUAL "1")
        set(EXTERN pthread)
    endif()
else()
    set(EXTERN WS2_32)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall")
#set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ftest-coverage -fprofile-arcs")
#add_definitions(-D_NONE_BLOCK)
add_definitions(${OPTION})
if (NOT WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated -Wno-format")
endif()

if("${ANDROID}" EQUAL "1")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/../../../../jniLibs/${ANDROID_ABI})
else()
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/../target/lib)
    add_definitions(-DFULLY_COMPILE)
    include_directories(..)
endif()

include_directories(.)
file(GLOB SRC_FILE_LIST "./**/*[^test]*.*[^.txt]*")
project(${TARGET} C CXX)
add_library(${TARGET} SHARED ${SRC_FILE_LIST})
#add_library(${TARGET} STATIC ${SRC_FILE_LIST})

if("${ANDROID}" EQUAL "1")
    target_link_libraries(${TARGET} log android)
else()
    file(GLOB TEST_FILE "*test*.*")
    if(EXISTS ${TEST_FILE})
        if(WIN32)
            set(PROC ${TARGET}_test)
        else()
            set(PROC ${TARGET}.exe)
        endif()
        add_executable(${PROC} ${SRC_FILE_LIST} ${TEST_FILE})
    endif()
endif()

target_link_libraries(${TARGET} ${EXTERN})
add_custom_command(TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E
    copy_if_different
    "${PROJECT_SOURCE_DIR}/common/Scadup.h"
    "${PROJECT_SOURCE_DIR}/../target/include/${TARGET}.h"
)
