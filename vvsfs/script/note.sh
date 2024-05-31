#!/bin/bash
# after insmod
MODPATH=$(realpath vvsfs.ko)
echo ${MODPATH}
BSS=$(cat /sys/module/vvsfs/sections/.bss)
DATA=$(cat /sys/module/vvsfs/sections/.data)
TEXT=$(cat /sys/module/vvsfs/sections/.text)
gdb add-symbol-file ${MODPATH} ${TEXT} -s .data ${DATA} -s .bss ${BSS}
