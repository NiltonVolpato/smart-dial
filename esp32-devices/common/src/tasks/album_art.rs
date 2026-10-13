// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Decodes album art for the UI's core (see the `album-art` crate), wherever
//! it's spawned: away from the UI, as decoding takes ~100 ms or more.

use album_art::{ArtRequests, ArtResults};
use embassy_time::Instant;

#[embassy_executor::task]
pub async fn album_art_task(requests: &'static ArtRequests, results: &'static ArtResults) {
    loop {
        let request = requests.receive().await;
        let started = Instant::now();
        let bytes = request.jpeg.len();
        let result = album_art::process(request);
        defmt::info!(
            "[ALBUM ART] {} B decoded in {} ms ({})",
            bytes,
            started.elapsed().as_millis(),
            if result.pixels.is_some() { "ok" } else { "failed" }
        );
        results.send(result).await;
    }
}
