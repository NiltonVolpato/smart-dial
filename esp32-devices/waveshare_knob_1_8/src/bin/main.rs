#![no_std]
#![no_main]
#![deny(clippy::large_stack_frames)]

use alloc::boxed::Box;

use app_launcher::LauncherAppFactory;
use app_shell::AppShell;
use defmt::info;
use embassy_executor::Spawner;
use esp_hal::clock::CpuClock;
use esp_hal::interrupt::software::{SoftwareInterrupt, SoftwareInterruptControl};
use esp_hal::system::Stack;
use esp_hal::time::Instant;
use esp_hal::timer::timg::TimerGroup;
use panic_rtt_target as _;
use waveshare_knob_1_8::bsp::{
    Board, Bsp, Core1Peripherals, Sh8601, battery_task, coprocessor_task, create_shared_client,
    display_task, haptic_task, rotary_task, run_event_loop, simd, system_task, touch_task,
};
use waveshare_knob_1_8::tasks::{PROFILER_ENABLED, profiler_task, screensaver_task};

extern crate alloc;

defmt::timestamp!("{=u64:tus} C{}", { Instant::now().duration_since_epoch().as_micros() }, {
    esp_hal::system::Cpu::current() as usize
});

esp_bootloader_esp_idf::esp_app_desc!();

/// Core 1 execution stack (16 KiB, 16-byte aligned).
const APP_CORE_STACK_SIZE: usize = 16 * 1024;
static mut APP_CORE_STACK: Stack<APP_CORE_STACK_SIZE> = Stack::new();

/// Entry point for Core 1 (`AppCpu`): owns SH8601 display initialization and QSPI DMA transfers.
fn core1_entry(core1: Core1Peripherals, sw_int2: SoftwareInterrupt<'static, 2>) -> ! {
    simd::enable_pie();
    let display = Sh8601::new(core1.display);

    let interrupt_executor =
        Box::leak(Box::new(esp_rtos::embassy::InterruptExecutor::new(sw_int2)));
    let send_spawner = interrupt_executor.start(esp_hal::interrupt::Priority::Priority1);
    send_spawner.spawn(display_task(display).expect("Failed to create display task"));

    let executor = Box::leak(Box::new(esp_rtos::embassy::Executor::new()));
    executor.run(|_spawner| {});
}

#[allow(clippy::large_stack_frames)]
#[esp_rtos::main]
async fn main(spawner: Spawner) -> ! {
    rtt_target::rtt_init_defmt!();
    simd::enable_pie();

    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let mut peripherals = esp_hal::init(config);
    // ADC1 seeds the TRNG: both are released at the end of the block (the TRNG
    // first), as the battery task needs ADC1.
    let boot_id = {
        let _trng_source =
            esp_hal::rng::TrngSource::new(peripherals.RNG.reborrow(), peripherals.ADC1.reborrow());
        let trng = esp_hal::rng::Trng::try_new().expect("Failed to initialize TRNG");
        trng.random() | 1
    };
    let board = Board::new(peripherals);

    // 1. Initialize PSRAM (Region 0 - default general allocator)
    esp_alloc::psram_allocator!(board.system.psram, esp_hal::psram);

    // 2. Register internal DRAM heaps (reclaimed bootloader memory for DMA buffers)
    esp_alloc::heap_allocator!(#[esp_hal::ram(reclaimed)] size: 73744);

    // 3. Initialize RTOS & Embassy tick driver
    let timg0 = TimerGroup::new(board.system.timg0);
    let sw_int = SoftwareInterruptControl::new(board.system.sw_interrupt);
    esp_rtos::start(timg0.timer0, sw_int.software_interrupt0);

    // 4. Start Core 1 (AppCpu) for display and QSPI DMA transfers
    esp_rtos::start_second_core::<APP_CORE_STACK_SIZE>(
        board.system.cpu_ctrl,
        sw_int.software_interrupt1,
        unsafe { &mut *core::ptr::addr_of_mut!(APP_CORE_STACK) },
        move || {
            core1_entry(board.core1, sw_int.software_interrupt2);
        },
    );

    info!("Memory & RTOS initialized, bringing up BSP on Core 0...");

    // 5. Initialize BSP on Core 0 (touch, rotary, Slint platform, profiler)
    let bsp = Bsp::init(board.core0);

    // 6. Spawn interrupt-driven input, haptics & screensaver tasks
    let feedback_channel = alloc::rc::Rc::new(waveshare_knob_1_8::bsp::HapticChannel::new());

    spawner.spawn(rotary_task(bsp.rotary_a, bsp.rotary_b).expect("Failed to create rotary task"));
    if let Some(touch) = bsp.touch {
        spawner.spawn(touch_task(touch).expect("Failed to create touch task"));
    }
    if let Some(haptic_i2c) = bsp.haptic_i2c {
        spawner.spawn(
            haptic_task(haptic_i2c, feedback_channel.clone())
                .expect("Failed to create haptic task"),
        );
    }
    let coprocessor_client = create_shared_client(boot_id);

    spawner.spawn(
        coprocessor_task(
            bsp.coprocessor.uart,
            bsp.coprocessor.tx,
            bsp.coprocessor.rx,
            coprocessor_client.clone(),
        )
        .expect("Failed to create coprocessor task"),
    );
    spawner.spawn(
        system_task(coprocessor::system::Client(coprocessor_client.clone()))
            .expect("Failed to create system task"),
    );
    spawner.spawn(
        battery_task(
            bsp.battery.adc,
            bsp.battery.pin,
            feedback_channel.clone(),
            coprocessor::system::Client(coprocessor_client.clone()),
        )
        .expect("Failed to create battery task"),
    );
    spawner.spawn(screensaver_task().expect("Failed to create screensaver task"));
    if PROFILER_ENABLED {
        spawner.spawn(profiler_task(bsp.profiler_timer).expect("Failed to create profiler task"));
    }

    info!("Starting AppShell with Launcher on Core 0...");
    let sonos_factory = Box::new(app_sonos::SonosAppFactory::new(coprocessor::sonos::Client(
        coprocessor_client.clone(),
    )));
    let launcher = LauncherAppFactory::default_apps_with_sonos(sonos_factory);
    let platform = alloc::rc::Rc::new(common::EspAppShellPlatform::new(
        Some(waveshare_knob_1_8::bsp::rtc::current_timestamp),
        Some(waveshare_knob_1_8::bsp::rtc::current_zoned),
        Some(feedback_channel.clone()),
    ));
    let shell = AppShell::new(spawner, platform, Box::new(launcher));
    AppShell::start(&shell);

    // 7. Enter Slint MCU event loop on Core 0
    let feedback = waveshare_knob_1_8::bsp::WaveshareFeedback(feedback_channel);
    run_event_loop(bsp.window, &feedback, coprocessor::wifi::Client(coprocessor_client)).await;
}
