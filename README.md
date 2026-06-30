# System Programming Mallocator Project

This repository contains a custom dynamic memory allocator implemented in C for a system programming course project. The allocator was designed to balance memory utilization and throughput while handling repeated small allocations efficiently.

## Overview

- Implemented `mm_malloc`, `mm_free`, and `mm_realloc`
- Built a segregated explicit free list allocator
- Added immediate coalescing for adjacent free blocks
- Used best-fit style search within size classes
- Added a small-object pool optimization for frequent small allocations
- Optimized `realloc` to grow in place when possible

## Project Structure

- `mm.c`: allocator implementation
- `mm.h`: allocator interface and team metadata struct
- `mdriver.c`: driver used to validate correctness and measure performance
- `memlib.*`, `fsecs.*`, `fcyc.*`, `clock.*`, `ftimer.*`: support code provided for the lab environment
- `tracefiles/`: benchmark traces used by the driver

## Implementation Highlights

The allocator organizes free blocks with segregated free lists so allocation requests can search a narrower range of candidate blocks. Free blocks are coalesced immediately to reduce fragmentation, and block splitting is used when a larger free block can satisfy a smaller request.

For workloads with many repeated small allocations, the allocator keeps a dedicated small-object pool to reduce search overhead and improve throughput. The `realloc` path also attempts in-place growth before falling back to allocate-and-copy behavior.

## Reported Results

According to the submitted project report:

- All benchmark traces passed correctness checks
- Space utilization: `98%`
- Throughput: `10,630 Kops`
- Performance index: `99/100`

## Build

This project was originally developed for a 32-bit lab environment.

```bash
make
./mdriver
```

Depending on the local toolchain, `-m32` support may be required to build successfully.

## Note

The original submission report is stored separately from this public portfolio repository to avoid exposing personal academic document details while still preserving the implementation itself.
