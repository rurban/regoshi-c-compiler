#!/bin/sh
make -s CC=aarch64-linux-gnu-gcc && ./arm64-cross.sh "$@" && timeout 5s qemu-aarch64 -L /usr/aarch64-redhat-linux/sys-root/fc43 ./a.arm64
