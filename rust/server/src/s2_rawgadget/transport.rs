const USB_DIR_IN: u8 = 0x80;
const USB_TYPE_MASK: u8 = 0x60;
const USB_TYPE_STANDARD: u8 = 0x00;
const USB_TYPE_CLASS: u8 = 0x20;
const USB_TYPE_VENDOR: u8 = 0x40;
const USB_RECIP_MASK: u8 = 0x1f;
const USB_RECIP_DEVICE: u8 = 0x00;
const USB_RECIP_INTERFACE: u8 = 0x01;
const USB_RECIP_ENDPOINT: u8 = 0x02;

const USB_REQ_GET_STATUS: u8 = 0x00;
const USB_REQ_CLEAR_FEATURE: u8 = 0x01;
const USB_REQ_SET_FEATURE: u8 = 0x03;
const USB_REQ_SET_ADDRESS: u8 = 0x05;
const USB_REQ_GET_DESCRIPTOR: u8 = 0x06;
const USB_REQ_GET_CONFIGURATION: u8 = 0x08;
const USB_REQ_SET_CONFIGURATION: u8 = 0x09;
const USB_REQ_GET_INTERFACE: u8 = 0x0a;
const USB_REQ_SET_INTERFACE: u8 = 0x0b;
const USB_ENDPOINT_HALT: u16 = 0;
const USB_DEVICE_REMOTE_WAKEUP: u16 = 1;

const USB_DT_DEVICE: u8 = 0x01;
const USB_DT_CONFIG: u8 = 0x02;
const USB_DT_STRING: u8 = 0x03;
const USB_DT_HID: u8 = 0x21;
const USB_DT_REPORT: u8 = 0x22;

fn endpoint_bit(address: u8) -> u8 {
    match address {
        HID_OUT_ADDRESS => 1 << 0,
        HID_IN_ADDRESS => 1 << 1,
        VENDOR_OUT_ADDRESS => 1 << 2,
        VENDOR_IN_ADDRESS => 1 << 3,
        AUDIO_PLAYBACK_ADDRESS => 1 << 4,
        AUDIO_CAPTURE_ADDRESS => 1 << 5,
        _ => 0,
    }
}

fn endpoint_handle(inner: &Inner, address: u8) -> Option<EndpointHandle> {
    let state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
    match address {
        HID_OUT_ADDRESS => state.endpoints.hid_out,
        HID_IN_ADDRESS => state.endpoints.hid_in,
        VENDOR_OUT_ADDRESS => state.endpoints.vendor_out,
        VENDOR_IN_ADDRESS => state.endpoints.vendor_in,
        AUDIO_PLAYBACK_ADDRESS => state.endpoints.audio_out,
        AUDIO_CAPTURE_ADDRESS => state.endpoints.audio_in,
        _ => None,
    }
}

fn publish_audio_state(inner: &Inner) {
    let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
    if state.state < GadgetState::HidReady || state.state >= GadgetState::Resetting {
        return;
    }
    state.state = if state.audio_in_alt != 0 {
        GadgetState::AudioCaptureActive
    } else if state.audio_out_alt != 0 {
        GadgetState::AudioPlaybackActive
    } else {
        GadgetState::HidReady
    };
}

fn disable_all_endpoints(inner: &Inner) {
    let endpoints = {
        let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
        let endpoints = state.endpoints;
        state.endpoints = Endpoints::default();
        state.audio_out_alt = 0;
        state.audio_in_alt = 0;
        endpoints
    };
    for endpoint in [
        endpoints.audio_out,
        endpoints.audio_in,
        endpoints.hid_in,
        endpoints.hid_out,
        endpoints.vendor_out,
        endpoints.vendor_in,
    ]
    .into_iter()
    .flatten()
    {
        let _ = inner.gadget.disable_endpoint(endpoint);
    }
}

