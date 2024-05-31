#!/bin/bash
cd testdir
touch a
mkdir 1
ls
mv a 1
cd 1
ls
cd ..
rm -r 1
ls -ila
