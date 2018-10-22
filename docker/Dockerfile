# Dockerfile for building urdma
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

# Examples:
#  $ docker build -t urdma-builder
#  $ docker build --build-arg RDMA_CORE_VERSION=18.2 -t urdma-builder-rdma-core-18

FROM ubuntu:bionic
ARG RDMA_CORE_VERSION
ENV RDMA_CORE_VERSION ${RDMA_CORE_VERSION:-18.2}
RUN apt-get update && apt-get install -y \
    autoconf \
    automake \
    build-essential \
    cmake \
    dpdk-dev \
    git \
    libjson-c-dev \
    libnl-3-dev \
    libnl-route-3-dev \
    libsystemd-dev \
    libtool \
    libudev-dev \
    linux-headers-generic \
    ninja-build \
    pandoc \
    pkg-config \
    python \
    valgrind \
    uthash-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /usr/src
RUN git clone --single-branch -b v${RDMA_CORE_VERSION} \
        https://github.com/linux-rdma/rdma-core.git \
 && cd rdma-core && mkdir -p build && cd build \
 && cmake -GNinja -DCMAKE_INSTALL_PREFIX=/usr .. \
 && ninja && ninja install \
 && rm -rf /usr/src/rdma-core
COPY build.sh /
RUN mkdir -p /out
CMD /build.sh
