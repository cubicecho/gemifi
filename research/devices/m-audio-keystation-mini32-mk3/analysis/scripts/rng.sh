#!/bin/sh
# usage: rng.sh start end   (hex, no 0x)
# DIS=path to a full listing made with: python -I dis.py aprom.bin c0 45ec > full-dis.txt
s=$(printf %08x 0x$1); e=$(printf %08x 0x$2)
awk -v s=$s -v e=$e '{a=substr($1,1,8)} a>=s && a<e' "${DIS:-full-dis.txt}"
