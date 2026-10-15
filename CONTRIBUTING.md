# Contributing to MinimaDock

Thanks for helping! MinimaDock replaces the Windows taskbar, so a broken build can leave someone without one. The process below keeps every change safe to try and easy to review.

## How changes flow

1. **Fork** the repository and create a branch for one change (`feature/hide-on-idle`, `fix/tray-flicker`).
2. **Open an issue first** for anything bigger than a small fix, so the idea can be discussed before you build it.
3. **Open a pull request** against `main`. GitHub Actions builds it and runs the tests automatically.
4. A maintainer reviews it, tries it on their PC and merges it.
5. **Releases are published by the maintainer only.** Everyone who installs MinimaDock updates from these releases, so `main` and the releases must stay trustworthy.

## Build

MinimaDock is native C++20 (Win32, Direct2D, DirectWrite, DirectComposition) built with the portable [llvm-mingw 20261006](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006) toolchain. Extract `llvm-mingw-20261006-ucrt-x86_64.zip` into `.tools\` (verify its SHA-256 from [docs/development.md](docs/development.md)), then:

```powershell
.\tools\Build-Portable.ps1
```

That builds everything into `build\portable\` and runs the tests. Desktop UI tests need an interactive session; `-SkipDesktopTests` runs only the model tests (as CI does).

Builds are **reproducible**: the same source and toolchain give byte-for-byte identical binaries, and CI prints their SHA-256 hashes.

## Try your build safely

Never replace your taskbar with an untested build without a way back.

- **Framed preview:** run `build\portable\MinimaDock.exe` on its own. It opens in a normal window and changes nothing.
- **Snapshots:** `MinimaDock.exe --snapshot-customize out.png 1` and the other `--snapshot-*` modes in [docs/development.md](docs/development.md) render UI to a PNG. Some launcher snapshots contain your real file names; don't post those publicly.
- **Timed trial:** stage your build with `tools\Package.ps1` (copies it to `out\`), then `tools\Taskbar-Test.ps1 -Action Deploy` runs it as your taskbar for ten minutes and then reverts automatically. **Ctrl+Alt+Backspace** reverts at any time.
- **Install:** `out\Install-MinimaDock.cmd` installs what is staged in `out\`; Uninstall puts the Windows taskbar back.

### Smart App Control

MinimaDock is not code-signed. With Smart App Control on, Windows may refuse to run a particular build. The installer and trial check this first and change nothing if the build is blocked. Please don't try to work around a block (renaming files, adding junk to change the hash, and so on), and never commit changes that weaken Windows security. Mention a block in your PR instead.

## Code style

- Match the surrounding code: compact C++ in the existing style, short comments that explain *why*, no new dependencies or runtimes.
- Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`).
- Keep work off the UI thread (network, files, shell queries) and keep idle CPU near zero: no timers or animation frames when nothing is moving.
- Respect reduced motion and the user's light/dark theme.
- Anything that touches recovery, the work area or taskbar visibility (`src/recovery/`, the supervisor) needs extra care and a note in the PR on how you tested it.
- Privacy: nothing typed in the launcher leaves the PC. New network access needs a clear reason and a setting.

## Pull request checklist

- Builds with `tools\Build-Portable.ps1` and the tests pass.
- Tried in the framed preview or a timed trial (say which).
- Screenshots or a short clip for visual changes.
- Docs updated when behaviour or settings change (README, `docs/`).

## Reporting bugs and ideas

Use the issue templates. For bugs, include your Windows version, display setup (scaling, monitors) and what you expected to happen.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
