#!/bin/sh

# enable linux emulation

kldload linux64
sysctl compat.linux.osrelease=4.3.0
