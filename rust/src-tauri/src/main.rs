#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::{
    path::Path,
    process::Command,
    sync::{
        atomic::{AtomicU64, Ordering},
        Once,
    },
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use chrono::Utc;
use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use sha2::{Digest, Sha256};
use tauri::{Emitter, Manager, State};
use tokio_tungstenite::{
    connect_async,
    tungstenite::{
        client::IntoClientRequest,
        http::HeaderValue,
        Message,
    },
};
use uuid::Uuid;

const TRUSTED_CLIENT_TOKEN: &str = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const WSS_URL: &str = "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const CHROMIUM_FULL_VERSION: &str = "143.0.3650.75";
const CHROMIUM_MAJOR_VERSION: &str = "143";
const MAX_TEXT_BYTES: usize = 4096;
const INITIAL_TEXT_BYTES: usize = 80;
const TARGET_TEXT_BYTES: usize = 780;
const VOICE_LIST: &str = include_str!("../voice_list.tsv");
static TLS_INIT: Once = Once::new();

#[derive(Default)]
struct AppState {
    current_job: AtomicU64,
}

#[derive(Serialize)]
struct Voice {
    language: String,
    name: String,
    code: String,
}

#[derive(Clone, Serialize)]
struct ProgressPayload {
    job_id: u64,
    percent: u8,
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
fn stop(state: State<'_, AppState>) {
    state.current_job.fetch_add(1, Ordering::SeqCst);
}

#[tauri::command]
async fn synthesize_text(
    state: State<'_, AppState>,
    text: String,
    voice: String,
    job_id: u64,
) -> Result<Vec<u8>, String> {
    state.current_job.store(job_id, Ordering::SeqCst);
    synthesize(&text, &voice, job_id, &state, None).await
}

#[tauri::command]
async fn save_audio(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    text: String,
    voice: String,
    job_id: u64,
) -> Result<Option<String>, String> {
    state.current_job.store(job_id, Ordering::SeqCst);

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

    let audio = synthesize(&text, &voice, job_id, &state, Some(&app)).await?;
    tokio::fs::write(&path, audio)
        .await
        .map_err(|err| format!("保存失败: {err}"))?;
    open_parent(&path);
    Ok(Some(path.display().to_string()))
}

async fn synthesize(
    text: &str,
    voice: &str,
    job_id: u64,
    state: &AppState,
    app: Option<&tauri::AppHandle>,
) -> Result<Vec<u8>, String> {
    init_tls();
    ensure_current(state, job_id)?;

    let clean = escape_xml(&remove_incompatible_characters(text));
    let parts = if app.is_some() {
        split_text_by_byte_length(&clean, MAX_TEXT_BYTES)
    } else {
        split_text_for_playback(&clean, INITIAL_TEXT_BYTES, TARGET_TEXT_BYTES)
    };
    if parts.is_empty() {
        return Err("文本为空".into());
    }

    let url = format!(
        "{WSS_URL}&Sec-MS-GEC={}&Sec-MS-GEC-Version={}&ConnectionId={}",
        generate_sec_ms_gec_token()?,
        generate_sec_ms_gec_version(),
        connect_id()
    );
    let mut request = url.into_client_request().map_err(to_string)?;
    let headers = request.headers_mut();
    headers.insert("Pragma", HeaderValue::from_static("no-cache"));
    headers.insert("Cache-Control", HeaderValue::from_static("no-cache"));
    headers.insert(
        "Origin",
        HeaderValue::from_static("chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold"),
    );
    headers.insert("Accept-Encoding", HeaderValue::from_static("gzip, deflate, br, zstd"));
    headers.insert("Accept-Language", HeaderValue::from_static("en-US,en;q=0.9"));
    headers.insert("User-Agent", HeaderValue::from_str(&user_agent()).map_err(to_string)?);
    headers.insert("Cookie", HeaderValue::from_str(&format!("muid={};", generate_muid())).map_err(to_string)?);

    let (socket, _) = connect_async(request).await.map_err(|err| format!("连接失败: {err}"))?;
    let (mut write, mut read) = socket.split();

    let mut audio = Vec::with_capacity(text.len().max(1024) * 128);
    let mut index = 0usize;
    let mut downloading = false;
    send_text_part(&mut write, &parts[index], voice).await?;
    emit_progress(app, job_id, 1);

    while let Some(message) = read.next().await {
        ensure_current(state, job_id)?;
        match message.map_err(to_string)? {
            Message::Binary(message) => {
                if !downloading {
                    return Err("收到意外音频数据".into());
                }
                let data = parse_audio_message(&message)?;
                if !data.is_empty() {
                    audio.extend_from_slice(data);
                    emit_progress(app, job_id, half_progress(index, parts.len()));
                }
            }
            Message::Text(message) => {
                let headers = parse_headers(&message);
                match headers.get("Path").map(String::as_str) {
                    Some("turn.start") => downloading = true,
                    Some("turn.end") => {
                        downloading = false;
                        index += 1;
                        emit_progress(app, job_id, full_progress(index, parts.len()));
                        if index >= parts.len() {
                            break;
                        }
                        send_text_part(&mut write, &parts[index], voice).await?;
                    }
                    Some("audio.metadata" | "response") => {}
                    _ => return Err(format!("无法识别服务响应: {message}")),
                }
            }
            Message::Close(_) => break,
            _ => {}
        }
    }

    trim_trailing_zeros(&mut audio);
    if audio.is_empty() {
        return Err("未收到音频数据".into());
    }
    emit_progress(app, job_id, 100);
    Ok(audio)
}

async fn send_text_part<W>(write: &mut W, part: &str, voice: &str) -> Result<(), String>
where
    W: SinkExt<Message> + Unpin,
    W::Error: std::fmt::Display,
{
    let date = date_to_string();
    let config = concat!(
        "Content-Type:application/json; charset=utf-8\r\n",
        "Path:speech.config\r\n\r\n",
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{",
        "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"true\"},",
        "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n"
    );
    write
        .send(Message::Text(format!("X-Timestamp:{date}\r\n{config}").into()))
        .await
        .map_err(to_string)?;

    let ssml = mkssml(part, voice);
    let data = format!(
        "X-RequestId:{}\r\nContent-Type:application/ssml+xml\r\nX-Timestamp:{}Z\r\nPath:ssml\r\n\r\n{}",
        connect_id(),
        date,
        ssml
    );
    write.send(Message::Text(data.into())).await.map_err(to_string)
}

fn parse_audio_message(message: &[u8]) -> Result<&[u8], String> {
    if message.len() < 2 {
        return Err("音频消息缺少头长度".into());
    }
    let header_len = ((message[0] as usize) << 8) | message[1] as usize;
    if message.len() < header_len + 2 {
        return Err("音频消息缺少数据".into());
    }
    Ok(&message[header_len + 2..])
}

fn parse_headers(message: &str) -> std::collections::HashMap<String, String> {
    message
        .split_once("\r\n\r\n")
        .map(|(headers, _)| headers)
        .unwrap_or(message)
        .lines()
        .filter_map(|line| {
            let (key, value) = line.split_once(':')?;
            Some((key.trim().to_string(), value.trim().to_string()))
        })
        .collect()
}

fn mkssml(text: &str, voice: &str) -> String {
    format!(
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><voice name='Microsoft Server Speech Text to Speech Voice ({voice})'><prosody pitch='+0Hz' rate='+0%' volume='+0%'>{text}</prosody></voice></speak>"
    )
}

fn remove_incompatible_characters(text: &str) -> String {
    text.chars()
        .map(|ch| match ch as u32 {
            0..=8 | 11..=12 | 14..=31 => ' ',
            _ => ch,
        })
        .collect()
}

fn escape_xml(text: &str) -> String {
    text.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

fn split_text_for_playback(text: &str, initial_len: usize, next_len: usize) -> Vec<String> {
    let mut bytes = text.as_bytes();
    let mut parts = Vec::new();
    let safe_initial = initial_len.clamp(64, MAX_TEXT_BYTES);
    let safe_max = next_len.clamp(safe_initial, MAX_TEXT_BYTES);
    let mut limit = safe_initial;

    while !bytes.is_empty() {
        if bytes.len() <= limit {
            push_trimmed(&mut parts, bytes);
            break;
        }

        let mut split_at = find_last_punctuation(bytes, limit)
            .or_else(|| (safe_max > limit).then(|| find_first_punctuation(bytes, limit, safe_max)).flatten())
            .unwrap_or_else(|| safe_utf8_split(bytes, safe_max.min(bytes.len())));
        split_at = adjust_xml_entity(bytes, split_at);
        if split_at == 0 {
            split_at = safe_utf8_split(bytes, limit.min(bytes.len()));
        }

        push_trimmed(&mut parts, &bytes[..split_at]);
        bytes = &bytes[split_at..];
        let growth = (limit / 8).clamp(16, 40);
        limit = (limit + growth).min(safe_max);
    }

    parts
}

fn split_text_by_byte_length(text: &str, byte_len: usize) -> Vec<String> {
    let mut bytes = text.as_bytes();
    let mut parts = Vec::new();

    while bytes.len() > byte_len {
        let mut split_at = find_last_punctuation(bytes, byte_len)
            .unwrap_or_else(|| safe_utf8_split(bytes, byte_len.min(bytes.len())));
        split_at = adjust_xml_entity(bytes, split_at);
        if split_at == 0 {
            split_at = safe_utf8_split(bytes, byte_len.min(bytes.len()));
        }
        push_trimmed(&mut parts, &bytes[..split_at]);
        bytes = &bytes[split_at..];
    }
    push_trimmed(&mut parts, bytes);
    parts
}

fn push_trimmed(parts: &mut Vec<String>, bytes: &[u8]) {
    if let Ok(text) = std::str::from_utf8(bytes) {
        let text = text.trim();
        if !text.is_empty() {
            parts.push(text.to_string());
        }
    }
}

fn punctuation_marks() -> [&'static [u8]; 13] {
    [
        b"\n",
        b".",
        b"!",
        b"?",
        b",",
        "。".as_bytes(),
        "！".as_bytes(),
        "？".as_bytes(),
        "，".as_bytes(),
        "、".as_bytes(),
        "；".as_bytes(),
        "：".as_bytes(),
        "…".as_bytes(),
    ]
}

fn find_last_punctuation(text: &[u8], limit: usize) -> Option<usize> {
    let safe_limit = limit.min(text.len());
    punctuation_marks()
        .iter()
        .filter_map(|mark| rfind_subslice(&text[..safe_limit], mark).map(|index| index + mark.len()))
        .max()
}

fn find_first_punctuation(text: &[u8], start: usize, hard_limit: usize) -> Option<usize> {
    let safe_start = start.min(text.len());
    let safe_hard_limit = hard_limit.min(text.len());
    punctuation_marks()
        .iter()
        .filter_map(|mark| {
            find_subslice(&text[safe_start..safe_hard_limit], mark)
                .map(|index| safe_start + index + mark.len())
        })
        .min()
}

fn find_subslice(text: &[u8], needle: &[u8]) -> Option<usize> {
    text.windows(needle.len()).position(|window| window == needle)
}

fn rfind_subslice(text: &[u8], needle: &[u8]) -> Option<usize> {
    text.windows(needle.len()).rposition(|window| window == needle)
}

fn safe_utf8_split(text: &[u8], limit: usize) -> usize {
    let mut split_at = limit.min(text.len());
    while split_at > 0 && std::str::from_utf8(&text[..split_at]).is_err() {
        split_at -= 1;
    }
    split_at
}

fn adjust_xml_entity(text: &[u8], split_at: usize) -> usize {
    let mut adjusted = split_at;
    while adjusted > 0 {
        let Some(amp) = text[..adjusted].iter().rposition(|byte| *byte == b'&') else {
            break;
        };
        if text[amp..].iter().position(|byte| *byte == b';').is_some_and(|semi| amp + semi < adjusted) {
            break;
        }
        adjusted = amp;
    }
    adjusted
}

fn trim_trailing_zeros(bytes: &mut Vec<u8>) {
    while bytes.last() == Some(&0) {
        bytes.pop();
    }
}

fn connect_id() -> String {
    Uuid::new_v4().simple().to_string()
}

fn generate_muid() -> String {
    connect_id().to_uppercase()
}

fn date_to_string() -> String {
    Utc::now()
        .format("%a %b %d %Y %H:%M:%S GMT+0000 (Coordinated Universal Time)")
        .to_string()
}

fn generate_sec_ms_gec_token() -> Result<String, String> {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(to_string)?
        .as_secs();
    let mut ticks = (secs + 11_644_473_600) * 10_000_000;
    ticks -= ticks % 3_000_000_000;

    let mut hasher = Sha256::new();
    hasher.update(format!("{ticks}{TRUSTED_CLIENT_TOKEN}"));
    Ok(hasher.finalize().iter().map(|byte| format!("{byte:02X}")).collect())
}

fn generate_sec_ms_gec_version() -> String {
    format!("1-{CHROMIUM_FULL_VERSION}")
}

fn user_agent() -> String {
    format!("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/{CHROMIUM_MAJOR_VERSION}.0.0.0 Safari/537.36 Edg/{CHROMIUM_MAJOR_VERSION}.0.0.0")
}

fn half_progress(index: usize, total: usize) -> u8 {
    (((index * 100 + 50) / total).clamp(1, 99)) as u8
}

fn full_progress(index: usize, total: usize) -> u8 {
    ((index * 100 / total).clamp(1, 100)) as u8
}

fn emit_progress(app: Option<&tauri::AppHandle>, job_id: u64, percent: u8) {
    if let Some(app) = app {
        let _ = app.emit("save-progress", ProgressPayload { job_id, percent });
    }
}

fn ensure_current(state: &AppState, job_id: u64) -> Result<(), String> {
    if state.current_job.load(Ordering::SeqCst) == job_id {
        Ok(())
    } else {
        Err("已停止".into())
    }
}

fn to_string(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn init_tls() {
    TLS_INIT.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
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

fn read_selected_text(app: tauri::AppHandle) {
    thread::spawn(move || {
        simulate_ctrl_c();
        thread::sleep(Duration::from_millis(180));
        let Ok(mut clipboard) = arboard::Clipboard::new() else {
            return;
        };
        let Ok(text) = clipboard.get_text() else {
            return;
        };
        let text = text.replace(['\r', '\n'], "").trim().to_string();
        if !text.is_empty() {
            let _ = app.emit("hotkey-text", text);
        }
    });
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
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![get_voices, synthesize_text, save_audio, stop])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split_keeps_utf8_and_xml_entities_intact() {
        let text = escape_xml("你好 & <world>。再来一段很长很长的中文文本，用来触发切分。");
        let parts = split_text_for_playback(&text, 16, 32);
        assert!(parts.len() > 1);
        assert_eq!(parts.concat(), text);
        assert!(parts.iter().all(|part| !part.ends_with('&')));
    }

    #[test]
    fn token_shape_matches_edge_requirement() {
        let token = generate_sec_ms_gec_token().unwrap();
        assert_eq!(token.len(), 64);
        assert!(token.chars().all(|ch| ch.is_ascii_hexdigit() && !ch.is_ascii_lowercase()));
    }

    #[test]
    fn byte_split_is_used_for_save_path() {
        let text = "a".repeat(MAX_TEXT_BYTES + 8);
        let parts = split_text_by_byte_length(&text, MAX_TEXT_BYTES);
        assert_eq!(parts.len(), 2);
    }

    #[tokio::test]
    #[ignore]
    async fn live_synthesis_smoke() {
        let state = AppState::default();
        state.current_job.store(1, Ordering::SeqCst);
        let audio = synthesize("测试", "zh-CN, XiaoyiNeural", 1, &state, None).await.unwrap();
        assert!(audio.len() > 1024);
    }
}