fn enable_base_endpoints(inner: &Inner) -> io::Result<()> {
    disable_all_endpoints(inner);
    let hid_in = inner.gadget.enable_endpoint(EndpointDescriptor::new(
        HID_IN_ADDRESS,
        0x03,
        64,
        4,
    ))?;
    let hid_out = match inner.gadget.enable_endpoint(EndpointDescriptor::new(
        HID_OUT_ADDRESS,
        0x03,
        64,
        4,
    )) {
        Ok(handle) => handle,
        Err(error) => {
            let _ = inner.gadget.disable_endpoint(hid_in);
            return Err(error);
        }
    };
    let vendor_out = match inner.gadget.enable_endpoint(EndpointDescriptor::new(
        VENDOR_OUT_ADDRESS,
        0x02,
        64,
        0,
    )) {
        Ok(handle) => handle,
        Err(error) => {
            let _ = inner.gadget.disable_endpoint(hid_in);
            let _ = inner.gadget.disable_endpoint(hid_out);
            return Err(error);
        }
    };
    let vendor_in = match inner.gadget.enable_endpoint(EndpointDescriptor::new(
        VENDOR_IN_ADDRESS,
        0x02,
        64,
        0,
    )) {
        Ok(handle) => handle,
        Err(error) => {
            let _ = inner.gadget.disable_endpoint(hid_in);
            let _ = inner.gadget.disable_endpoint(hid_out);
            let _ = inner.gadget.disable_endpoint(vendor_out);
            return Err(error);
        }
    };
    let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
    state.endpoints.hid_in = Some(hid_in);
    state.endpoints.hid_out = Some(hid_out);
    state.endpoints.vendor_out = Some(vendor_out);
    state.endpoints.vendor_in = Some(vendor_in);
    Ok(())
}

fn transition_audio_alt(inner: &Inner, interface: u16, alternate: u16) -> io::Result<()> {
    let capture = interface == u16::from(AUDIO_CAPTURE_INTERFACE);
    if alternate == 0 {
        let endpoint = {
            let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
            if capture {
                state.audio_in_alt = 0;
                state.endpoints.audio_in.take()
            } else {
                state.audio_out_alt = 0;
                state.endpoints.audio_out.take()
            }
        };
        if let Some(endpoint) = endpoint
            && let Err(error) = inner.gadget.disable_endpoint(endpoint)
        {
            let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
            if capture {
                state.endpoints.audio_in = Some(endpoint);
            } else {
                state.endpoints.audio_out = Some(endpoint);
            }
            if error.kind() != io::ErrorKind::Interrupted {
                publish_audio_state(inner);
                return Err(error);
            }
        }
        let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
        if capture {
            queues.mic_audio.clear();
        } else {
            queues.console_audio.clear();
        }
        drop(queues);
        publish_audio_state(inner);
        return Ok(());
    }

    let existing = endpoint_handle(
        inner,
        if capture {
            AUDIO_CAPTURE_ADDRESS
        } else {
            AUDIO_PLAYBACK_ADDRESS
        },
    );
    let endpoint = match existing {
        Some(endpoint) => endpoint,
        None => inner.gadget.enable_endpoint(EndpointDescriptor::new(
            if capture {
                AUDIO_CAPTURE_ADDRESS
            } else {
                AUDIO_PLAYBACK_ADDRESS
            },
            0x0d,
            S2_AUDIO_USB_FRAME_BYTES as u16,
            1,
        ))?,
    };
    {
        let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
        if capture {
            state.endpoints.audio_in = Some(endpoint);
            state.audio_in_alt = 1;
        } else {
            state.endpoints.audio_out = Some(endpoint);
            state.audio_out_alt = 1;
        }
    }
    publish_audio_state(inner);
    Ok(())
}

fn ep0_write_limited(inner: &Inner, data: &[u8], requested: u16) -> io::Result<()> {
    let length = usize::from(requested).min(data.len());
    inner.gadget.ep0_write(&data[..length]).map(|_| ())
}

fn ep0_read_exact_stage(inner: &Inner, requested: u16) -> io::Result<Vec<u8>> {
    let data = inner.gadget.ep0_read(usize::from(requested))?;
    if data.len() != usize::from(requested) {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "Raw Gadget EP0 OUT stage was shorter than wLength",
        ));
    }
    Ok(data)
}

