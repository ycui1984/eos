0. OVERVIEW

this repo contains source code and doc of the contest software
doc directory contains all the description of design and
implementation. Please read it to understand this software project.

code directory contains all source codes. Basically,
we modify four parts in the Linux kernel 3.8.8 to demonstrate our
idea. Specifically, disk scheduling, CPU scheduling, synchronization
and page replacement. There are three subdirs under code.
disk\_sched dir is the kernel code which finishes disk scheduling and 
cpu scheduling, syn is the code which finishes synchronization and 
pagecache is the code which finishes page cache replacement.

1. INSTALLATION INSTRUCTIONS
this project modifies Linux kernel to improve its performance.
So, at least, this project requires a Linux machine. 
To test a particular part, we need to compile the kernel first.
The following example tests the efficiency of CPU scheduling.
One should perform similar steps to test other parts.

1.1 go to directory code/disk\_sched

1.2 mv SPEC /etc/

1.3 cp /etc/SPEC/scheduler\_spec 	/etc/SPEC/spec

1.4 install the new kernel

1.5 go to EOS-contest/code/disk\_sched/linux-3.8.8/modules/automod/

1.6 make 

1.7 insmod auto.ko

1.8 echo enable > /proc/fast\_auto

1.9 run CPU scheduling intensive benchmarks and feel speed improvement

1.10 echo disable > /proc/fast\_auto

2. run test presented in video

2.1 go to code/disk\_sched/cilk-5.4.6/

2.2 ./configure CFLAGS="-D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L"

2.3 make

2.4 make install

2.5 go to code/disk\_sched/cilk-5.4.6/examples

2.6 run bash clean.sh on system without modification, clean system

2.7 run bash batch.sh on system with CPU scheduling patch, you can compare the 
performance difference. Basically, on clean system, it needs more than 4 mins to finish
, on system with our modifications, it needs about 40 seconds to complete
