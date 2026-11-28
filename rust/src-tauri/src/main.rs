#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::{
    path::Path,
    process::Command,
    sync::{
        atomic::{AtomicBool, Ordering},
        OnceLock,
    },
    thread,
    time::Duration,
};

mod drm;
mod ocr;
mod tts;

use serde::Serialize;
use tauri::{Emitter, Manager, State};
use tts::AppState;

const VOICE_LIST: &str = include_str!("../voice_list.tsv");

#[cfg(target_os = "windows")]
static MIDDLE_MOUSE_APP: OnceLock<tauri::AppHandle> = OnceLock::new();
static MIDDLE_MOUSE_ENABLED: AtomicBool = AtomicBool::new(true);

#[derive(Serialize)]
struct Voice {
    language: String,
    name: String,
    code: String,
}

#[tauri::command]
fn get_voices() -> Vec<Voice> {
    VOICE_LIST
        .lines()
        .filter_map(|line| {
            let mut fields = line.split('\t');
            Some(Voice {
                language: fields.next()?.to_string(),
                name: fields.next()?.to_string(),
                code: fields.next()?.to_string(),
            })
        })
        .collect()
}

#[tauri::command]
fn close_window(app: tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.close();
    }
}

#[tauri::command]
fn stop(state: State<'_, AppState>) {
    state.cancel_current();
}

#[tauri::command]
fn set_middle_mouse_enabled(enabled: bool) {
    MIDDLE_MOUSE_ENABLED.store(enabled, Ordering::Relaxed);
}

#[tauri::command]
async fn synthesize_stream(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    text: String,
    voice: String,
    job_id: u64,
) -> Result<(), String> {
    let _ = state.start_job(job_id);
    tts::synthesize_streaming(&text, &voice, job_id, &app).await
}

#[tauri::command]
async fn save_audio(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    text: String,
    voice: String,
    job_id: u64,
) -> Result<Option<String>, String> {
    let _ = state.start_job(job_id);

    let Some(file) = rfd::AsyncFileDialog::new()
        .add_filter("MP3 audio", &["mp3"])
        .set_file_name("speech.mp3")
        .save_file()
        .await
    else {
        return Ok(None);
    };

    let mut path = file.path().to_path_buf();
    if path.extension().is_none() {
        path.set_extension("mp3");
    }

    let audio = tts::synthesize(&text, &voice, job_id, &state, Some(&app)).await?;
    tokio::fs::write(&path, audio)
        .await
        .map_err(|err| format!("保存失败: {err}"))?;
    open_parent(&path);
    Ok(Some(path.display().to_string()))
}

fn open_parent(path: &Path) {
    if let Some(parent) = path.parent() {
        let _ = Command::new("explorer").arg(parent).spawn();
    }
}

#[cfg(target_os = "windows")]
fn simulate_ctrl_c() {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        keybd_event, KEYBD_EVENT_FLAGS, KEYEVENTF_KEYUP, VIRTUAL_KEY, VK_CONTROL,
    };
    const VK_C: VIRTUAL_KEY = VIRTUAL_KEY(0x43);

    unsafe {
        keybd_event(VK_CONTROL.0 as u8, 0, KEYBD_EVENT_FLAGS(0), 0);
        keybd_event(VK_C.0 as u8, 0, KEYBD_EVENT_FLAGS(0), 0);
        keybd_event(VK_C.0 as u8, 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_CONTROL.0 as u8, 0, KEYEVENTF_KEYUP, 0);
    }
}

#[cfg(not(target_os = "windows"))]
fn simulate_ctrl_c() {}

fn clean_readout_text(raw: &str) -> String {
    raw.replace("\r\n", "\n")
        .replace('\r', "\n")
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .collect::<Vec<_>>()
        .join("\n")
}

