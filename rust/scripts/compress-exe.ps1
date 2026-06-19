$ErrorActionPreference = "Stop"

$exe = Resolve-Path (Join-Path $PSScriptRoot "..\src-tauri\target\release\edge-tts-gui-rust.exe")
if (-not (Get-Command upx -ErrorAction SilentlyContinue)) {
    throw "UPX not found. Install UPX or put upx.exe on PATH."
}

$before = (Get-Item $exe).Length
upx --best --lzma --force $exe
$after = (Get-Item $exe).Length

"Compressed {0:N2} MB -> {1:N2} MB" -f ($before / 1MB), ($after / 1MB)