fn ep0_stall(inner: &Inner) -> io::Result<()> {
    inner.gadget.ep0_stall()
}

fn handle_get_descriptor(inner: &Inner, control: ControlRequest) -> io::Result<()> {
    let descriptor_type = (control.value() >> 8) as u8;
    let descriptor_index = control.value() as u8;
    if control.request_type() & USB_RECIP_MASK == USB_RECIP_DEVICE {
        match descriptor_type {
            USB_DT_DEVICE if descriptor_index == 0 && control.index() == 0 => {
                return ep0_write_limited(inner, &device_descriptor(), control.length());
            }
            USB_DT_CONFIG if descriptor_index == 0 && control.index() == 0 => {
                return ep0_write_limited(inner, &config_descriptor(), control.length());
            }
            USB_DT_STRING => {
                if let Some(descriptor) = string_descriptor(descriptor_index, &inner.serial) {
                    return ep0_write_limited(inner, &descriptor, control.length());
                }
            }
            _ => {}
        }
    }
    if control.request_type() & USB_RECIP_MASK == USB_RECIP_INTERFACE && control.index() == 0 {
        match descriptor_type {
            USB_DT_REPORT => {
                return ep0_write_limited(inner, S2_PRO_REPORT_DESC, control.length());
            }
            USB_DT_HID => {
                return ep0_write_limited(inner, &hid_descriptor(), control.length());
            }
            _ => {}
        }
    }
    ep0_stall(inner)
}

fn handle_audio_class_request(inner: &Inner, control: ControlRequest) -> io::Result<bool> {
    let entity = (control.index() >> 8) as u8;
    let interface = control.index() as u8;
    let selector = (control.value() >> 8) as u8;
    let channel = control.value() as u8;
    if interface != 2 || !matches!(entity, 2 | 5) || channel != 0 {
        return Ok(false);
    }
    let input = control.request_type() & USB_DIR_IN != 0;
    let capture = entity == 5;
    if !input {
        let expected = match selector {
            0x01 => 1,
            0x02 => 2,
            _ => 0,
        };
        if control.request() != 0x01 || usize::from(control.length()) != expected || expected == 0 {
            ep0_stall(inner)?;
            return Ok(true);
        }
        let data = ep0_read_exact_stage(inner, control.length())?;
        let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
        let current = if capture { state.capture } else { state.playback };
        let updated = if selector == 0x01 {
            AudioControl::new(data[0] != 0, current.volume_1_256_db())
        } else {
            AudioControl::new(current.muted(), i16::from_le_bytes([data[0], data[1]]))
        };
        if capture {
            state.capture = updated;
        } else {
            state.playback = updated;
        }
        return Ok(true);
    }

    let current = {
        let state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
        if capture { state.capture } else { state.playback }
    };
    match selector {
        0x01 if control.length() == 1 => {
            let value = match control.request() {
                0x81 => u8::from(current.muted()),
                0x82 => 0,
                0x83 | 0x84 => 1,
                _ => {
                    ep0_stall(inner)?;
                    return Ok(true);
                }
            };
            ep0_write_limited(inner, &[value], control.length())?;
        }
        0x02 if control.length() == 2 => {
            let value = match control.request() {
                0x81 => current.volume_1_256_db(),
                0x82 => -0x2580,
                0x83 => 0,
                0x84 => 1,
                _ => {
                    ep0_stall(inner)?;
                    return Ok(true);
                }
            };
            ep0_write_limited(inner, &value.to_le_bytes(), control.length())?;
        }
        _ => ep0_stall(inner)?,
    }
    Ok(true)
}

