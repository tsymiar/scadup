cmake_minimum_required(VERSION 3.0)

project(scadop CXX)

if("${ANDROID}" EQUAL "1")
    add_subdirectory(src/kaics)
else()
    add_subdirectory(test)
endif()
