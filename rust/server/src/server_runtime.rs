use crate::app_state::{
    requested_virtual_slots, ServerContext, UsbControllerFamily, MAX_CLIENTS,
};
use crate::controller_profiles::report_requests_pair;
use crate::s2_audio_bridge::S2AudioBridge;
use crate::s2_rawgadget::{RawGadgetConfiguration, REENUMERATION_DISCONNECT_INTERVAL};
use crate::s2_reports::S2ReportContext;
use crate::s2_service::{S2LiveService, S2TickOutcome};
use crate::server_control::{
    configured_client_capacity, free_virtual_slot_count, inspect_control_datagram, ControlDatagram,
};
use crate::udp_feedback::flush_feedback_to_udp;
use crate::virtual_controller::{HidGadgetController, VirtualController};
use crate::web_server::WebServer;
use crate::writers::write_once;
use ns_shared::control_packets::ClientAssignmentPacket;
use ns_shared::crypto::derive_key;
use ns_shared::protocol::{
    ControllerType, MultiReport, Packet as InputPacket, DEFAULT_PORT, DEFAULT_SECRET,
    CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED, CLIENT_ASSIGNMENT_FLAG_SERVER_FULL,
    CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP, CONTROLLER_CONSOLE_PORT_NONE,
    CONTROLLER_PLAYER_INDEX_UNKNOWN, EXT_PAD_PRESENT, FLAG_DISCONNECT,
    PACKET_SIZE as INPUT_PACKET_SIZE, S2_AUDIO_PORT_OFFSET,
};
use std::collections::HashMap;
use std::env;
use std::io;
use std::net::{IpAddr, SocketAddr, UdpSocket};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const DEFAULT_WEB_PORT: u16 = 8080;
const USB_WRITE_PERIOD: Duration = Duration::from_millis(4);

#[derive(Clone, Debug)]
struct Options {
    bind: String,
    port: u16,
    secret: String,
    family: UsbControllerFamily,
    hid_paths: Vec<PathBuf>,
    web_port: Option<u16>,
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
            web_port: None,
            verbose: false,
        }
    }
}

struct InputReceiveState<'a> {
    options: &'a Options,
    packet_buffer: &'a mut [u8; 65_535],
    endpoint_slots: &'a mut HashMap<SocketAddr, usize>,
    pending_restart: &'a mut Option<UsbControllerFamily>,
}