fn handle_hid_class_request(inner: &Inner, control: ControlRequest) -> io::Result<bool> {
    let interface = control.index() as u8;
    let entity = (control.index() >> 8) as u8;
    if control.request_type() & USB_RECIP_MASK != USB_RECIP_INTERFACE || interface != 0 || entity != 0 {
        return Ok(false);
    }
    match control.request() {
        0x01 => {
            let zeros = vec![0u8; usize::from(control.length()).min(64)];
            ep0_write_limited(inner, &zeros, control.length())?;
        }
        0x02 if control.length() == 1 => {
            let idle = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).idle_rate;
            ep0_write_limited(inner, &[idle], 1)?;
        }
        0x03 if control.length() == 1 => {
            let protocol = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).protocol;
            ep0_write_limited(inner, &[protocol], 1)?;
        }
        0x09 if control.length() != 0 && control.length() <= 64 => {
            let payload = ep0_read_exact_stage(inner, control.length())?;
            let generation = inner.generation.load(Ordering::Acquire);
            let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
            while queues.output.len() >= QUEUE_LIMIT {
                queues.output.pop_front();
            }
            queues.output.push_back(QueuedReport { bytes: payload, generation });
        }
        0x0a if control.length() == 0 => {
            inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).idle_rate = (control.value() >> 8) as u8;
            let _ = ep0_read_exact_stage(inner, 0)?;
        }
        0x0b if control.length() == 0 && matches!(control.value(), 0 | 1) => {
            inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).protocol = control.value() as u8;
            let _ = ep0_read_exact_stage(inner, 0)?;
        }
        _ => ep0_stall(inner)?,
    }
    Ok(true)
}

