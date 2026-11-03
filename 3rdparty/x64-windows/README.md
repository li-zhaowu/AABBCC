# Vendored x64 Release dependencies

Headers, libraries, runtime DLLs, and licenses copied on 2026-11-03.
The build reads these files relative to the repository; the original
installation paths are not required.

| Package | Version | Original installation |
| --- | --- | --- |
| minizip-ng | 4.0.4 | OSGeo4W |
| zlib | 1.2.13 | vcpkg x64-windows |
| zstd | 1.5.4 | vcpkg x64-windows |
| libjpeg-turbo | 2.1.5.1 | vcpkg x64-windows |
| liblzma | 5.4.1 | vcpkg x64-windows |
| bzip2 | 1.0.8 | vcpkg x64-windows |
| ffnvcodec | 13.0.19.0 | FFmpeg nv-codec-headers, selected by the vcpkg baseline |
| libyuv | d98915a654d3564e4802a0004add46221c4e4348 | Upstream revision selected by the vcpkg baseline, built with VS 2022 as a static x64 Release library using /MD |

`FILES.json` records the original source path and SHA-256 for each copied file.
Source paths are provenance only and are never used by the build.
Licenses are in `share/<package>/copyright`.

OSGeo4W's minizip-ng headers are copied to `include/minizip-ng` to match the
upstream include paths used by this project. `lib/libminizip.lib` is the
minizip-ng library; the unrelated legacy `minizip.lib` is not included.

These binaries support x64 Release with the DLL C runtime (`/MD`). Debug and
other architectures are not provided here. libsodium is stored separately
under `../libsodium-1.0.22-stable`.

Run `./CompileRelease.ps1 -Rebuild` from the repository root for a full rebuild.
All intermediate files and build outputs are written to `Z:\AABBCC-build`.
