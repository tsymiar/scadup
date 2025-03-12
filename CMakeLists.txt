cmake_minimum_required(VERSION 2.8...3.16)

project(main CXX)

if("${ANDROID}" EQUAL "1")
    add_subdirectory(src)
else()
    add_subdirectory(test)
endif()
