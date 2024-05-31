#!/bin/bash
# Create vvsfs.raw
dd if=/dev/zero of=mydisk.img bs=1024 count=20484
mkdir testdir
