cmake_minimum_required(VERSION 3.0)

project(test CXX)
set(CMAKE_CXX_STANDARD 11)
set(OPTION "-fPIC -pthread")

set(TARGET test)
file(GLOB TEST_FILE "*test*.*")

set(SCADUP_SRC ${CMAKE_SOURCE_DIR}/src)
include_directories(${SCADUP_SRC})
add_subdirectory(${SCADUP_SRC}/Scadup build)
find_library(SCADUP_LIB NAMES scadup PATHS build ${CMAKE_SOURCE_DIR}/lib NOTFOUND)

#[[
macro(SUBDIRLIST result curdir)
    file(GLOB children RELATIVE ${curdir} ${curdir}/*)
    set(sublist "")
    foreach(child ${children})
        if(IS_DIRECTORY ${curdir}/${child})
            LIST(APPEND sublist ${child})
        endif()
    endforeach()
    set(${result} ${sublist})
endmacro()
SUBDIRLIST(SUBDIRS ${CMAKE_SOURCE_DIR})
#]]

macro(GET_SRC_FILE result subdirs)
foreach(DIR ${subdirs})
    file(GLOB SRC_FILES ${SCADUP_SRC}/${DIR}/*.c*)
    foreach(SRC ${SRC_FILES})
        list(APPEND srcfiles ${SRC})
    endforeach()
endforeach()
set(${result} ${srcfiles})
endmacro()

set(SUBDIRS Utils)
if(NOT EXISTS ${SCADUP_LIB})
    list(APPEND SUBDIRS Scadup)
endif()

GET_SRC_FILE(SRC_FILE "${SUBDIRS}")
add_executable(${TARGET} ${SRC_FILE} ${TEST_FILE})

if(EXISTS ${SCADUP_LIB})
    add_dependencies(${TARGET} scadup)
    target_link_libraries(${TARGET} ${SCADUP_LIB})
endif()

target_link_libraries(${TARGET} ${OPTION})
