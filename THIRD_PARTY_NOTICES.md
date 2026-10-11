# Third-party notices

MinimaDock's project code and original geometric launcher artwork are MIT licensed; see `LICENSE`. That license does not replace the licenses of dependencies, Windows components or third-party app icons extracted on a user's machine.

The current portable x64 build uses LLVM/MinGW 20261006 and statically links runtime support. The following unmodified notices from that toolchain are retained for binary distributions:

- LLVM runtime components: [Apache License 2.0 with LLVM Exceptions and bundled notices](third_party/licenses/LLVM.txt).
- MinGW-w64 runtime: [runtime copyright and license notices](third_party/licenses/MinGW-w64-runtime.txt).
- MinGW-w64 winpthreads: [winpthreads notices](third_party/licenses/winpthreads.txt).

`tools/Package.ps1` copies these files and the project license into `out`. Include them with any compiled release. A different toolchain or additional dependencies requires reviewing its own distribution notices; this list is specific to the current portable build.

The downloaded compiler and local development tools are excluded from Git. The new logo generator uses Python's standard library and imports no image. The optional legacy `Trace-LauncherLogo.py` tool requires Pillow but is not used to produce the current artwork; Pillow is not bundled in the application. Windows APIs and fonts are provided by the operating system. App icons are resolved at runtime and are not bundled project artwork.

The former Google Images sword reference and traced design are not included in current project assets or source. Historical local captures/builds/deployments are private and excluded from publication.