fn handle_control(inner: &Inner, control: ControlRequest) -> io::Result<()> {
    let request_type = control.request_type();
    let request = control.request();
    let input = request_type & USB_DIR_IN != 0;
    let request_class = request_type & USB_TYPE_MASK;

    if request_class == USB_TYPE_VENDOR {
        match inner.native.handle_ep0(request_type, request, control.length()) {
            Some(Ep0Reply::Data(data)) if input => {
                return ep0_write_limited(inner, &data, control.length());
            }
            Some(Ep0Reply::StatusOnly) if !input => {
                let _ = ep0_read_exact_stage(inner, control.length())?;
                return Ok(());
            }
            _ => return ep0_stall(inner),
        }
    }

    if request_class == USB_TYPE_STANDARD && input && request == USB_REQ_GET_DESCRIPTOR {
        return handle_get_descriptor(inner, control);
    }

    if request_class == USB_TYPE_STANDARD && !input && request == USB_REQ_SET_CONFIGURATION {
        if request_type != USB_RECIP_DEVICE
            || control.value() > 1
            || control.index() != 0
            || control.length() != 0
        {
            inner.native.enumeration().request_reenumeration("malformed mandatory SET_CONFIGURATION request");
            return ep0_stall(inner);
        }
        let _ = ep0_read_exact_stage(inner, 0)?;
        if control.value() == 0 {
            disable_all_endpoints(inner);
            clear_connection_state(inner, GadgetState::Addressed);
            inner.native.enumeration().bus_reset();
            return Ok(());
        }
        inner.generation.fetch_add(1, Ordering::AcqRel);
        clear_queues(inner);
        inner.native.reset();
        enable_base_endpoints(inner)?;
        inner.gadget.configure()?;
        {
            let mut state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
            state.configuration = 1;
            state.remote_wakeup = false;
            state.halted_endpoints = 0;
            state.playback = AudioControl::default();
            state.capture = AudioControl::default();
            state.state = GadgetState::HidReady;
        }
        inner.native.enumeration().usb_configured();
        inner.native.enumeration().native_handshake();
        return Ok(());
    }

    if request_class == USB_TYPE_STANDARD && !input && request == USB_REQ_SET_ADDRESS {
        if control.value() > 127 || control.index() != 0 || control.length() != 0 {
            return ep0_stall(inner);
        }
        let _ = ep0_read_exact_stage(inner, 0)?;
        inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).state = GadgetState::Addressed;
        return Ok(());
    }

    if request_class == USB_TYPE_STANDARD && input && request == USB_REQ_GET_CONFIGURATION {
        if control.value() != 0 || control.index() != 0 || control.length() != 1 {
            return ep0_stall(inner);
        }
        let configured = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).configuration;
        return ep0_write_limited(inner, &[configured], 1);
    }

    if request_class == USB_TYPE_STANDARD && !input && request == USB_REQ_SET_INTERFACE {
        let ready = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).state >= GadgetState::HidReady;
        if !ready
            || !matches!(control.index(), 3 | 4)
            || control.value() > 1
            || control.length() != 0
        {
            return ep0_stall(inner);
        }
        let _ = ep0_read_exact_stage(inner, 0)?;
        return transition_audio_alt(inner, control.index(), control.value());
    }

    if request_class == USB_TYPE_STANDARD && input && request == USB_REQ_GET_INTERFACE {
        if control.value() != 0 || control.length() != 1 || !matches!(control.index(), 3 | 4) {
            return ep0_stall(inner);
        }
        let state = inner.state.lock().unwrap_or_else(|poison| poison.into_inner());
        let alternate = if control.index() == 3 { state.audio_out_alt } else { state.audio_in_alt };
        drop(state);
        return ep0_write_limited(inner, &[alternate], 1);
    }

    if request_class == USB_TYPE_STANDARD && input && request == USB_REQ_GET_STATUS {
        if control.value() != 0 || control.length() != 2 {
            return ep0_stall(inner);
        }
        let recipient = request_type & USB_RECIP_MASK;
        let status = if recipient == USB_RECIP_DEVICE && control.index() == 0 {
            let remote = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).remote_wakeup;
            0x0001 | if remote { 0x0002 } else { 0 }
        } else if recipient == USB_RECIP_ENDPOINT {
            let bit = endpoint_bit(control.index() as u8);
            if bit == 0 {
                return ep0_stall(inner);
            }
            let halted = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).halted_endpoints;
            u16::from(halted & bit != 0)
        } else if recipient == USB_RECIP_INTERFACE && control.index() <= 4 {
            0
        } else {
            return ep0_stall(inner);
        };
        return ep0_write_limited(inner, &status.to_le_bytes(), 2);
    }

    if request_class == USB_TYPE_STANDARD
        && !input
        && matches!(request, USB_REQ_SET_FEATURE | USB_REQ_CLEAR_FEATURE)
    {
        let recipient = request_type & USB_RECIP_MASK;
        if recipient == USB_RECIP_ENDPOINT && control.value() == USB_ENDPOINT_HALT && control.length() == 0 {
            let address = control.index() as u8;
            let bit = endpoint_bit(address);
            let Some(endpoint) = endpoint_handle(inner, address) else {
                return ep0_stall(inner);
            };
            if bit == 0 {
                return ep0_stall(inner);
            }
            if request == USB_REQ_SET_FEATURE {
                inner.gadget.set_halt(endpoint)?;
                inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).halted_endpoints |= bit;
            } else {
                inner.gadget.clear_halt(endpoint)?;
                inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).halted_endpoints &= !bit;
            }
        } else if recipient == USB_RECIP_DEVICE
            && control.value() == USB_DEVICE_REMOTE_WAKEUP
            && control.index() == 0
            && control.length() == 0
        {
            inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).remote_wakeup = request == USB_REQ_SET_FEATURE;
        } else {
            return ep0_stall(inner);
        }
        let _ = ep0_read_exact_stage(inner, 0)?;
        return Ok(());
    }

    if request_class == USB_TYPE_CLASS {
        if handle_audio_class_request(inner, control)? || handle_hid_class_request(inner, control)? {
            return Ok(());
        }
        return ep0_stall(inner);
    }

    ep0_stall(inner)
}

