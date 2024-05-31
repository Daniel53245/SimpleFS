#!/bin/bash
cd testdir
touch a
touch b
ls -ila
sudo chown root a
ls -ila
sudo rm a
sudo rm b
