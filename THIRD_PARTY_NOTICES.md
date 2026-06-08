# Third-Party Notices

This file is the public notice file for dependencies bundled into Locking Glass release artifacts. It intentionally lists bundled runtime dependencies, not every build-only tool used during development.

Locking Glass currently bundles the following third-party runtime components in its Windows release packages.

## VirtualDesktopAccessor.dll

- Purpose: live Windows virtual desktop notifications and top-level window moves
- Project: `VirtualDesktopAccessor`
- Upstream repository: <https://github.com/Ciantic/VirtualDesktopAccessor>
- Upstream releases: <https://github.com/Ciantic/VirtualDesktopAccessor/releases>
- Release currently downloaded by `scripts/stage-windows-install.ps1`: `2024-12-16-windows11`
- License: `MIT`
- Copyright: `Jari Pennanen, 2015-2024`

Locking Glass copies this notice into staged and installed Windows packages as `THIRD_PARTY_NOTICES.txt`.

## Microsoft .NET Runtime Components

- Purpose: self-contained Windows helper executables for the live desktop probe and setup bootstrapper
- Project: `.NET`
- Upstream project: <https://github.com/dotnet/runtime>
- License: Microsoft .NET Library License and bundled third-party component notices
- Package notice files:
  - `DOTNET_RUNTIME_LICENSE.txt`
  - `DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt`

The release staging script copies these files from the `dotnet.exe` installation used to publish the self-contained helper executables. They are required package payload files so the bundled .NET runtime notices ship with the installer payload.

MIT License

Copyright (c) 2015-2024 Jari Pennanen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
