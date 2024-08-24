#!/bin/bash

g++ -std=c++20 -O0 bug.cpp -o bug
g++ -std=c++20 -O3 spmc.cpp -o spmc
