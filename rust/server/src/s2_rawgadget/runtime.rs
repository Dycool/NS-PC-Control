const QUEUE_LIMIT: usize = 32;
const WORKER_COUNT: usize = 7;
pub const REENUMERATION_DISCONNECT_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Ord, PartialOrd)]
pub enum GadgetState {
    #[default]
    Stopped,
    DeviceInitialized,
    Connected,
    Addressed,
    Configured,
    HidReady,
    AudioPlaybackActive,
    AudioCaptureActive,
    Resetting,
    Failed,
}

#[derive(Clone, Debug)]
pub struct RawGadgetConfiguration {
    device_path: PathBuf,
    udc_root: PathBuf,
    legacy_gadget_dir: PathBuf,
    serial: String,
}

impl Default for RawGadgetConfiguration {
    fn default() -> Self {
        Self {
            device_path: PathBuf::from("/dev/raw-gadget"),
            udc_root: PathBuf::from("/sys/class/udc"),
            legacy_gadget_dir: PathBuf::from("/sys/kernel/config/usb_gadget/ns_ctrl"),
            serial: "000000000000".to_owned(),
        }
    }
}

impl RawGadgetConfiguration {
    #[must_use]
    pub fn new(
        device_path: impl Into<PathBuf>,
        udc_root: impl Into<PathBuf>,
        legacy_gadget_dir: impl Into<PathBuf>,
        serial: impl Into<String>,
    ) -> Self {
        Self {
            device_path: device_path.into(),
            udc_root: udc_root.into(),
            legacy_gadget_dir: legacy_gadget_dir.into(),
            serial: serial.into(),
        }
    }

    #[must_use]
    pub fn device_path(&self) -> &Path {
        &self.device_path
    }

    #[must_use]
    pub fn udc_root(&self) -> &Path {
        &self.udc_root
    }

