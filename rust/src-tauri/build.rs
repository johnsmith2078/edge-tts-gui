use std::path::Path;

fn main() {
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../RapidOCR");
    let dst = Path::new(env!("CARGO_MANIFEST_DIR")).join("rapidocr");
    if src.exists() && !dst.exists() {
        copy_dir(&src, &dst).expect("copy RapidOCR into crate");
    }
    tauri_build::build();
}

fn copy_dir(src: &Path, dst: &Path) -> Result<(), std::io::Error> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let path = entry.path();
        let dest = dst.join(entry.file_name());
        if path.is_dir() {
            copy_dir(&path, &dest)?;
        } else {
            std::fs::copy(&path, &dest)?;
        }
    }
    Ok(())
}
