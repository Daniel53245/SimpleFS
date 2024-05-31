#!/bin/bash
make clean
rm mydisk.img
rmdir testdir

dd if=/dev/zero of=mydisk.img bs=1024 count=20484
mkdir testdir

make # compile
sudo insmod vvsfs.ko # register
./mkfs.vvsfs mydisk.img # format
sudo mount -t vvsfs -o loop mydisk.img testdir # mount
