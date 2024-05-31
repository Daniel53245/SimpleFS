#!/bin/bash

cp vvsfs.c testdir
cd testdir
ls -ail
df
rm vvsfs.c
df
cd ..
