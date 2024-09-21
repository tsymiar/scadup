#!/bin/bash
if [ -d build ]; then
    rm -rvf build
fi
mkdir build; cd build
cmake ..
make -j8
cd -
