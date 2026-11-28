use std::{sync::Once, time::Duration};

use chrono::Utc;
use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use tauri::{Emitter, Manager};
use tokio::net::TcpStream;
use tokio_tungstenite::{
    connect_async,
    tungstenite::{
        client::IntoClientRequest,
        http::{HeaderValue, StatusCode},
        Error as WsError,
        Message,
    },
    MaybeTlsStream, WebSocketStream,
};
use uuid::Uuid;

use crate::drm;

const TRUSTED_CLIENT_TOKEN: &str = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const WSS_URL: &str = "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const CHROMIUM_FULL_VERSION: &str = "143.0.3650.75";
const CHROMIUM_MAJOR_VERSION: &str = "143";
const MAX_TEXT_BYTES: usize = 4096;
const INITIAL_TEXT_BYTES: usize = 80;
const TARGET_TEXT_BYTES: usize = 780;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const READ_TIMEOUT: Duration = Duration::from_secs(30);
static TLS_INIT: Once = Once::new();

pub struct AppState {
    job_tx: tokio::sync::watch::Sender<u64>,
}

impl Default for AppState {
    fn default() -> Self {
        let (job_tx, _) = tokio::sync::watch::channel(0);
        Self { job_tx }
    }
}

impl AppState {
    pub fn start_job(&self, job_id: u64) -> tokio::sync::watch::Receiver<u64> {
        self.job_tx.send_replace(job_id);
        self.job_tx.subscribe()
    }

    fn subscribe(&self) -> tokio::sync::watch::Receiver<u64> {
        self.job_tx.subscribe()
    }

    pub fn cancel_current(&self) {
        let next = (*self.job_tx.borrow()).wrapping_add(1);
        let _ = self.job_tx.send(next);
    }
}

#[derive(Clone, Serialize)]
struct ProgressPayload {
    job_id: u64,
    percent: u8,
}

#[derive(Clone, Serialize)]
struct PlaybackChunkPayload {
    job_id: u64,
    audio_base64: String,
}

#[derive(Clone, Serialize)]
struct PlaybackDonePayload {
    job_id: u64,
}

async fn run_cancellable<T, F>(
    rx: &mut tokio::sync::watch::Receiver<u64>,
    job_id: u64,
    limit: Duration,
    timeout_msg: &str,
    fut: F,
) -> Result<T, String>
where
    F: std::future::Future<Output = T>,
{
    tokio::select! {
        biased;
        _ = rx.wait_for(|&cur| cur != job_id) => Err("已停止".into()),
        res = tokio::time::timeout(limit, fut) => res.map_err(|_| timeout_msg.to_string()),
    }
}

async fn synthesize_parts<F>(
    parts: &[String],
    voice: &str,
    job_id: u64,
    state: &AppState,
    app: Option<&tauri::AppHandle>,
    mut on_turn_audio: F,
) -> Result<(), String>
where
    F: FnMut(&[u8]) -> Result<(), String>,
{
    init_tls();
    let mut rx = state.subscribe();
    if *rx.borrow() != job_id {
        return Err("已停止".into());
    }
    if parts.is_empty() {
        return Err("文本为空".into());
    }

    let socket = run_cancellable(
        &mut rx,
        job_id,
        CONNECT_TIMEOUT,
        "连接语音服务超时",
        connect_tts_socket(),
    )
    .await??;
    let (mut write, mut read) = socket.split();

    let mut current_chunk = Vec::new();
    let mut index = 0usize;
    let mut downloading = false;
    run_cancellable(
        &mut rx,
        job_id,
        READ_TIMEOUT,
        "发送语音请求超时",
        send_text_part(&mut write, &parts[index], voice),
    )
    .await??;
    emit_progress(app, job_id, 1);

    while let Some(message) = run_cancellable(
        &mut rx,
        job_id,
        READ_TIMEOUT,
        "接收语音数据超时",
        read.next(),
    )
    .await?
    {
        if *rx.borrow() != job_id {
            return Err("已停止".into());
        }
        match message.map_err(to_string)? {
            Message::Binary(message) => {
                if !downloading {
                    return Err("收到意外音频数据".into());
                }
                let data = parse_audio_message(&message)?;
                if !data.is_empty() {
                    current_chunk.extend_from_slice(data);
                    emit_progress(app, job_id, half_progress(index, parts.len()));
                }
            }
            Message::Text(message) => {
                let headers = parse_headers(&message);
                match headers.get("Path").map(String::as_str) {
                    Some("turn.start") => downloading = true,
                    Some("turn.end") => {
                        if current_chunk.is_empty() {
                            return Err("未收到音频数据".into());
                        }
                        on_turn_audio(&current_chunk)?;
                        current_chunk.clear();
                        downloading = false;
                        index += 1;
                        emit_progress(app, job_id, full_progress(index, parts.len()));
                        if index >= parts.len() {
                            break;
                        }
                        run_cancellable(
                            &mut rx,
                            job_id,
                            READ_TIMEOUT,
                            "发送语音请求超时",
                            send_text_part(&mut write, &parts[index], voice),
                        )
                        .await??;
                    }
                    Some("audio.metadata" | "response") => {}
                    _ => return Err(format!("无法识别服务响应: {message}")),
                }
            }
            Message::Close(_) => break,
            _ => {}
        }
    }

    if index < parts.len() {
        return Err("语音服务在合成完成前断开了连接".into());
    }
    Ok(())
}