fn retime_motion_report(inner: &Inner, report: &mut [u8]) {
    let Some(report_id) = report.first().copied() else {
        return;
    };
    let (length_index, data_index) = match report_id {
        0x07 | 0x08 => (16, 17),
        0x09 => (15, 16),
        _ => return,
    };
    let generation = inner.generation.load(Ordering::Acquire);
    let mut timing = inner.motion.lock().unwrap_or_else(|poison| poison.into_inner());
    if timing.generation != generation {
        *timing = MotionTiming { generation, ..MotionTiming::default() };
    }
    if report.get(length_index).copied().unwrap_or(0) < 4 || report.len() < data_index + 2 {
        timing.last_write_us = 0;
        timing.fraction = 0;
        timing.last_report_id = report_id;
        return;
    }
    let write_us = u64::try_from(inner.origin.elapsed().as_micros()).unwrap_or(u64::MAX);
    let elapsed_ticks = if timing.last_write_us != 0
        && timing.last_report_id == report_id
        && write_us > timing.last_write_us
    {
        let delta = write_us - timing.last_write_us;
        let scaled = timing.fraction.saturating_add(delta.saturating_mul(800));
        let ticks = (scaled / 1_000_000).clamp(1, 15) as u16;
        timing.fraction = scaled % 1_000_000;
        ticks
    } else {
        timing.fraction = 0;
        3
    };
    timing.tick = timing.tick.wrapping_add(elapsed_ticks) & 0x0fff;
    let encoded = ((elapsed_ticks & 0x0f) << 12) | timing.tick;
    report[data_index..data_index + 2].copy_from_slice(&encoded.to_le_bytes());
    timing.last_write_us = write_us;
    timing.last_report_id = report_id;
}

fn push_report(queue: &mut VecDeque<QueuedReport>, bytes: Vec<u8>, generation: u64) {
    while queue.len() >= QUEUE_LIMIT {
        queue.pop_front();
    }
    queue.push_back(QueuedReport { bytes, generation });
}

fn spawn_workers(inner: &Arc<Inner>) -> Vec<JoinHandle<()>> {
    let functions: [fn(Arc<Inner>, usize); WORKER_COUNT] = [
        event_worker,
        hid_in_worker,
        hid_out_worker,
        vendor_in_worker,
        vendor_out_worker,
        audio_out_worker,
        audio_in_worker,
    ];
    functions
        .into_iter()
        .enumerate()
        .map(|(index, function)| {
            let inner = Arc::clone(inner);
            thread::spawn(move || function(inner, index))
        })
        .collect()
}

fn event_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    while inner.running.load(Ordering::Acquire) {
        match inner.gadget.next_event(512) {
            Ok(event) => match event.kind() {
                EventKind::Connect => {
                    inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).state = GadgetState::Connected;
                    inner.native.enumeration().bus_reset();
                }
                EventKind::Control => match event.control_request() {
                    Ok(control) => {
                        if let Err(error) = handle_control(&inner, control)
                            && error.kind() != io::ErrorKind::Interrupted
                        {
                            let _ = inner.native.enumeration().request_reenumeration("Raw Gadget EP0 request failed");
                        }
                    }
                    Err(_) => {
                        let _ = inner.gadget.ep0_stall();
                        let _ = inner.native.enumeration().request_reenumeration("truncated Raw Gadget control event");
                    }
                },
                EventKind::Suspend => {}
                EventKind::Resume => publish_audio_state(&inner),
                EventKind::Reset => {
                    disable_all_endpoints(&inner);
                    clear_connection_state(&inner, GadgetState::Resetting);
                    inner.native.enumeration().bus_reset();
                }
                EventKind::Disconnect => {
                    disable_all_endpoints(&inner);
                    clear_connection_state(&inner, GadgetState::DeviceInitialized);
                    inner.native.enumeration().bus_reset();
                }
                EventKind::Unknown(_) => {}
            },
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) if !inner.running.load(Ordering::Acquire) => break,
            Err(_) => {
                inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).state = GadgetState::Failed;
                let _ = inner.native.enumeration().request_reenumeration("Raw Gadget event fetch failed");
                inner.running.store(false, Ordering::Release);
                inner.input_cv.notify_all();
                inner.vendor_cv.notify_all();
                inner.audio_cv.notify_all();
                break;
            }
        }
    }
}

