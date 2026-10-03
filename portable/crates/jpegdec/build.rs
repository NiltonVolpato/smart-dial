use std::env;

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    let manifest_dir = env!("CARGO_MANIFEST_DIR");
    let jpegdec_src = format!("{manifest_dir}/../../../third_party/JPEGDEC/src");

    println!("cargo::rerun-if-changed={jpegdec_src}/JPEGDEC.cpp");
    println!("cargo::rerun-if-changed={jpegdec_src}/JPEGDEC.h");
    println!("cargo::rerun-if-changed={jpegdec_src}/jpeg.inl");
    println!("cargo::rerun-if-changed=src/dsps_fft2r_platform.h");
    println!("cargo::rerun-if-changed=src/wrapper.cpp");

    let mut build = cc::Build::new();
    build.cpp(true);

    if target.starts_with("xtensa") {
        build.compiler("xtensa-esp32s3-elf-g++");
        build.define("ESP_PLATFORM", None);
        build.define("ARDUINO_ARCH_ESP32", None);
    } else {
        build.define("__LINUX__", None);
    }

    build
        .include(&jpegdec_src)
        .include(format!("{manifest_dir}/src"))
        .flag_if_supported("-Wno-shift-negative-value")
        .file(format!("{jpegdec_src}/JPEGDEC.cpp"))
        .file("src/wrapper.cpp");

    if target.starts_with("xtensa") {
        build
            .file(format!("{jpegdec_src}/s3_simd_420.S"))
            .file(format!("{jpegdec_src}/s3_simd_444.S"))
            .file(format!("{jpegdec_src}/s3_simd_dequant.S"));
    }

    build.compile("jpegdec");
}
