use std::path::Path;

fn main() {
    println!("cargo:rerun-if-changed=../../RapidOCR");
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../RapidOCR");
    let dst = Path::new(env!("CARGO_MANIFEST_DIR")).join("rapidocr");
    if src.exists() {
        sync_dir(&src, &dst).expect("sync RapidOCR into crate");
    }
    tauri_build::build();
}

fn needs_copy(src: &Path, dst: &Path) -> Result<bool, std::io::Error> {
    if !dst.exists() {
        return Ok(true);
    }
    let src_meta = std::fs::metadata(src)?;
    let dst_meta = std::fs::metadata(dst)?;
    if src_meta.len() != dst_meta.len() {
        return Ok(true);
    }
    Ok(src_meta.modified()? > dst_meta.modified()?)
}

fn sync_dir(src: &Path, dst: &Path) -> Result<(), std::io::Error> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let path = entry.path();
        let dest = dst.join(entry.file_name());
        if path.is_dir() {
            sync_dir(&path, &dest)?;
        } else if needs_copy(&path, &dest)? {
            std::fs::copy(&path, &dest)?;
        }
    }
    Ok(())
}
