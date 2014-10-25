#!/bin/bash

sync
echo 3 > /proc/sys/vm/drop_caches

echo enable > /proc/fast_auto

time ./fft --nproc 1024 --yield

echo disable > /proc/fast_auto
