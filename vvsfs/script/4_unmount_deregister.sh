#!/bin/bash
# deregister vvsfs, unmount
sudo umount testdir # unmoun the previous loop device
sudo rmmod vvsfs # deregister vvsfs module