fn read_selected_text(app: tauri::AppHandle) {
    thread::spawn(move || {
        let image_before = arboard::Clipboard::new()
            .ok()
            .and_then(|mut c| c.get_image().ok());

        simulate_ctrl_c();
        thread::sleep(Duration::from_millis(180));

        let text = arboard::Clipboard::new()
            .ok()
            .and_then(|mut c| c.get_text().ok())
            .map(|raw| clean_readout_text(&raw))
            .unwrap_or_default();

        if !text.is_empty() {
            let _ = app.emit("hotkey-text", text);
            return;
        }

        if let Some(img) = image_before {
            match ocr::perform_ocr(&img.bytes, img.width as u32, img.height as u32) {
                Ok(ocr_text) => {
                    let cleaned = clean_readout_text(&ocr_text);
                    if !cleaned.is_empty() {
                        let _ = app.emit("hotkey-text", cleaned);
                    }
                }
                Err(e) => eprintln!("OCR failed: {e}"),
            }
        }
    });
}

#[cfg(target_os = "windows")]
unsafe extern "system" fn middle_mouse_proc(
    n_code: i32,
    w_param: windows::Win32::Foundation::WPARAM,
    l_param: windows::Win32::Foundation::LPARAM,
) -> windows::Win32::Foundation::LRESULT {
    use windows::Win32::UI::WindowsAndMessaging::{CallNextHookEx, WM_MBUTTONDOWN};

    if n_code >= 0
        && w_param.0 == WM_MBUTTONDOWN as usize
        && MIDDLE_MOUSE_ENABLED.load(Ordering::Relaxed)
    {
        if let Some(app) = MIDDLE_MOUSE_APP.get() {
            read_selected_text(app.clone());
        }
    }

    unsafe { CallNextHookEx(None, n_code, w_param, l_param) }
}

#[cfg(target_os = "windows")]
fn register_middle_mouse(app: tauri::AppHandle) -> windows::core::Result<()> {
    use windows::Win32::UI::WindowsAndMessaging::{SetWindowsHookExW, WH_MOUSE_LL};

    let _ = MIDDLE_MOUSE_APP.set(app);
    // ponytail: process-lifetime hook; Windows removes it on exit.
    unsafe { SetWindowsHookExW(WH_MOUSE_LL, Some(middle_mouse_proc), None, 0) }?;
    Ok(())
}

fn main() {
    use tauri_plugin_global_shortcut::{Code, GlobalShortcutExt, Shortcut, ShortcutState};

    tauri::Builder::default()
        .manage(AppState::default())
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.show();
                let _ = window.set_focus();
            }
        }))
        .plugin(
            tauri_plugin_global_shortcut::Builder::new()
                .with_handler(|app, _shortcut, event| {
                    if event.state == ShortcutState::Pressed {
                        read_selected_text(app.clone());
                    }
                })
                .build(),
        )
        .setup(|app| {
            let shortcut = Shortcut::new(None, Code::F9);
            if let Err(error) = app.global_shortcut().register(shortcut) {
                eprintln!("failed to register F9: {error}");
            }
            if let Err(error) = register_middle_mouse(app.handle().clone()) {
                eprintln!("failed to register middle mouse button: {error}");
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            get_voices,
            synthesize_stream,
            save_audio,
            stop,
            close_window,
            set_middle_mouse_enabled
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn middle_mouse_toggle_updates_flag() {
        set_middle_mouse_enabled(false);
        assert!(!MIDDLE_MOUSE_ENABLED.load(Ordering::Relaxed));
        set_middle_mouse_enabled(true);
        assert!(MIDDLE_MOUSE_ENABLED.load(Ordering::Relaxed));
    }

    #[test]
    fn clean_readout_text_preserves_line_breaks_without_fusing_words() {
        assert_eq!(clean_readout_text("  Hello \r\n\n world  "), "Hello\nworld");
        assert_eq!(clean_readout_text("第一行\n\r\n第二行"), "第一行\n第二行");
    }
}
