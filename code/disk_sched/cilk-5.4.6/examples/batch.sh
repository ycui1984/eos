#!/bin/bash

sync
echo 3 > /proc/sys/vm/drop_caches
echo "enable" > /proc/fast_auto
time ./matmul --nproc 512 1000  
echo "disable" > /proc/fast_auto
