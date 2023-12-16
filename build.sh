#!/bin/bash
if [ -d build ]; then
    rm -rvf build
    clear
fi
mkdir build; cd build
cmake ..
make -j8
cd -
