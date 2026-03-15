# stratum-utils
A curated set of standalone applications built on top of [Stratum](https://github.com/BroBordd/stratum), a bare-metal OpenGL ES 2.0 UI framework for Android. These utilities serve as both practical tools and reference implementations for Stratum-based development.
# Integration
This repository is consumed as a submodule by both the Stratum and boot-menu repositories. Applications are compiled as standalone binaries and can be launched directly or surfaced through the boot-menu extras system.
# Requirements
- Android device with root access
- Stratum framework libraries (`libstratum.so`, `stub.so`)
- OpenGL ES 2.0 support
