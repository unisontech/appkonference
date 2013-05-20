#!/bin/sh

cd konference

ASTERISK_SRC_DIR=$1
ASTERISK_MODULES_DIR=$2

make ASTERISK_SRC_DIR=$ASTERISK_SRC_DIR ASTERISK_SRC_VERSION=1100 &&
make install INSTALL_MODULES_DIR=$ASTERISK_MODULES_DIR ASTERISK_SRC_DIR=$ASTERISK_SRC_DIR ASTERISK_SRC_VERSION=1100

cd ..
