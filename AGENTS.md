# Repository Guidelines

## Project Structure & Module Organization
- `src/`: Qt (C++17) application source.
  - `edge-tts-gui.pro`: qmake project entrypoint.
  - `main.cpp`: app startup + global hotkey/mouse hooks (Windows).
  - `dialog.*` / `dialog.ui`: main UI and interactions.
  - `communicate.*`: Edge TTS WebSocket client + audio playback.
  - `resources.qrc`, `icon.png`, `favicon.ico`: bundled app assets.
  - `voice_list.tsv`: voice metadata used by the UI.
- Root: `README.md`, `LICENSE`, and screenshots (`cover.jpg`, `button.jpg`).

## Build, Test, and Development Commands
Prereqs: Qt 6.x with `Widgets`, `WebSockets`, `Multimedia`, and `Network`, plus a Windows kit (MinGW or MSVC).

- Qt Creator (recommended): open `src/edge-tts-gui.pro` and Build/Run.
- Command line (example):
  - `cd src`
  - `qmake edge-tts-gui.pro`
  - `mingw32-make -j` (MinGW) or `nmake` (MSVC)
- Packaging (Windows): `windeployqt path\to\edge-tts-gui.exe` to stage required Qt DLLs.

## Coding Style & Naming Conventions
- Use 4-space indentation and follow the surrounding file’s brace/style conventions.
- Keep UI logic in `Dialog` and network/audio logic in `Communicate` (avoid mixing concerns).
- Naming patterns used in this repo: classes `PascalCase`, methods `lowerCamelCase`, members `m_...`, constants `k...`/`ms_...`.

## Testing Guidelines
There is no automated test suite in the repo today.

- For behavior changes, include manual verification steps in the PR (e.g., “Play”, “Stop”, global `F9` readout, voice selection).
- Prefer adding small, non-UI helper functions when possible so logic can be unit-tested later (Qt Test).

## Commit & Pull Request Guidelines
- Commit history is mostly short, imperative subjects (often “add/fix/update”, sometimes Chinese). Keep messages concise and scoped.
- PRs should include: what changed, why, how to test, and screenshots/GIFs for UI changes; link related issues if applicable.

## Security & Configuration Tips
- Don’t commit build outputs (`src/build/`, `build/`, `dist/`) or Qt Creator user files (`*.pro.user`).
- Avoid hardcoding secrets/tokens; treat network failures as expected and handle them gracefully.
