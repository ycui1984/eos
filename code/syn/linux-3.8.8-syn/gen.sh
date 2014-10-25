#!/bin/bash

make -j 16 && make modules -j 8 && make modules_install -j 8 && make install