pub fn run() -> Result<(), Box<dyn std::error::Error>> {
    let original_arguments = env::args().skip(1).collect::<Vec<_>>();
    let options = parse_args(original_arguments.clone().into_iter())
        .map_err(|message| io::Error::new(io::ErrorKind::InvalidInput, message))?;
    let context = Arc::new(ServerContext::default());
    context
        .set_family(options.family, 0)
        .map_err(|_| io::Error::other("cannot set initial USB family"))?;

    let socket = UdpSocket::bind((options.bind.as_str(), options.port))?;
    socket.set_nonblocking(true)?;
    let key = derive_key(&options.secret);
    let origin = Instant::now();
    let web_running = Arc::new(AtomicBool::new(true));
    let web_thread = start_web_server(&options, Arc::clone(&context), Arc::clone(&web_running));

    let raw_configuration = RawGadgetConfiguration::default();
    let mut s2_service = if options.family == UsbControllerFamily::Switch2 {
        Some(S2LiveService::setup(&raw_configuration)?)
    } else {
        None
    };
    let audio_port = options.port.wrapping_add(S2_AUDIO_PORT_OFFSET);
    let mut s2_audio = if s2_service.is_some() {
        Some(S2AudioBridge::bind(&options.bind, audio_port, key)?)
    } else {
        None
    };
    let mut controllers: Vec<Box<dyn VirtualController>> = if s2_service.is_none() {
        options
            .hid_paths
            .iter()
            .filter_map(|path| match HidGadgetController::open(path) {
                Ok(controller) => Some(Box::new(controller) as Box<dyn VirtualController>),
                Err(error) => {
                    eprintln!("[usb] cannot open {}: {error}", path.display());
                    None
                }
            })
            .collect()
    } else {
        Vec::new()
    };

    println!(
        "Started ns-backend server on {}:{}{}{}",
        options.bind,
        options.port,
        if options.family == UsbControllerFamily::Switch2 {
            " (Switch 2 native Raw Gadget)"
        } else if options.family == UsbControllerFamily::Hori {
            " (HORI USB identity)"
        } else {
            ""
        },
        if options.verbose { " (verbose)" } else { "" }
    );
    if options.verbose {
        eprintln!("[server] USB writer cadence: 250 Hz (4 ms)");
        if s2_audio.is_some() {
            eprintln!(
                "[s2][audio] dedicated UDP bridge ready on input port + {}",
                S2_AUDIO_PORT_OFFSET
            );
        }
        if let Some(port) = options.web_port {
            eprintln!("[web] HTTP webapp + WebSocket input listening on port {port}");
        }
    }

    let mut packet_buffer = [0_u8; 65_535];
    let mut endpoint_slots = HashMap::<SocketAddr, usize>::new();
    let mut next_write = Instant::now();
    let mut pending_restart = None;

    while context.is_running() {
        let now_us = elapsed_us(&origin);
        receive_input_datagrams(
            &socket,
            &context,
            &key,
            InputReceiveState {
                options: &options,
                packet_buffer: &mut packet_buffer,
                endpoint_slots: &mut endpoint_slots,
                pending_restart: &mut pending_restart,
            },
            now_us,
        )?;

        if let (Some(service), Some(audio)) = (s2_service.as_ref(), s2_audio.as_mut()) {
            let active_pro_ips = active_udp_pro_ips(&context, &endpoint_slots, now_us);
            audio.tick(service.runtime(), &active_pro_ips, now_us)?;
        }

        if Instant::now() >= next_write {
            // Do not mutate a live session merely because its controls are
            // stale. The C++ writer neutralizes a snapshot at 350 ms while
            // retaining requested controller identity until the 30 s session
            // timeout. snapshot()/active_client_count()/register_udp() already
            // enforce that hard timeout from last_rx_us.
            endpoint_slots.retain(|_, slot| {
                context
                    .snapshot(*slot, now_us)
                    .is_some_and(|snapshot| snapshot.active())
            });

            if let Some(service) = s2_service.as_mut() {
                let active_pro_ips = active_udp_pro_ips(&context, &endpoint_slots, now_us);
                let headset_state = s2_audio.as_mut().map_or(0, |audio| {
                    audio.headset_state(service.report_timer(), &active_pro_ips, now_us)
                });
                let outcome = service.tick(
                    &context,
                    now_us,
                    S2ReportContext {
                        headset_state,
                        ..S2ReportContext::default()
                    },
                );
                match outcome {
                    S2TickOutcome::Submitted => context.record_hid_write(),
                    S2TickOutcome::ReenumerateForIdentity { profile } => {
                        reenumerate_s2(service, &raw_configuration, profile)?;
                    }
                    S2TickOutcome::ReenumerateForProtocol => {
                        let profile = service.live_profile();
                        reenumerate_s2(service, &raw_configuration, profile)?;
                    }
                    S2TickOutcome::UnsupportedPair { client_index, .. } => {
                        let _ = context.disconnect(client_index);
                        endpoint_slots.retain(|_, slot| *slot != client_index);
                    }
                    S2TickOutcome::Idle => {}
                }
            } else if !controllers.is_empty() {
                write_once(&context, &mut controllers, now_us)?;
            }

            for client_index in 0..MAX_CLIENTS {
                if let Err(error) = flush_feedback_to_udp(&socket, &context, client_index, now_us)
                    && options.verbose
                {
                    eprintln!(
                        "[udp] feedback send failed for slot {}: {error}",
                        client_index + 1
                    );
                }
            }
            next_write += USB_WRITE_PERIOD;
            if next_write < Instant::now() {
                next_write = Instant::now();
            }
        } else {
            thread::sleep(Duration::from_micros(250));
        }
    }

    if let Some(service) = s2_service.as_ref() {
        service.teardown();
    }
    web_running.store(false, Ordering::Release);
    if let Some(thread) = web_thread {
        let _ = thread.join();
    }
    if let Some(family) = pending_restart {
        restart_with_family(family, &original_arguments)?;
    }
    Ok(())
}

