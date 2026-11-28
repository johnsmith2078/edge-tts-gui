use std::{
    path::{Path, PathBuf},
    process::Command,
    sync::{Mutex, OnceLock},
};

struct OcrResource {
    path: &'static str,
    bytes: &'static [u8],
}

macro_rules! ocr_res {
    ($path:literal) => {
        OcrResource {
            path: $path,
            bytes: include_bytes!(concat!("../rapidocr/", $path)),
        }
    };
}

static OCR_RESOURCES: &[OcrResource] = &[
    ocr_res!("win-BIN-CPU-x64/RapidOcrOnnx.exe"),
    ocr_res!("models/ch_PP-OCRv4_det_infer.onnx"),
    ocr_res!("models/ch_PP-OCRv4_rec_infer.onnx"),
    ocr_res!("models/ch_ppocr_mobile_v2.0_cls_infer.onnx"),
    ocr_res!("models/ppocr_keys_v1.txt"),
];

const OCR_READY_MARKER: &str = ".ocr-ready";
static OCR_DIR: OnceLock<PathBuf> = OnceLock::new();
static OCR_EXTRACT_LOCK: Mutex<()> = Mutex::new(());

fn ocr_dir() -> &'static Path {
    OCR_DIR.get_or_init(|| std::env::temp_dir().join("edge-tts-gui-ocr"))
}

fn is_ocr_dir_ready(dir: &Path, resources: &[OcrResource]) -> bool {
    dir.join(OCR_READY_MARKER).is_file()
        && resources.iter().all(|r| {
            std::fs::metadata(dir.join(r.path))
                .map(|meta| meta.len() == r.bytes.len() as u64)
                .unwrap_or(false)
        })
}

fn extract_ocr_resources(dir: &Path, resources: &[OcrResource]) -> Result<(), String> {
    if is_ocr_dir_ready(dir, resources) {
        return Ok(());
    }
    let _ = std::fs::remove_dir_all(dir);
    for r in resources {
        let dest = dir.join(r.path);
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("mkdir: {e}"))?;
        }
        std::fs::write(&dest, r.bytes).map_err(|e| format!("write {}: {e}", r.path))?;
    }
    std::fs::write(dir.join(OCR_READY_MARKER), b"ok")
        .map_err(|e| format!("write {OCR_READY_MARKER}: {e}"))?;
    Ok(())
}

fn ensure_ocr_extracted() -> Result<(), String> {
    let _guard = OCR_EXTRACT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    extract_ocr_resources(ocr_dir(), OCR_RESOURCES)
}

fn cleanup_ocr_artifacts(dir: &Path, stem: &str) {
    for suffix in [".png", "-result.jpg", "-result.txt"] {
        let _ = std::fs::remove_file(dir.join(format!("{stem}{suffix}")));
    }
}

pub fn perform_ocr(image_bytes: &[u8], width: u32, height: u32) -> Result<String, String> {
    ensure_ocr_extracted()?;
    let dir = ocr_dir();
    let stem = format!("_ocr_input_{}", uuid::Uuid::new_v4().simple());
    let image_path = dir.join(format!("{stem}.png"));
    image::save_buffer(&image_path, image_bytes, width, height, image::ColorType::Rgba8)
        .map_err(|e| format!("save image: {e}"))?;

    let exe = dir.join("win-BIN-CPU-x64").join("RapidOcrOnnx.exe");
    let models = dir.join("models");
    let mut cmd = Command::new(&exe);
    cmd.current_dir(dir);
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    let output = cmd
        .arg("--models").arg(&models)
        .arg("--det").arg("ch_PP-OCRv4_det_infer.onnx")
        .arg("--cls").arg("ch_ppocr_mobile_v2.0_cls_infer.onnx")
        .arg("--rec").arg("ch_PP-OCRv4_rec_infer.onnx")
        .arg("--keys").arg("ppocr_keys_v1.txt")
        .arg("--image").arg(&image_path)
        .output();

    cleanup_ocr_artifacts(dir, &stem);
    let output = output.map_err(|e| format!("run OCR: {e}"))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(format!("OCR exited: {}", stderr.trim()));
    }

    let raw = String::from_utf8_lossy(&output.stdout).to_string();
    // Strip diagnostic lines before the real result
    if let Some(pos) = raw.rfind("FullDetectTime") {
        if let Some(nl) = raw[pos..].find('\n') {
            return Ok(raw[pos + nl + 1..].to_string());
        }
    }
    Ok(raw)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cleanup_ocr_artifacts_only_removes_current_run_files() {
        let dir = std::env::temp_dir().join(format!(
            "edge-tts-ocr-clean-{}",
            uuid::Uuid::new_v4().simple()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let stem = "_ocr_input_abc";
        for suffix in [".png", "-result.jpg", "-result.txt"] {
            std::fs::write(dir.join(format!("{stem}{suffix}")), b"tmp").unwrap();
        }
        let user_jpg = dir.join("user-result.jpg");
        let user_txt = dir.join("user-result.txt");
        std::fs::write(&user_jpg, b"keep").unwrap();
        std::fs::write(&user_txt, b"keep").unwrap();

        cleanup_ocr_artifacts(&dir, stem);

        for suffix in [".png", "-result.jpg", "-result.txt"] {
            assert!(!dir.join(format!("{stem}{suffix}")).exists());
        }
        assert!(user_jpg.exists());
        assert!(user_txt.exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn extract_ocr_resources_recovers_from_partial_extraction() {
        let dir = std::env::temp_dir().join(format!(
            "edge-tts-ocr-extract-{}",
            uuid::Uuid::new_v4().simple()
        ));
        let resources = [
            OcrResource {
                path: "win-BIN-CPU-x64/RapidOcrOnnx.exe",
                bytes: b"exe-bytes",
            },
            OcrResource {
                path: "models/det.onnx",
                bytes: b"model-bytes",
            },
        ];

        let partial_exe = dir.join(resources[0].path);
        std::fs::create_dir_all(partial_exe.parent().unwrap()).unwrap();
        std::fs::write(&partial_exe, resources[0].bytes).unwrap();
        assert!(!is_ocr_dir_ready(&dir, &resources));

        extract_ocr_resources(&dir, &resources).unwrap();
        assert!(is_ocr_dir_ready(&dir, &resources));
        assert_eq!(
            std::fs::read(dir.join(resources[0].path)).unwrap(),
            resources[0].bytes
        );
        assert_eq!(
            std::fs::read(dir.join(resources[1].path)).unwrap(),
            resources[1].bytes
        );

        std::fs::write(dir.join(resources[1].path), b"x").unwrap();
        assert!(!is_ocr_dir_ready(&dir, &resources));
        extract_ocr_resources(&dir, &resources).unwrap();
        assert!(is_ocr_dir_ready(&dir, &resources));
        assert_eq!(
            std::fs::read(dir.join(resources[1].path)).unwrap(),
            resources[1].bytes
        );

        let _ = std::fs::remove_dir_all(&dir);
    }
}
