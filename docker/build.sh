#!/bin/sh
# Script for building urdma inside a docker container
#
# Authors: Patrick MacArthur <patrick@patrickmacarthur.net>
#
# Copyright (c) 2018 University of New Hampshire
#
# This software is available to you under a choice of one of two
# licenses.  You may choose to be licensed under the terms of the GNU
# General Public License (GPL) Version 2, available from the file
# COPYING in the main directory of this source tree, or the
# BSD license below:
#
#   Redistribution and use in source and binary forms, with or
#   without modification, are permitted provided that the following
#   conditions are met:
#
#   - Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#   - Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
#   - Neither the name of IBM nor the names of its contributors may be
#     used to endorse or promote products derived from this software without
#     specific prior written permission.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

if [ x$1 = x-h ] || [ x$1 = x--help ]; then
	printf "Usage: %s [[GITHUBUSER/]BRANCH]\n\n" "$0"
	printf "Example:\n\t%s patrickmacarthur/fix-rdma-core-18-build\n" "$0"
	exit 0
fi

ghbranch=${1:-zrlio/master}
fork=${ghbranch%%/*}
if [ "x$fork" = x ] || [ "x$fork" = "x$ghbranch" ]; then
	fork=zrlio
fi
branch=${ghbranch#*/}

set -eux
git clone -b "${branch}" https://github.com/${fork}/urdma.git
cd urdma
autoreconf --force --install
. /usr/share/dpdk/dpdk-sdk-env.sh
./configure --sysconfdir=/etc --prefix=/usr
make distcheck
mkdir -p /out
cp urdma-*.tar.gz /out
