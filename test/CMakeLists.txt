cmake_minimum_required(VERSION 2.8...3.16)
project(test C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT WIN32)
    set(OPTION "-fPIC -g -ggdb")
    if (NOT "${ANDROID}" EQUAL "1")
        set(EXTERN pthread)
    endif()
else()
    set(EXTERN WS2_32)
    # MSVC rejects inet_ntoa()/localtime() as deprecated without these
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

add_definitions(${OPTION})
# CMAKE_CXX_STANDARD covers this: MSVC wants /std:c++11, GCC/Clang wants -std=c++11

file(GLOB TEST_FILE "*test*.*")
set(TARGET test)
include_directories(${CMAKE_SOURCE_DIR}/target/include)

if (NOT WIN32)
    add_executable(${TARGET} ${TEST_FILE})
    add_subdirectory(${CMAKE_SOURCE_DIR}/src build)
    target_link_libraries(${TARGET} scadup ${EXTERN})
else()
    set(SCADUP_SRC ${CMAKE_SOURCE_DIR}/src)
    include_directories(${SCADUP_SRC})
    add_subdirectory(${SCADUP_SRC} build)
    find_library(SCADUP_LIB NAMES scadup PATHS build ${CMAKE_SOURCE_DIR}/lib NOTFOUND)
    macro(GET_SUB_SRC_FILES subfiles parent)
        file(GLOB children RELATIVE ${parent} ${parent}/*)
        set(sublist)
        foreach(child ${children})
            if(IS_DIRECTORY ${parent}/${child})
                LIST(APPEND sublist ${child})
            endif()
        endforeach()
        foreach(DIR ${sublist})
            file(GLOB SRC_LIST ${SCADUP_SRC}/${DIR}/*.c*)
            foreach(SRC ${SRC_LIST})
                list(APPEND filelist ${SRC})
            endforeach()
        endforeach()
        set(${subfiles} ${filelist})
    endmacro()
    GET_SUB_SRC_FILES(SUB_FILES ${SCADUP_SRC})
    add_executable(${TARGET} ${SUB_FILES} ${TEST_FILE})
    if(EXISTS ${SCADUP_LIB})
        target_link_libraries(${TARGET} ${SCADUP_LIB})
    endif()
    target_link_libraries(${TARGET} ${EXTERN})
endif()

# The tests include the published headers from target/include, which the library writes
# during its POST_BUILD step, so the library has to finish first. target_link_libraries()
# implies that only when the built library is actually linked, and that is skipped here
# (this executable compiles the sources itself, and find_library() runs at configure time
# when the library does not exist yet). Without this the test can compile while
# target/include is still missing, hitting C1083 on a clean checkout.
add_dependencies(${TARGET} scadup)

file(COPY scadup.cfg DESTINATION ${CMAKE_BINARY_DIR}/test)