fn receive_input_datagrams(
    socket: &UdpSocket,
    context: &ServerContext,
    key: &[u8; 32],
    state: InputReceiveState<'_>,
    now_us: u64,
) -> io::Result<()> {
    let InputReceiveState {
        options,
        packet_buffer,
        endpoint_slots,
        pending_restart,
    } = state;
    loop {
        match socket.recv_from(packet_buffer) {
            Ok((size, address)) => {
                let bytes = &packet_buffer[..size];
                let requester_slot = endpoint_slots.get(&address).copied().filter(|slot| {
                    context
                        .snapshot(*slot, now_us)
                        .is_some_and(|snapshot| snapshot.active())
                });
                if let Some(control) =
                    inspect_control_datagram(context, key, bytes, requester_slot, now_us)
                {
                    if let ControlDatagram::Reply { payload, restart } = control {
                        socket.send_to(&payload, address)?;
                        if let Some(family) = restart {
                            *pending_restart = Some(family);
                            context.stop();
                        }
                    }
                    continue;
                }
                if size != INPUT_PACKET_SIZE {
                    continue;
                }
                let Ok(packet) = InputPacket::decode_authenticated(bytes, key) else {
                    continue;
                };
                let mut report = *packet.report();
                if options.family == UsbControllerFamily::Switch2 {
                    for subpad in 1..4 {
                        report.pad_mut(subpad).expect("fixed subpad").reset();
                    }
                }

                if packet.flags() & FLAG_DISCONNECT != 0 {
                    if let Some(slot) = requester_slot {
                        let _ = context.disconnect(slot);
                    }
                    endpoint_slots.remove(&address);
                    continue;
                }

                if options.family == UsbControllerFamily::Switch2
                    && report_requests_pair(report.pad(0).expect("P1 exists"))
                {
                    send_rejection(
                        socket,
                        address,
                        context,
                        now_us,
                        CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED,
                    )?;
                    if let Some(slot) = requester_slot {
                        let _ = context.disconnect(slot);
                    }
                    endpoint_slots.remove(&address);
                    continue;
                }

                let slot = if let Some(slot) = requester_slot {
                    slot
                } else {
                    let required_slots = requested_slots(&report);
                    let active_clients = context.active_client_count(now_us);
                    let free_slots = free_virtual_slot_count(context, now_us);
                    if active_clients >= configured_client_capacity(context)
                        || required_slots > free_slots
                    {
                        send_rejection(
                            socket,
                            address,
                            context,
                            now_us,
                            CLIENT_ASSIGNMENT_FLAG_SERVER_FULL,
                        )?;
                        continue;
                    }
                    let Ok(slot) = context.register_udp(address, now_us) else {
                        send_rejection(
                            socket,
                            address,
                            context,
                            now_us,
                            CLIENT_ASSIGNMENT_FLAG_SERVER_FULL,
                        )?;
                        continue;
                    };
                    endpoint_slots.insert(address, slot);
                    slot
                };

                if context
                    .update_udp_report(slot, packet.sequence(), report, now_us)
                    .is_ok()
                {
                    let _ = context.enable_udp_feedback(slot);
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
            Err(error) => return Err(error),
        }
    }
}

fn active_udp_pro_ips(
    context: &ServerContext,
    endpoint_slots: &HashMap<SocketAddr, usize>,
    now_us: u64,
) -> Vec<IpAddr> {
    endpoint_slots
        .iter()
        .filter_map(|(endpoint, slot)| {
            let snapshot = context.snapshot(*slot, now_us)?;
            if !snapshot.active() {
                return None;
            }
            let profile = snapshot.report().pads()[0].requested_profile_raw();
            (profile == ControllerType::Pro as u8 || profile == ControllerType::ProS2 as u8)
                .then_some(endpoint.ip())
        })
        .collect()
}

fn start_web_server(
    options: &Options,
    context: Arc<ServerContext>,
    running: Arc<AtomicBool>,
) -> Option<JoinHandle<()>> {
    let port = options.web_port?;
    let verbose = options.verbose;
    Some(thread::spawn(move || {
        let server = WebServer::new("webapp", port, context);
        if let Err(error) = server.serve(running)
            && verbose
        {
            eprintln!("[web] server stopped: {error}");
        }
    }))
}

fn reenumerate_s2(
    service: &mut S2LiveService,
    configuration: &RawGadgetConfiguration,
    profile: ControllerType,
) -> io::Result<()> {
    service.teardown();
    thread::sleep(REENUMERATION_DISCONNECT_INTERVAL);
    *service = S2LiveService::setup_for_profile(configuration, profile)?;
    Ok(())
}

fn requested_slots(report: &MultiReport) -> usize {
    let mut any_present = false;
    let mut slots = 0usize;
    for pad in report.pads() {
        let present = pad.input().vendor() & EXT_PAD_PRESENT != 0;
        any_present |= present;
        slots = slots.saturating_add(requested_virtual_slots(pad, present));
    }
    if any_present {
        slots
    } else {
        requested_virtual_slots(&report.pads()[0], true)
    }
}

fn send_rejection(
    socket: &UdpSocket,
    address: SocketAddr,
    context: &ServerContext,
    now_us: u64,
    reason_flag: u8,
) -> io::Result<()> {
    let sleeping = context.poll_switch2_sleep(now_us);
    let flags = reason_flag
        | if sleeping {
            CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP
        } else {
            0
        };
    let active_clients = context.active_client_count(now_us);
    let packet = ClientAssignmentPacket::new(
        flags,
        [
            CONTROLLER_PLAYER_INDEX_UNKNOWN,
            0,
            0,
            CONTROLLER_CONSOLE_PORT_NONE,
        ],
        [ControllerType::Default, ControllerType::Default],
        [
            u8::try_from(active_clients.min(configured_client_capacity(context)))
                .unwrap_or(u8::MAX),
            u8::try_from(configured_client_capacity(context)).unwrap_or(u8::MAX),
            u8::try_from(free_virtual_slot_count(context, now_us)).unwrap_or(u8::MAX),
        ],
    );
    socket.send_to(&packet.encode(), address)?;
    Ok(())
}

fn restart_with_family(
    family: UsbControllerFamily,
    original_arguments: &[String],
) -> io::Result<()> {
    let mut arguments = Vec::new();
    let mut index = 0usize;
    while index < original_arguments.len() {
        match original_arguments[index].as_str() {
            "--s2" | "-s2" | "--hori" | "-hori" => index += 1,
            "--family" => index += 2,
            _ => {
                arguments.push(original_arguments[index].clone());
                index += 1;
            }
        }
    }
    match family {
        UsbControllerFamily::Switch1 => {}
        UsbControllerFamily::Switch2 => arguments.push("--s2".to_owned()),
        UsbControllerFamily::Hori => arguments.push("--hori".to_owned()),
    }

    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        let executable = env::current_exe()?;
        let error = std::process::Command::new(executable).args(arguments).exec();
        Err(error)
    }
    #[cfg(not(unix))]
    {
        let _ = family;
        let _ = arguments;
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "backend family restart is only supported on Unix",
        ))
    }
}

