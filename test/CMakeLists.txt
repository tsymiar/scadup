cmake_minimum_required(VERSION 2.8...3.16)

project(test C CXX)

if (NOT WIN32)
    set(OPTION "-fPIC -g -ggdb")
    if (NOT "${ANDROID}" EQUAL "1")
        set(EXTERN pthread)
    endif()
else()
    set(EXTERN WS2_32)
endif()
add_definitions(${OPTION})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(TARGET test)
file(GLOB TEST_FILE "*test*.*")

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
    add_dependencies(${TARGET} scadup)
    target_link_libraries(${TARGET} ${SCADUP_LIB})
endif()

target_link_libraries(${TARGET} ${EXTERN})

file(COPY scadup.cfg DESTINATION ${CMAKE_BINARY_DIR}/test) 