    #[must_use]
    pub fn serial(&self) -> &str {
        &self.serial
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct Endpoints {
    hid_in: Option<EndpointHandle>,
    hid_out: Option<EndpointHandle>,
    vendor_out: Option<EndpointHandle>,
    vendor_in: Option<EndpointHandle>,
    audio_out: Option<EndpointHandle>,
    audio_in: Option<EndpointHandle>,
}

#[derive(Clone, Copy, Debug)]
struct RuntimeState {
    state: GadgetState,
    endpoints: Endpoints,
    configuration: u8,
    audio_out_alt: u8,
    audio_in_alt: u8,
    remote_wakeup: bool,
    halted_endpoints: u8,
    idle_rate: u8,
    protocol: u8,
    playback: AudioControl,
    capture: AudioControl,
}

impl Default for RuntimeState {
    fn default() -> Self {
        Self {
            state: GadgetState::Stopped,
            endpoints: Endpoints::default(),
            configuration: 0,
            audio_out_alt: 0,
            audio_in_alt: 0,
            remote_wakeup: false,
            halted_endpoints: 0,
            idle_rate: 0,
            protocol: 1,
            playback: AudioControl::default(),
            capture: AudioControl::default(),
        }
    }
}

#[derive(Clone, Debug)]
struct QueuedReport {
    bytes: Vec<u8>,
    generation: u64,
}

#[derive(Clone, Debug)]
struct VendorReply {
    bytes: Vec<u8>,
    request: Vec<u8>,
    generation: u64,
}

#[derive(Default)]
struct VendorReplyQueue(VecDeque<VendorReply>);

impl VendorReplyQueue {
    fn iter(&self) -> impl Iterator<Item = &VendorReply> {
        self.0.iter()
    }

    fn len(&self) -> usize {
        self.0.len()
    }

    fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    fn push_back(&mut self, value: VendorReply) {
        self.0.push_back(value);
    }

    fn pop_front(&mut self) -> Option<VendorReply> {
        self.0.pop_front()
    }

    fn front(&mut self) -> Option<&VendorReply> {
        let _ = self.0.make_contiguous();
        self.0.front()
    }
}

#[derive(Default)]
struct Queues {
    input: VecDeque<QueuedReport>,
    output: VecDeque<QueuedReport>,
    vendor_output: VecDeque<QueuedReport>,
    vendor_input: VendorReplyQueue,
    console_audio: VecDeque<[u8; S2_AUDIO_USB_FRAME_BYTES]>,
    mic_audio: VecDeque<[u8; S2_AUDIO_USB_FRAME_BYTES]>,
}

#[derive(Default)]
struct MotionTiming {
    tick: u16,
    fraction: u64,
    last_write_us: u64,
    last_report_id: u8,
    generation: u64,
}

struct Inner {
    gadget: Arc<RawGadget>,
    native: Arc<NativeController>,
    running: AtomicBool,
    generation: AtomicU64,
    state: Mutex<RuntimeState>,
    queues: Mutex<Queues>,
    input_cv: Condvar,
    vendor_cv: Condvar,
    audio_cv: Condvar,
    worker_tokens: Mutex<[Option<ThreadInterruptToken>; WORKER_COUNT]>,
    motion: Mutex<MotionTiming>,
    serial: String,
    origin: Instant,
}

pub struct RawGadgetRuntime {
    inner: Arc<Inner>,
    workers: Mutex<Vec<JoinHandle<()>>>,
}

impl RawGadgetRuntime {
    pub fn setup(configuration: &RawGadgetConfiguration) -> io::Result<Self> {
        ensure_raw_gadget_device(configuration.device_path())?;
        unbind_legacy_gadget(&configuration.legacy_gadget_dir)?;
        let udc = first_udc_name(configuration.udc_root())?;
        install_interrupt_handler()?;
        let gadget = Arc::new(RawGadget::open(
            configuration.device_path(),
            &udc,
            &udc,
            UsbSpeed::Full,
        )?);
        let native = Arc::new(NativeController::default());
        native.reset();
        native.enumeration().gadget_started();
        let inner = Arc::new(Inner {
            gadget,
            native,
            running: AtomicBool::new(true),
            generation: AtomicU64::new(1),
            state: Mutex::new(RuntimeState {
                state: GadgetState::DeviceInitialized,
                ..RuntimeState::default()
            }),
            queues: Mutex::new(Queues::default()),
            input_cv: Condvar::new(),
            vendor_cv: Condvar::new(),
            audio_cv: Condvar::new(),
            worker_tokens: Mutex::new([None; WORKER_COUNT]),
            motion: Mutex::new(MotionTiming::default()),
            serial: configuration.serial.clone(),
            origin: Instant::now(),
        });
        let workers = spawn_workers(&inner);
        Ok(Self {
            inner,
            workers: Mutex::new(workers),
        })
    }

    #[must_use]
    pub fn module_available(path: &Path) -> bool {
        path.exists() || command_success("modprobe", &["-n", "raw_gadget"])
    }

    #[must_use]
    pub fn nodes_ready(&self) -> bool {
        self.state() >= GadgetState::HidReady
    }

    #[must_use]
    pub fn transport_active(&self) -> bool {
        self.inner.running.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn io_ready(&self) -> bool {
        self.nodes_ready() && self.transport_active()
    }

    #[must_use]
    pub fn host_enabled(&self) -> bool {
        self.state() >= GadgetState::Configured && self.state() < GadgetState::Resetting
    }

    #[must_use]
    pub fn state(&self) -> GadgetState {
        self.inner
            .state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .state
    }

    #[must_use]
    pub fn native(&self) -> Arc<NativeController> {
        Arc::clone(&self.inner.native)
    }

    pub fn submit_input_report(&self, report: &[u8]) -> bool {
        if !self.io_ready() || report.is_empty() || report.len() > 64 {
            return false;
        }
        let generation = self.inner.generation.load(Ordering::Acquire);
        let mut queues = self
            .inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        queues.input.clear();
        queues.input.push_back(QueuedReport {
            bytes: report.to_vec(),
            generation,
        });
        self.inner.input_cv.notify_one();
        true
    }

    pub fn poll_output_report(&self) -> Option<Vec<u8>> {
        let generation = self.inner.generation.load(Ordering::Acquire);
        pop_current(
            &mut self
                .inner
                .queues
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .output,
            generation,
        )
    }

    pub fn drain_output(&self) {
        self.inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .output
            .clear();
    }

    pub fn poll_vendor_report(&self) -> Option<Vec<u8>> {
        let generation = self.inner.generation.load(Ordering::Acquire);
        pop_current(
            &mut self
                .inner
                .queues
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .vendor_output,
            generation,
        )
    }

    pub fn submit_vendor_report(&self, report: &[u8], request: &[u8]) -> bool {
        if !self.io_ready() || report.is_empty() {
            return false;
        }
        let generation = self.inner.generation.load(Ordering::Acquire);
        let mut queues = self
            .inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        if queues
            .vendor_input
            .iter()
            .any(|entry| entry.generation == generation && entry.request == request)
        {
            return true;
        }
        while queues.vendor_input.len() >= QUEUE_LIMIT {
            queues.vendor_input.pop_front();
        }
        queues.vendor_input.push_back(VendorReply {
            bytes: report.to_vec(),
            request: request.to_vec(),
            generation,
        });
        self.inner.vendor_cv.notify_one();
        true
    }

    pub fn pop_console_audio(
        &self,
        timeout: Duration,
    ) -> Option<[u8; S2_AUDIO_USB_FRAME_BYTES]> {
        let queues = self
            .inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let mut queues = self
            .inner
            .audio_cv
            .wait_timeout_while(queues, timeout, |queues| {
                queues.console_audio.is_empty() && self.transport_active()
            })
            .unwrap_or_else(|poison| poison.into_inner())
            .0;
        queues.console_audio.pop_front()
    }

    pub fn queue_microphone_audio(&self, data: &[u8]) -> bool {
        if data.is_empty()
            || !data.len().is_multiple_of(S2_AUDIO_USB_FRAME_BYTES)
            || !self.transport_active()
        {
            return false;
        }
        let mut queues = self
            .inner
            .queues
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let (frames, remainder) = data.as_chunks::<S2_AUDIO_USB_FRAME_BYTES>();
        debug_assert!(remainder.is_empty());
        for frame in frames {
            while queues.mic_audio.len() >= QUEUE_LIMIT {
                queues.mic_audio.pop_front();
            }
            queues.mic_audio.push_back(*frame);
        }
        true
    }

    #[must_use]
    pub fn playback_control(&self) -> AudioControl {
        self.inner
            .state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .playback
    }

    #[must_use]
    pub fn capture_control(&self) -> AudioControl {
        self.inner
            .state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .capture
    }

    pub fn teardown(&self) {
        if !self.inner.running.swap(false, Ordering::AcqRel) {
            return;
        }
        self.inner.input_cv.notify_all();
        self.inner.vendor_cv.notify_all();
        self.inner.audio_cv.notify_all();
        interrupt_all_workers(&self.inner);
        disable_all_endpoints(&self.inner);
        interrupt_all_workers(&self.inner);
        let mut workers = self
            .workers
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        for worker in workers.drain(..) {
            let _ = worker.join();
        }
        clear_connection_state(&self.inner, GadgetState::Stopped);
    }
}

impl Drop for RawGadgetRuntime {
    fn drop(&mut self) {
        self.teardown();
    }
}

fn pop_current(queue: &mut VecDeque<QueuedReport>, generation: u64) -> Option<Vec<u8>> {
    while let Some(entry) = queue.pop_front() {
        if entry.generation == generation {
            return Some(entry.bytes);
        }
    }
    None
}

fn clear_queues(inner: &Inner) {
    *inner
        .queues
        .lock()
        .unwrap_or_else(|poison| poison.into_inner()) = Queues::default();
}

fn clear_connection_state(inner: &Inner, state: GadgetState) {
    inner.generation.fetch_add(1, Ordering::AcqRel);
    clear_queues(inner);
    inner.native.reset();
    let mut runtime = inner
        .state
        .lock()
        .unwrap_or_else(|poison| poison.into_inner());
    runtime.state = state;
    runtime.configuration = 0;
    runtime.audio_out_alt = 0;
    runtime.audio_in_alt = 0;
    runtime.remote_wakeup = false;
    runtime.halted_endpoints = 0;
    runtime.playback = AudioControl::default();
    runtime.capture = AudioControl::default();
}

fn ensure_raw_gadget_device(path: &Path) -> io::Result<()> {
    if path.exists() {
        return Ok(());
    }
    let _ = Command::new("modprobe").arg("raw_gadget").status();
    if path.exists() {
        return Ok(());
    }
    if let Some(module) = raw_gadget_module_candidate() {
        let status = Command::new("insmod").arg(&module).status()?;
        if status.success() && path.exists() {
            return Ok(());
        }
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!(
            "{} is unavailable after loading raw_gadget",
            path.display()
        ),
    ))
}

fn raw_gadget_module_candidate() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("NS_RAW_GADGET_MODULE")
        .map(PathBuf::from)
        .filter(|path| path.exists())
    {
        return Some(path);
    }
    let release = Command::new("uname")
        .arg("-r")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())?;
    let release = release.trim();
    [
        PathBuf::from(format!(
            "/usr/lib/ns-pc-control/raw-gadget/{release}/raw_gadget.ko"
        )),
        PathBuf::from(format!(
            "/usr/local/lib/ns-pc-control/raw-gadget/{release}/raw_gadget.ko"
        )),
        PathBuf::from(format!(
            "server/prebuilt-raw-gadget/raw_gadget-{release}.ko"
        )),
    ]
    .into_iter()
    .find(|path| path.exists())
}

