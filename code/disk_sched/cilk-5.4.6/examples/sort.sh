#!/bin/bash

for (( i=0; i<6; i++ ))
do
	./cilksort --nproc 256 >> sortlog
done
