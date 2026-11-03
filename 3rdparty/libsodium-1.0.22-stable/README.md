# libsodium 1.0.22-stable

Official Windows MSVC binary distribution, downloaded on 2026-11-03.

- URL: https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-stable-msvc.zip
- Official archive listing timestamp: 2026-10-27.
- Archive SHA-256: `6e7e43fc73342d5b99101f53699d863310d48096a6bf47874cffefe8d41efdc6`
- License: ISC; see `LICENSE`.
- The upstream detached signature is included alongside this file.

`BuildOutput.targets` uses the headers in `libsodium/include` and the x64 Release
VS 2022 import library and DLL in `libsodium/x64/Release/v143/dynamic`.
The DLL is copied to `Z:\AABBCC-build\bin\x64\Release` after linking.
No Debug configuration is built by `CompileRelease.ps1`.

The official package includes additional architectures and configurations;
they are retained as supplied, but are not used by this build.
