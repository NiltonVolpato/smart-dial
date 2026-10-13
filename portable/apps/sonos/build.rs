fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-env-changed=SONOS_SEED_IP");
    baker::bake_library("app_sonos", "ui/sonos.slint")
}