fn unbind_legacy_gadget(gadget_dir: &Path) -> io::Result<()> {
    let udc = gadget_dir.join("UDC");
    if udc.exists() {
        fs::write(udc, "")?;
    }
    Ok(())
}

fn first_udc_name(root: &Path) -> io::Result<String> {
    let mut names = fs::read_dir(root)?
        .filter_map(Result::ok)
        .filter_map(|entry| entry.file_name().into_string().ok())
        .collect::<Vec<_>>();
    names.sort();
    names.into_iter().next().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "no USB device controller is available",
        )
    })
}

fn command_success(program: &str, arguments: &[&str]) -> bool {
    Command::new(program)
        .args(arguments)
        .status()
        .is_ok_and(|status| status.success())
}

fn interrupt_all_workers(inner: &Inner) {
    let tokens = *inner
        .worker_tokens
        .lock()
        .unwrap_or_else(|poison| poison.into_inner());
    for token in tokens.into_iter().flatten() {
        let _ = interrupt_thread(token);
    }
}

fn interrupt_worker(inner: &Inner, index: usize) {
    if let Some(token) = inner
        .worker_tokens
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .get(index)
        .copied()
        .flatten()
    {
        let _ = interrupt_thread(token);
    }
}

fn register_worker(inner: &Inner, index: usize) {
    if let Some(slot) = inner
        .worker_tokens
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .get_mut(index)
    {
        *slot = Some(current_thread_interrupt_token());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn configuration_defaults_to_real_raw_gadget_paths() {
        let config = RawGadgetConfiguration::default();
        assert_eq!(config.device_path(), Path::new("/dev/raw-gadget"));
        assert_eq!(config.udc_root(), Path::new("/sys/class/udc"));
    }

    #[test]
    fn generation_filter_drops_stale_reports() {
        let mut queue = VecDeque::from([
            QueuedReport {
                bytes: vec![1],
                generation: 1,
            },
            QueuedReport {
                bytes: vec![2],
                generation: 2,
            },
        ]);
        assert_eq!(pop_current(&mut queue, 2), Some(vec![2]));
    }
}
