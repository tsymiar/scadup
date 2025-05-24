#!/bin/bash
if [ "$1" == "clean" ]; then
    rm -rvf .vs* build jniLibs target lib out
#    export TERM=xterm
#    clear
else
if [ ! -d build ]; then
    mkdir build
fi
cd build
cmake ..
make -j8
cd -
fi
