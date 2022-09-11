#!/usr/bin/env bash
# Install dependencies required by the prime_server Docker builder image.

set -x -o errexit -o pipefail -o nounset

readonly primeserver_version=0.7.0
readonly primeserver_commit_hash=bfd15349fc5ddb578b78e266ba9012b51a2086b3

# Adds the primeserver deps
apt-get update --assume-yes
apt-get install --yes \
    autoconf \
    automake \
    pkg-config \
    libtool \
    make \
    gcc \
    git \
    g++ \
    lcov \
    wget \
    libcurl4-openssl-dev \
    libzmq3-dev \
    libczmq-dev

mkdir /prime_server
cd prime_server

git clone --recursive https://github.com/kevinkreiser/prime_server
cd prime_server
git checkout ${primeserver_commit_hash}
