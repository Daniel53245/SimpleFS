#!/bin/bash
cd testdir
touch 1 2 3 4 5 6 7 8 9 0
ls -ila
mkdir a
cd a
touch 1 2 3 4 5 6 7 8 9 0
cd ..
ls -ila
rm -r a
rm ./*
ls -ila
