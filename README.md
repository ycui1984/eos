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
The following example tests the efficiency of disk scheduling.
One should perform similar steps to test other parts.

1.1 go to directory code/disk\_sched
1.2 mv SPEC /etc/
1.3 cp /etc/SPEC/disk\_spec 	/etc/SPEC/spec
1.4 change <hard disk id> in /etc/SPEC/spec to the target disk id, e.g., sda
1.5 install the new kernel
1.6 go to EOS-contest/code/disk\_sched/linux-3.8.8/modules/automod/
1.7 make 
1.8 insmod auto.ko
1.9 echo enable > /proc/fast\_auto
1.10 run disk intensive benchmarks and feel speed improvement
1.11 echo disable > /proc/fast\_auto
