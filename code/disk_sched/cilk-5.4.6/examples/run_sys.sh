#!/bin/bash

for (( i =0; i<5; i++ ))
do

	sync
	echo 3 > /proc/sys/vm/drop_caches

#echo enable > /proc/fast_auto

	sysbench --num-threads=64 --test=threads --thread-yields=250000 --thread-locks=16 run

#echo disable > /proc/fast_auto
done
