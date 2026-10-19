use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    println!("cargo::rerun-if-changed=idf_component.yml");

    if !target.starts_with("xtensa") {
        return;
    }

    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set"));
    let component_dir = out_dir.join("esp_new_jpeg");
    let lib_dir = component_dir.join("lib").join("esp32s3");
    let lib_file = lib_dir.join("libesp_new_jpeg.a");

    if !lib_file.exists() {
        fetch_esp_new_jpeg(&manifest_dir, &component_dir);
    }

    assert!(
        lib_file.exists(),
        "libesp_new_jpeg.a missing after component fetch at {}",
        lib_file.display()
    );

    println!("cargo::rustc-link-search=native={}", lib_dir.display());
    println!("cargo::rustc-link-lib=static=esp_new_jpeg");
}

fn find_idf_python() -> String {
    if let Ok(home) = env::var("HOME") {
        let env_base = Path::new(&home).join(".espressif").join("python_env");
        if let Ok(entries) = std::fs::read_dir(env_base) {
            for entry in entries.flatten() {
                let candidate = entry.path().join("bin").join("python");
                if candidate.exists() {
                    return candidate.to_string_lossy().into_owned();
                }
            }
        }
    }
    "python3".to_string()
}

fn fetch_esp_new_jpeg(manifest_dir: &Path, dest_dir: &Path) {
    let python = find_idf_python();
    let script = r#"
import os, shutil, sys
from idf_component_tools.manager import ManifestManager
from idf_component_tools.manifest import SolvedComponent
from idf_component_tools.registry.service_details import get_storage_client
from idf_component_tools.sources.web_service import WebServiceSource

manifest_dir, dest_dir = sys.argv[1], sys.argv[2]
manifest = ManifestManager(manifest_dir, "jpeg").load()
dep_name, dep_spec = next((k, v) for k, v in manifest.dependencies.items() if "esp_new_jpeg" in k)
version_spec = str(dep_spec).lstrip("^~=")

client = get_storage_client()
info = client.component(dep_name, version_spec)
source = WebServiceSource()
comp = SolvedComponent(
    name=dep_name,
    version=info["version"],
    source=source,
    component_hash=info["component_hash"],
)
if os.path.exists(dest_dir):
    shutil.rmtree(dest_dir)
source.download(comp, dest_dir)
"#;

    let status = Command::new(&python)
        .args(["-c", script])
        .arg(manifest_dir)
        .arg(dest_dir)
        .status()
        .unwrap_or_else(|e| panic!("Failed to execute {python} to fetch esp_new_jpeg: {e}"));

    assert!(
        status.success(),
        "ESP-IDF component manager failed to fetch espressif/esp_new_jpeg"
    );
}
