# Linux environment for building and testing the allocator (the tests need
# ltrace, valgrind and the Linux brk()/mmap() syscalls).
FROM ubuntu:22.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gcc make libc6-dev python3 python-is-python3 ltrace valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /mem-alloc
