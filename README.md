1.0 OVERVIEW

this repo contains source code and doc of the contest software
doc/ directory contains all the description of design and
implementation. Please read it to understand this software project.

code/ directory contains all source codes. Basically,
we modify four parts in the Linux kernel 3.8.8 to demonstrate our
idea. Specifically, disk scheduling, CPU scheduling, synchronization
and page replacement. There are three subdirs under code.
disk\_sched/ dir is the kernel code which finishes disk scheduling and 
cpu scheduling, syn/ is the code which finishes synchronization and 
pagecache/ is the code which finishes page cache replacement.
We also upload clean linux kernel(without modifications). 
One can see the modifications we made by creating patches.
For example, if one want to see the code to implement the synchronization
part, run command ``diff -ruNa <path to clean kernel> <path to syn kernel>``

2.0 INSTALLATION INSTRUCTIONS
this project modifies Linux kernel to improve its performance.
So, at least, this project requires a Linux machine. 
To test a particular part, we need to compile the kernel first.
The following example tests the efficiency of CPU scheduling.
One should perform similar steps to test other parts.

2.1 cd code/disk\_sched  

2.2 mv SPEC /etc/

2.3 cp /etc/SPEC/scheduler\_spec 	/etc/SPEC/spec

2.4 install the new kernel

2.5 cd EOS-contest/code/disk\_sched/linux-3.8.8/modules/automod/

2.6 make 

2.7 insmod auto.ko

2.8 echo enable > /proc/fast\_auto

2.9 run CPU scheduling intensive benchmarks and feel speed improvement

2.10 echo disable > /proc/fast\_auto

3.0 run test presented in video

3.1 cd code/disk\_sched/cilk-5.4.6/

3.2 ./configure CFLAGS="-D\_XOPEN\_SOURCE=600 -D\_POSIX\_C\_SOURCE=200809L"

3.3 make

3.4 make install

3.5 cd code/disk\_sched/cilk-5.4.6/examples

3.6 run ``bash clean.sh`` on system without modification, clean system

3.7 run ``bash batch.sh`` on system with CPU scheduling patch, you can compare the 
performance difference. Basically, on clean system, it needs more than 4 mins to finish
, on system with our modifications, it needs about 40 seconds to complete
