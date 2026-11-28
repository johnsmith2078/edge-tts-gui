# Repository Guidelines

## Project Structure & Module Organization
- `rust/`: Tauri/Rust application.
  - `src/`: vanilla HTML/CSS/JS frontend (`main.js`, `style.css`).
  - `src-tauri/`: Rust backend (`src/main.rs`, `src/drm.rs`), Tauri config (`tauri.conf.json`), embedded voice list (`voice_list.tsv`), and icon.
- `RapidOCR/`: bundled OCR binary and ONNX models copied into `rust/src-tauri/rapidocr/` at build time by `rust/src-tauri/build.rs`.
- Root: `README.md`, `LICENSE`, and screenshot (`cover.jpg`).

## Build, Test, and Development Commands
- Prereqs: Rust stable, Node.js/npm, WebView2 runtime, and optional `upx.exe` on `PATH` for compression.
- Install deps: `cd rust && npm install`.
- Dev run: `cd rust && npm run tauri dev`.
- Tests: `cd rust/src-tauri && cargo test`.
- Optional live Edge TTS smoke test: `cd rust/src-tauri && cargo test live_synthesis_smoke -- --ignored --nocapture`.
- No-bundle release build: `cd rust && npm run build:no-bundle`.
- Final output: `rust/src-tauri/target/release/edge-tts-gui-rust.exe`.
- Tauri bundling stays disabled in `rust/src-tauri/tauri.conf.json` (`bundle.active=false`); do not switch to MSI/NSIS/AppImage bundling unless explicitly requested.

## Coding Style & Naming Conventions
- Follow standard Rust (`rustfmt`) and frontend JS/CSS conventions used in `rust/`.
- Keep UI logic in `rust/src/` and TTS/DRM/OCR/hotkey logic in `rust/src-tauri/src/`.

## Testing Guidelines
- Add unit tests alongside Rust modules (`drm.rs`, `main.rs`) and run `cargo test` before submitting.
- For UI or system-hook behavior changes, include manual verification steps (e.g., Play, Stop, Save, global `F9` / middle-mouse readout, voice selection).

## Commit & Pull Request Guidelines
- Commit history is mostly short, imperative subjects (often “add/fix/update”, sometimes Chinese). Keep messages concise and scoped.
- PRs should include: what changed, why, how to test, and screenshots/GIFs for UI changes; link related issues if applicable.

## Security & Configuration Tips
- Don’t commit build outputs (`dist/`, `rust/dist/`, `rust/src-tauri/target/`, `rust/src-tauri/rapidocr/`, `rust/node_modules/`).
- Avoid hardcoding secrets/tokens; treat network failures as expected and handle them gracefully.
