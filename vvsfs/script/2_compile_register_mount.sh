#!/bin/bash
# Compile vvsfs.c and register vvsfs.ko
# mount testdir
make # compile
sudo insmod vvsfs.ko # register
./mkfs.vvsfs mydisk.img # format
sudo mount -t vvsfs -o loop mydisk.img testdir # mount
