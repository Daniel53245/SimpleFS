#!/bin/bash
dd if=/dev/zero of=mydisk.img bs=1024 count=20484
./mkfs.vvsfs mydisk.img
sudo mount -t vvsfs -o loop mydisk.img testdir