fn hid_in_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    while inner.running.load(Ordering::Acquire) {
        let queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
        let mut queues = inner
            .input_cv
            .wait_timeout_while(queues, Duration::from_millis(20), |queues| {
                queues.input.is_empty() && inner.running.load(Ordering::Acquire)
            })
            .unwrap_or_else(|poison| poison.into_inner())
            .0;
        if !inner.running.load(Ordering::Acquire) {
            break;
        }
        let Some(mut entry) = queues.input.pop_back() else {
            continue;
        };
        queues.input.clear();
        drop(queues);
        let generation = inner.generation.load(Ordering::Acquire);
        if entry.generation != generation {
            continue;
        }
        let Some(endpoint) = endpoint_handle(&inner, HID_IN_ADDRESS) else {
            continue;
        };
        retime_motion_report(&inner, &mut entry.bytes);
        let _ = inner.gadget.endpoint_write(endpoint, &entry.bytes);
    }
}

fn hid_out_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    while inner.running.load(Ordering::Acquire) {
        let Some(endpoint) = endpoint_handle(&inner, HID_OUT_ADDRESS) else {
            thread::sleep(Duration::from_millis(5));
            continue;
        };
        let generation = inner.generation.load(Ordering::Acquire);
        match inner.gadget.endpoint_read(endpoint, 64) {
            Ok(bytes) if !bytes.is_empty() && generation == inner.generation.load(Ordering::Acquire) => {
                let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
                push_report(&mut queues.output, bytes, generation);
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => thread::sleep(Duration::from_millis(2)),
        }
    }
}

fn vendor_out_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    while inner.running.load(Ordering::Acquire) {
        let Some(endpoint) = endpoint_handle(&inner, VENDOR_OUT_ADDRESS) else {
            thread::sleep(Duration::from_millis(5));
            continue;
        };
        let generation = inner.generation.load(Ordering::Acquire);
        match inner.gadget.endpoint_read(endpoint, 512) {
            Ok(bytes) if !bytes.is_empty() && generation == inner.generation.load(Ordering::Acquire) => {
                let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
                if queues.vendor_output.iter().any(|entry| entry.generation == generation && entry.bytes == bytes) {
                    continue;
                }
                push_report(&mut queues.vendor_output, bytes, generation);
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => thread::sleep(Duration::from_millis(2)),
        }
    }
}

fn vendor_in_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    while inner.running.load(Ordering::Acquire) {
        let queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
        let mut queues = inner
            .vendor_cv
            .wait_timeout_while(queues, Duration::from_millis(20), |queues| {
                queues.vendor_input.is_empty() && inner.running.load(Ordering::Acquire)
            })
            .unwrap_or_else(|poison| poison.into_inner())
            .0;
        if !inner.running.load(Ordering::Acquire) {
            break;
        }
        let Some(entry) = queues.vendor_input.front().cloned() else {
            continue;
        };
        drop(queues);
        if entry.generation != inner.generation.load(Ordering::Acquire) {
            remove_vendor_reply(&inner, &entry);
            continue;
        }
        let Some(endpoint) = endpoint_handle(&inner, VENDOR_IN_ADDRESS) else {
            remove_vendor_reply(&inner, &entry);
            continue;
        };
        let mut sent = false;
        for _ in 0..50 {
            if !inner.running.load(Ordering::Acquire)
                || entry.generation != inner.generation.load(Ordering::Acquire)
            {
                break;
            }
            match inner.gadget.endpoint_write(endpoint, &entry.bytes) {
                Ok(_) => {
                    sent = true;
                    break;
                }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(1));
                }
                Err(_) => break,
            }
        }
        remove_vendor_reply(&inner, &entry);
        if !sent && inner.running.load(Ordering::Acquire) {
            let _ = inner.native.enumeration().request_reenumeration("native vendor response endpoint write failed");
        }
    }
}

fn remove_vendor_reply(inner: &Inner, expected: &VendorReply) {
    let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
    if queues.vendor_input.front().is_some_and(|entry| {
        entry.generation == expected.generation
            && entry.request == expected.request
            && entry.bytes == expected.bytes
    }) {
        queues.vendor_input.pop_front();
    }
}

