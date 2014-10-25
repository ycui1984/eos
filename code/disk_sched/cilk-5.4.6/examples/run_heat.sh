#!/bin/bash

sync
echo 3 > /proc/sys/vm/drop_caches

echo enable > /proc/fast_auto

time ./heat --nproc 256

echo disable > /proc/fast_auto
