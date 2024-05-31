#!/bin/bash
# Recompile vvsfs.c, deregister vvsfs, unmount, register vvsfs, mount
make
sudo umount testdir # unmoun the previous loop device
sudo rmmod vvsfs # deregister vvsfs module
sudo insmod vvsfs.ko # regiser the new vvsfs module
./mkfs.vvsfs mydisk.img # format
sudo mount -t vvsfs -o loop mydisk.img testdir # mount
# cat /proc/filesystems