fn audio_out_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    let mut pending = Vec::with_capacity(S2_AUDIO_USB_FRAME_BYTES * 2);
    while inner.running.load(Ordering::Acquire) {
        let active = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).audio_out_alt != 0;
        let Some(endpoint) = endpoint_handle(&inner, AUDIO_PLAYBACK_ADDRESS).filter(|_| active) else {
            pending.clear();
            thread::sleep(Duration::from_millis(20));
            continue;
        };
        let generation = inner.generation.load(Ordering::Acquire);
        match inner.gadget.endpoint_read(endpoint, S2_AUDIO_USB_FRAME_BYTES) {
            Ok(bytes) if !bytes.is_empty() && generation == inner.generation.load(Ordering::Acquire) => {
                pending.extend_from_slice(&bytes);
                while pending.len() >= S2_AUDIO_USB_FRAME_BYTES {
                    let frame: [u8; S2_AUDIO_USB_FRAME_BYTES] = pending[..S2_AUDIO_USB_FRAME_BYTES]
                        .try_into()
                        .expect("audio frame slice has the fixed USB frame size");
                    pending.drain(..S2_AUDIO_USB_FRAME_BYTES);
                    let mut queues = inner.queues.lock().unwrap_or_else(|poison| poison.into_inner());
                    while queues.console_audio.len() >= QUEUE_LIMIT {
                        queues.console_audio.pop_front();
                    }
                    queues.console_audio.push_back(frame);
                    inner.audio_cv.notify_one();
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => {
                pending.clear();
                thread::sleep(Duration::from_millis(2));
            }
        }
    }
}

fn audio_in_worker(inner: Arc<Inner>, index: usize) {
    register_worker(&inner, index);
    let silence = [0u8; S2_AUDIO_USB_FRAME_BYTES];
    let mut next_frame = Instant::now();
    while inner.running.load(Ordering::Acquire) {
        let active = inner.state.lock().unwrap_or_else(|poison| poison.into_inner()).audio_in_alt != 0;
        let Some(endpoint) = endpoint_handle(&inner, AUDIO_CAPTURE_ADDRESS).filter(|_| active) else {
            next_frame = Instant::now();
            thread::sleep(Duration::from_millis(20));
            continue;
        };
        let generation = inner.generation.load(Ordering::Acquire);
        let frame = inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .mic_audio
            .pop_front()
            .unwrap_or(silence);
        if generation != inner.generation.load(Ordering::Acquire) {
            continue;
        }
        next_frame += Duration::from_millis(1);
        let _ = inner.gadget.endpoint_write(endpoint, &frame);
        let now = Instant::now();
        if next_frame + Duration::from_millis(4) < now {
            next_frame = now;
        }
        if next_frame > now {
            thread::sleep(next_frame - now);
        }
    }
}

#[cfg(test)]
mod transport_tests {
    use super::*;

    #[test]
    fn endpoint_bits_cover_every_switch2_endpoint() {
        let addresses = [
            HID_OUT_ADDRESS,
            HID_IN_ADDRESS,
            VENDOR_OUT_ADDRESS,
            VENDOR_IN_ADDRESS,
            AUDIO_PLAYBACK_ADDRESS,
            AUDIO_CAPTURE_ADDRESS,
        ];
        let mut mask = 0u8;
        for address in addresses {
            let bit = endpoint_bit(address);
            assert_ne!(bit, 0);
            assert_eq!(mask & bit, 0);
            mask |= bit;
        }
        assert_eq!(mask, 0x3f);
    }

    #[test]
    fn descriptors_fit_raw_gadget_endpoint_contracts() {
        assert_eq!(device_descriptor().len(), 18);
        assert!(config_descriptor().len() > 9);
        assert_eq!(hid_descriptor().len(), 9);
        assert!(!S2_PRO_REPORT_DESC.is_empty());
    }
}
