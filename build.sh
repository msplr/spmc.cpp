#!/bin/bash

g++ -std=c++17 -O0 bug.cpp -o bug -lpthread
g++ -std=c++17 -O3 spmc.cpp -o spmc -lpthread