pub async fn synthesize(
    text: &str,
    voice: &str,
    job_id: u64,
    state: &AppState,
    app: Option<&tauri::AppHandle>,
) -> Result<Vec<u8>, String> {
    let clean = escape_xml(&remove_incompatible_characters(text));
    let parts = if app.is_some() {
        split_text_by_byte_length(&clean, MAX_TEXT_BYTES)
    } else {
        split_text_for_playback(&clean, INITIAL_TEXT_BYTES, TARGET_TEXT_BYTES)
    };
    let mut audio = Vec::with_capacity(text.len().max(1024) * 128);
    synthesize_parts(&parts, voice, job_id, state, app, |chunk| {
        audio.extend_from_slice(chunk);
        Ok(())
    })
    .await?;
    emit_progress(app, job_id, 100);
    Ok(audio)
}

pub async fn synthesize_streaming(
    text: &str,
    voice: &str,
    job_id: u64,
    app: &tauri::AppHandle,
) -> Result<(), String> {
    use base64::Engine;
    let state = app.state::<AppState>();
    let clean = escape_xml(&remove_incompatible_characters(text));
    let parts = split_text_for_playback(&clean, INITIAL_TEXT_BYTES, TARGET_TEXT_BYTES);
    synthesize_parts(&parts, voice, job_id, &state, Some(app), |chunk| {
        let b64 = base64::engine::general_purpose::STANDARD.encode(chunk);
        app.emit("playback-chunk", PlaybackChunkPayload {
            job_id,
            audio_base64: b64,
        })
        .map_err(to_string)
    })
    .await?;
    app.emit("playback-done", PlaybackDonePayload { job_id })
        .map_err(to_string)
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
    let headers = std::str::from_utf8(&message[2..header_len + 2]).map_err(to_string)?;
    let headers = parse_headers(headers);
    let payload = &message[header_len + 2..];
    if headers.get("Path").map(String::as_str) != Some("audio") {
        return Err("收到意外的二进制消息".into());
    }
    match (headers.get("Content-Type").map(String::as_str), payload.is_empty()) {
        (Some("audio/mpeg"), false) | (None, true) => Ok(payload),
        _ => Err("音频消息的类型与数据不匹配".into()),
    }
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

fn escape_xml_attr(text: &str) -> String {
    escape_xml(text).replace('\'', "&apos;").replace('"', "&quot;")
}

fn mkssml(text: &str, voice: &str) -> String {
    let safe_voice = escape_xml_attr(voice);
    format!(
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><voice name='Microsoft Server Speech Text to Speech Voice ({safe_voice})'><prosody pitch='+0Hz' rate='+0%' volume='+0%'>{text}</prosody></voice></speak>"
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
    drm::generate_sec_ms_gec_v2(TRUSTED_CLIENT_TOKEN)
}

fn generate_sec_ms_gec_version() -> String {
    format!("2-{CHROMIUM_FULL_VERSION}")
}

fn build_tts_request() -> Result<tokio_tungstenite::tungstenite::http::Request<()>, String> {
    let url = format!(
        "{WSS_URL}&ConnectionId={}&Sec-MS-GEC={}&Sec-MS-GEC-Version={}",
        connect_id(),
        generate_sec_ms_gec_token()?,
        generate_sec_ms_gec_version(),
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
    headers.insert(
        "Cookie",
        HeaderValue::from_str(&format!("muid={};", generate_muid())).map_err(to_string)?,
    );
    Ok(request)
}

async fn connect_tts_socket() -> Result<WebSocketStream<MaybeTlsStream<TcpStream>>, String> {
    let request = build_tts_request()?;
    match connect_async(request).await {
        Ok((socket, _)) => Ok(socket),
        Err(WsError::Http(response)) if response.status() == StatusCode::FORBIDDEN => {
            if let Some(date) = response
                .headers()
                .get("date")
                .and_then(|value| value.to_str().ok())
            {
                if drm::adjust_clock_skew_from_date(date).is_ok() {
                    let retry_request = build_tts_request()?;
                    return connect_async(retry_request)
                        .await
                        .map(|(socket, _)| socket)
                        .map_err(|err| format!("连接失败: {err}"));
                }
            }
            Err(format!("连接失败: HTTP error: {}", response.status()))
        }
        Err(err) => Err(format!("连接失败: {err}")),
    }
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

fn to_string(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn init_tls() {
    TLS_INIT.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn audio_frame(headers: &str, payload: &[u8]) -> Vec<u8> {
        let mut frame = (headers.len() as u16).to_be_bytes().to_vec();
        frame.extend_from_slice(headers.as_bytes());
        frame.extend_from_slice(payload);
        frame
    }

    #[test]
    fn audio_frames_require_mpeg_data_or_an_empty_end_marker() {
        let payload = [0xff, 0xf3, 0, 0];
        let headers = "Path:audio\r\nContent-Type:audio/mpeg\r\n";
        assert_eq!(parse_audio_message(&audio_frame(headers, &payload)).unwrap(), payload);
        assert_eq!(parse_audio_message(&audio_frame("Path:audio\r\n", &[])).unwrap(), &[] as &[u8]);
        assert!(parse_audio_message(&audio_frame(headers, &[])).is_err());
        assert!(parse_audio_message(&audio_frame("Path:audio\r\n", &payload)).is_err());
        assert!(parse_audio_message(&audio_frame("Path:response\r\n", &[])).is_err());
    }

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
        let (cs64, sha256) = token.split_once('-').expect("v2 token should contain '-'");
        assert_eq!(cs64.len(), 16);
        assert_eq!(sha256.len(), 64);
        assert!(cs64.chars().all(|ch| ch.is_ascii_hexdigit() && !ch.is_ascii_lowercase()));
        assert!(sha256.chars().all(|ch| ch.is_ascii_hexdigit() && !ch.is_ascii_lowercase()));
        assert_eq!(generate_sec_ms_gec_version(), format!("2-{CHROMIUM_FULL_VERSION}"));
    }

    #[test]
    fn byte_split_is_used_for_save_path() {
        let text = "a".repeat(MAX_TEXT_BYTES + 8);
        let parts = split_text_by_byte_length(&text, MAX_TEXT_BYTES);
        assert_eq!(parts.len(), 2);
    }

    #[test]
    fn mkssml_escapes_voice_attribute() {
        let ssml = mkssml("hello", "zh-CN, Xiaoyi'&<\"Neural");
        assert!(ssml.contains("Xiaoyi&apos;&amp;&lt;&quot;Neural"));
    }

    #[tokio::test]
    async fn run_cancellable_aborts_when_job_stopped() {
        let state = AppState::default();
        let mut rx = state.start_job(7);
        let cancel_task = tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(20)).await;
            state.cancel_current();
        });
        let err = run_cancellable(
            &mut rx,
            7,
            Duration::from_secs(5),
            "超时",
            tokio::time::sleep(Duration::from_secs(5)),
        )
        .await
        .unwrap_err();
        cancel_task.await.unwrap();
        assert_eq!(err, "已停止");
    }

    #[tokio::test]
    async fn run_cancellable_returns_timeout_error() {
        let state = AppState::default();
        let mut rx = state.start_job(3);
        let err = run_cancellable(
            &mut rx,
            3,
            Duration::from_millis(15),
            "操作超时",
            tokio::time::sleep(Duration::from_secs(5)),
        )
        .await
        .unwrap_err();
        assert_eq!(err, "操作超时");
    }

    #[tokio::test]
    #[ignore]
    async fn live_synthesis_smoke() {
        let state = AppState::default();
        let _ = state.start_job(1);
        let audio = synthesize("测试", "zh-CN, XiaoyiNeural", 1, &state, None).await.unwrap();
        println!("single chunk audio bytes: {}", audio.len());
        assert!(audio.len() > 1024);
        assert_eq!(audio[0], 0xFF, "expected MP3 frame sync byte");
        assert_eq!(audio[1] & 0xE0, 0xE0, "expected MP3 frame sync bits");

        let long_text = "这是一段用于验证多分片流式合成的中文测试文本。第一句用来触发首片切分，第二句紧随其后验证同一连接下多轮合成是否正常返回完整的音频数据。";
        let parts = split_text_for_playback(&escape_xml(long_text), INITIAL_TEXT_BYTES, TARGET_TEXT_BYTES);
        assert!(parts.len() > 1, "expected multiple chunks, got {}", parts.len());
        let multi_audio = synthesize(long_text, "zh-CN, XiaoyiNeural", 1, &state, None).await.unwrap();
        println!("multi chunk ({} parts) audio bytes: {}", parts.len(), multi_audio.len());
        assert!(multi_audio.len() > audio.len());
        assert_eq!(multi_audio[0], 0xFF);
        assert_eq!(multi_audio[1] & 0xE0, 0xE0);
    }
}
