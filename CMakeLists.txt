cmake_minimum_required(VERSION 3.0)

project(Scadup_main CXX)

if("${ANDROID}" EQUAL "1")
    add_subdirectory(src/Scadup)
else()
    add_subdirectory(test)
endif()
