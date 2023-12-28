# libevse-security

![Github Actions](https://github.com/EVerest/libevse-security/actions/workflows/build_and_test.yml/badge.svg)

This is a C++ library for security related operations for charging stations. It respects the requirements specified in OCPP and ISO15118 and can be used in combination with OCPP and ISO15118 implementations.

In the near future this library will also contain support for secure storage on TPM2.0.

All documentation and the issue tracking can be found in our main repository here: https://github.com/EVerest/everest

## Prerequisites

The library requires OpenSSL 1.1.1.

## Build Instructions

Clone this repository and build with CMake.

```bash
git clone git@github.com:EVerest/libevsesecurity.git
cd libevsesecurity
cmake -B build
ninja -j$(nproc) -C build install
```

## Tests

GTest is required for building the test cases target.
To build the target and run the tests use:

```bash
cmake -B build -DBUILD_TESTING_EVSE_SECURITY=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=./dist
ninja -j$(nproc) -C build install
ninja -j$(nproc) -C build test
```