fn elapsed_us(origin: &Instant) -> u64 {
    u64::try_from(origin.elapsed().as_micros()).unwrap_or(u64::MAX)
}

fn parse_args(arguments: impl Iterator<Item = String>) -> Result<Options, String> {
    let mut options = Options::default();
    let mut arguments = arguments.peekable();
    while let Some(mut argument) = arguments.next() {
        argument = match argument.as_str() {
            "-s2" => "--s2".to_owned(),
            "-hori" => "--hori".to_owned(),
            other => other.to_owned(),
        };
        match argument.as_str() {
            "-no-bt" | "--no-bt" => {
                return Err("-no-bt was removed; Bluetooth controller input is disabled by default. Use --bt to enable it.".to_owned());
            }
            "-p" => {
                return Err("-p was removed; use -b PORT or -b ADDR:PORT instead".to_owned());
            }
            "-b" => {
                let value = take_value(&mut arguments, "-b")?;
                parse_bind_arg(&value, &mut options.bind, &mut options.port)?;
            }
            "--bind" => options.bind = take_value(&mut arguments, "--bind")?,
            "--port" => {
                options.port = take_value(&mut arguments, "--port")?
                    .parse()
                    .map_err(|_| "--port expects a valid u16".to_string())?;
            }
            "--secret" => options.secret = take_value(&mut arguments, "--secret")?,
            "--family" => {
                options.family = parse_family(&take_value(&mut arguments, "--family")?)?;
            }
            "--s2" => {
                if options.family == UsbControllerFamily::Hori {
                    return Err("--hori and --s2 are mutually exclusive".to_owned());
                }
                options.family = UsbControllerFamily::Switch2;
            }
            "--hori" => {
                if options.family == UsbControllerFamily::Switch2 {
                    return Err("--hori and --s2 are mutually exclusive".to_owned());
                }
                options.family = UsbControllerFamily::Hori;
            }
            "-w" => {
                let port = if arguments
                    .peek()
                    .is_some_and(|value| !value.starts_with('-'))
                {
                    take_value(&mut arguments, "-w")?
                        .parse::<u16>()
                        .map_err(|_| "invalid web port value".to_owned())?
                } else {
                    DEFAULT_WEB_PORT
                };
                options.web_port = Some(port);
            }
            "--hid-path" => options
                .hid_paths
                .push(PathBuf::from(take_value(&mut arguments, "--hid-path")?)),
            "--verbose" | "-v" => options.verbose = true,
            "--help" | "-h" => {
                println!(
                    "ns-backend - Switch Input Server\n\n  -b ADDR[:PORT]|PORT  Bind authenticated UDP input\n  -v                   Enable verbose output\n  --s2                  Use Switch 2 native Raw Gadget identity\n  --hori                Use legacy HORI USB identity\n  -w [PORT]             Serve browser webapp + WebSocket input (default 8080)"
                );
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok(options)
}

fn parse_family(value: &str) -> Result<UsbControllerFamily, String> {
    match value {
        "switch1" | "s1" => Ok(UsbControllerFamily::Switch1),
        "switch2" | "s2" => Ok(UsbControllerFamily::Switch2),
        "hori" => Ok(UsbControllerFamily::Hori),
        other => Err(format!("unknown family: {other}")),
    }
}

fn parse_bind_arg(raw: &str, bind: &mut String, port: &mut u16) -> Result<(), String> {
    if raw.is_empty() {
        return Err("invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT".to_owned());
    }
    if raw.bytes().all(|byte| byte.is_ascii_digit()) {
        *port = raw
            .parse::<u16>()
            .map_err(|_| "invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT".to_owned())?;
        *bind = "0.0.0.0".to_owned();
        return Ok(());
    }
    let mut address = raw;
    if let Some((candidate, candidate_port)) = raw.rsplit_once(':') {
        *port = candidate_port
            .parse::<u16>()
            .map_err(|_| "invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT".to_owned())?;
        address = candidate;
    }
    *bind = if address.is_empty() {
        "0.0.0.0".to_owned()
    } else {
        address.to_owned()
    };
    Ok(())
}

fn take_value(arguments: &mut impl Iterator<Item = String>, name: &str) -> Result<String, String> {
    arguments
        .next()
        .ok_or_else(|| format!("{name} requires a value"))
}
