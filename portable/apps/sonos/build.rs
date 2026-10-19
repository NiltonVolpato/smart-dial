fn main() -> Result<(), Box<dyn std::error::Error>> {
    baker::bake_library("app_sonos", "ui/sonos.slint")
}
