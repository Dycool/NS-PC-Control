#![forbid(unsafe_code)]

use ns_backend::app_state::{ServerContext, UsbControllerFamily};
use ns_backend::virtual_controller::{HidGadgetController, VirtualController};
use ns_backend::writers::write_once;
use ns_shared::crypto::derive_key;
use ns_shared::protocol::{
    Packet as InputPacket, DEFAULT_PORT, DEFAULT_SECRET, FLAG_DISCONNECT,
    PACKET_SIZE as INPUT_PACKET_SIZE,
};
use std::env;
use std::io;
use std::net::UdpSocket;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
struct Options {
    bind: String,
    port: u16,
    secret: String,
    family: UsbControllerFamily,
    hid_paths: Vec<PathBuf>,
    verbose: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            bind: "0.0.0.0".to_string(),
            port: DEFAULT_PORT,
            secret: DEFAULT_SECRET.to_string(),
            family: UsbControllerFamily::Switch1,
            hid_paths: Vec::new(),
            verbose: false,
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let options = parse_args(env::args().skip(1))
        .map_err(|message| io::Error::new(io::ErrorKind::InvalidInput, message))?;
    let context = Arc::new(ServerContext::default());
    context
        .set_family(options.family, 0)
        .map_err(|_| io::Error::other("cannot set initial USB family"))?;
    let socket = UdpSocket::bind((options.bind.as_str(), options.port))?;
    socket.set_nonblocking(true)?;
    let key = derive_key(&options.secret);
    let origin = Instant::now();
    let mut controllers: Vec<Box<dyn VirtualController>> = options
        .hid_paths
        .iter()
        .filter_map(|path| match HidGadgetController::open(path) {
            Ok(controller) => Some(Box::new(controller) as Box<dyn VirtualController>),
            Err(error) => {
                eprintln!("[usb] cannot open {}: {error}", path.display());
                None
            }
        })
        .collect();
    if options.verbose {
        eprintln!(
            "[server] Rust backend listening on {}:{} family={:?} hid_ports={}",
            options.bind,
            options.port,
            options.family,
            controllers.len()
        );
    }

    let mut packet_buffer = [0_u8; INPUT_PACKET_SIZE];
    let mut next_write = Instant::now();
    loop {
        let now_us = elapsed_us(&origin);
        match socket.recv_from(&mut packet_buffer) {
            Ok((size, address)) if size == INPUT_PACKET_SIZE => {
                if let Ok(packet) = InputPacket::decode_authenticated(&packet_buffer[..size], &key) {
                    let slot = match context.register_udp(address, now_us) {
                        Ok(slot) => slot,
                        Err(_) => continue,
                    };
                    if packet.flags() & FLAG_DISCONNECT != 0 {
                        let _ = context.disconnect(slot);
                        continue;
                    }
                    let _ = context.update_udp_report(
                        slot,
                        packet.sequence(),
                        *packet.report(),
                        now_us,
                    );
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(error) => return Err(Box::new(error)),
        }

        if Instant::now() >= next_write {
            context.expire_stale_clients(now_us);
            if !controllers.is_empty() {
                write_once(&context, &mut controllers, now_us)?;
            }
            next_write += Duration::from_millis(4);
            if next_write < Instant::now() {
                next_write = Instant::now();
            }
        } else {
            std::thread::sleep(Duration::from_micros(250));
        }
    }
}

fn elapsed_us(origin: &Instant) -> u64 {
    u64::try_from(origin.elapsed().as_micros()).unwrap_or(u64::MAX)
}

fn parse_args(arguments: impl Iterator<Item = String>) -> Result<Options, String> {
    let mut options = Options::default();
    let mut arguments = arguments.peekable();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--bind" => options.bind = take_value(&mut arguments, "--bind")?,
            "--port" | "-p" => {
                options.port = take_value(&mut arguments, "--port")?
                    .parse()
                    .map_err(|_| "--port expects a valid u16".to_string())?;
            }
            "--secret" => options.secret = take_value(&mut arguments, "--secret")?,
            "--family" => {
                options.family = match take_value(&mut arguments, "--family")?.as_str() {
                    "switch1" | "s1" => UsbControllerFamily::Switch1,
                    "switch2" | "s2" => UsbControllerFamily::Switch2,
                    "hori" => UsbControllerFamily::Hori,
                    other => return Err(format!("unknown family: {other}")),
                };
            }
            "--hid-path" => options
                .hid_paths
                .push(PathBuf::from(take_value(&mut arguments, "--hid-path")?)),
            "--verbose" | "-v" => options.verbose = true,
            "--help" | "-h" => {
                println!("ns-backend [--bind ADDR] [--port PORT] [--secret SECRET] [--family switch1|switch2|hori] [--hid-path PATH]... [-v]");
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok(options)
}

fn take_value(arguments: &mut impl Iterator<Item = String>, name: &str) -> Result<String, String> {
    arguments
        .next()
        .ok_or_else(|| format!("{name} requires a value"))
}
