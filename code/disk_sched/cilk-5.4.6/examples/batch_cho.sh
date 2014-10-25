#!/bin/bash

for (( i=0; i<5; i++ ))
do

	for prog in cholesky #fft matmul cilksort heat
	do
		sync
		echo 3 > /proc/sys/vm/drop_caches

		if [ $prog == "fft" ] 
		then
			(time ./$prog --nproc 1024 --yield) >> fft_log 2>&1  
		elif [ $prog == "matmul" ]
		then
			(time ./$prog --nproc 512 1000) >> matmul_log 2>&1 
		else
			(time ./$prog --nproc 512) >> ${prog}_log 2>&1
		fi

		sleep 180
	done
done
