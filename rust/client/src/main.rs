#![forbid(unsafe_code)]

use ns_client::cli::CliOptions;
use ns_client::macro_client::MacroClient;
use ns_client::main_window::open_web_ui;
use ns_client::stream_runtime::StreamRuntime;
use ns_client::udp_protocol::UdpClient;
use ns_shared::protocol::{
    Hat, MultiReport, BTN_A, BTN_B, BTN_CAPTURE, BTN_HOME, BTN_L, BTN_MINUS, BTN_PLUS,
    BTN_R, BTN_X, BTN_Y, BTN_ZL, BTN_ZR, EXT_PAD_PRESENT,
};
use std::env;
use std::io::{self, BufRead};
use std::sync::mpsc;
use std::thread;
use std::time::Instant;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let options = match CliOptions::parse(env::args().skip(1)) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}");
            return Ok(());
        }
    };
    if options.open_web() {
        open_web_ui(&options.web_url())?;
    }

    let mut client = UdpClient::connect(options.endpoint(), options.secret())?;
    eprintln!("[client] connected UDP transport to {}", client.server());
    let origin = Instant::now();
    let mut macro_client = MacroClient::default();
    if let Some(path) = options.macro_file() {
        macro_client.load_file(path, elapsed_us(&origin))?;
    }

    let (sender, receiver) = mpsc::channel::<String>();
    if options.interactive() {
        thread::spawn(move || {
            let input = io::stdin();
            for line in input.lock().lines().map_while(Result::ok) {
                if sender.send(line).is_err() { break; }
            }
        });
        eprintln!("[client] type controller tokens such as 'A', 'UP', 'PLUS', or 'QUIT'");
    }

    let mut runtime = StreamRuntime::default();
    let mut manual = MultiReport::default();
    manual.pad_mut(0).expect("pad zero").input_mut().set_vendor(EXT_PAD_PRESENT);
    loop {
        while let Ok(command) = receiver.try_recv() {
            if command.trim().eq_ignore_ascii_case("QUIT") {
                let _ = client.disconnect();
                return Ok(());
            }
            apply_manual_command(&mut manual, &command);
        }
        let mut report = manual;
        if let Some(mut macro_report) = macro_client.report(elapsed_us(&origin)) {
            macro_report.set_vendor(macro_report.vendor() | EXT_PAD_PRESENT);
            *report.pad_mut(0).expect("pad zero").input_mut() = macro_report;
        }
        runtime.tick(&mut client, report)?;
        if let Some(rumble) = client.receive_rumble()? {
            let [low, high] = rumble.amplitudes();
            eprintln!(
                "[rumble] pad={} low={} high={} duration={}0ms",
                rumble.subpad(), low, high, rumble.duration_10ms()
            );
        }
    }
}

fn elapsed_us(origin: &Instant) -> u64 {
    u64::try_from(origin.elapsed().as_micros()).unwrap_or(u64::MAX)
}

fn apply_manual_command(report: &mut MultiReport, command: &str) {
    let pad = report.pad_mut(0).expect("pad zero");
    let input = pad.input_mut();
    input.reset();
    input.set_vendor(EXT_PAD_PRESENT);
    for token in command.split('+').map(str::trim) {
        match token.to_ascii_uppercase().as_str() {
            "A" => input.set_buttons(input.buttons() | BTN_A),
            "B" => input.set_buttons(input.buttons() | BTN_B),
            "X" => input.set_buttons(input.buttons() | BTN_X),
            "Y" => input.set_buttons(input.buttons() | BTN_Y),
            "L" => input.set_buttons(input.buttons() | BTN_L),
            "R" => input.set_buttons(input.buttons() | BTN_R),
            "ZL" => input.set_buttons(input.buttons() | BTN_ZL),
            "ZR" => input.set_buttons(input.buttons() | BTN_ZR),
            "PLUS" => input.set_buttons(input.buttons() | BTN_PLUS),
            "MINUS" => input.set_buttons(input.buttons() | BTN_MINUS),
            "HOME" => input.set_buttons(input.buttons() | BTN_HOME),
            "CAPTURE" => input.set_buttons(input.buttons() | BTN_CAPTURE),
            "UP" => input.set_hat(Hat::North),
            "DOWN" => input.set_hat(Hat::South),
            "LEFT" => input.set_hat(Hat::West),
            "RIGHT" => input.set_hat(Hat::East),
            _ => {}
        }
    }
}
