#!/bin/bash

sync
echo 3 > /proc/sys/vm/drop_caches
time ./matmul --nproc 512 1000  
