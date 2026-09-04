#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OperationResult {
    code: AmiiboLibraryResult,
    tag_size: u16,
    detail: String,
}

impl OperationResult {
    fn new(code: AmiiboLibraryResult, tag_size: usize, detail: impl Into<String>) -> Self {
        Self {
            code,
            tag_size: u16::try_from(tag_size).unwrap_or(u16::MAX),
            detail: detail.into(),
        }
    }

    fn ok(tag_size: usize) -> Self {
        Self::new(AmiiboLibraryResult::Ok, tag_size, "")
    }

    #[must_use]
    pub const fn code(&self) -> AmiiboLibraryResult {
        self.code
    }

    #[must_use]
    pub const fn tag_size(&self) -> u16 {
        self.tag_size
    }

    #[must_use]
    pub fn detail(&self) -> &str {
        &self.detail
    }

    #[must_use]
    pub const fn is_ok(&self) -> bool {
        matches!(self.code, AmiiboLibraryResult::Ok)
    }
}

#[derive(Default)]
struct LibraryState {
    selected_ids: [Option<String>; 4],
    pending_v3_read_prefix: Option<Signature>,
}

fn library_state() -> &'static Mutex<LibraryState> {
    static STATE: OnceLock<Mutex<LibraryState>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(LibraryState::default()))
}

fn consume_pending_v3_read_prefix(output: &mut Signature) -> bool {
    let mut state = library_state()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let Some(prefix) = state.pending_v3_read_prefix.take() else {
        return false;
    };
    *output = prefix;
    true
}

fn ensure_resolver_registered() {
    static REGISTERED: OnceLock<()> = OnceLock::new();
    REGISTERED.get_or_init(|| {
        set_v3_read_prefix_resolver(Some(consume_pending_v3_read_prefix));
    });
}

#[cfg(test)]
fn test_data_root() -> &'static Mutex<Option<PathBuf>> {
    static ROOT: OnceLock<Mutex<Option<PathBuf>>> = OnceLock::new();
    ROOT.get_or_init(|| Mutex::new(None))
}

fn library_root() -> PathBuf {
    #[cfg(test)]
    {
        if let Some(root) = test_data_root()
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
        {
            return root.join("amiibo");
        }
    }
    if let Some(configured) = std::env::var_os("NS_PC_CONTROL_DATA_DIR") {
        if !configured.is_empty() {
            return PathBuf::from(configured).join("amiibo");
        }
    }
    PathBuf::from("/var/lib/ns-pc-control/amiibo")
}

fn tag_id(head: u32, tail: u32) -> String {
    format!("{head:08x}{tail:08x}")
}

fn tag_path(id: &str) -> PathBuf {
    library_root().join("tags").join(format!("{id}.bin"))
}

fn ensure_private_directory(path: &Path) -> Result<(), String> {
    fs::create_dir_all(path)
        .map_err(|error| format!("cannot create {}: {error}", path.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|error| format!("cannot restrict {}: {error}", path.display()))?;
    }
    Ok(())
}

fn atomic_write(path: &Path, data: &[u8]) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| format!("{} has no parent directory", path.display()))?;
    ensure_private_directory(parent)?;
    let temp = path.with_extension("bin.tmp");
    {
        let mut output = OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(&temp)
            .map_err(|error| format!("cannot open {}: {error}", temp.display()))?;
        output
            .write_all(data)
            .map_err(|error| format!("cannot write {}: {error}", temp.display()))?;
        output
            .sync_all()
            .map_err(|error| format!("cannot sync {}: {error}", temp.display()))?;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&temp, fs::Permissions::from_mode(0o600))
            .map_err(|error| format!("cannot restrict {}: {error}", temp.display()))?;
    }
    match fs::rename(&temp, path) {
        Ok(()) => Ok(()),
        Err(first_error) => {
            if path.exists() {
                fs::remove_file(path)
                    .map_err(|error| format!("cannot replace {} after {first_error}: {error}", path.display()))?;
                fs::rename(&temp, path)
                    .map_err(|error| format!("cannot replace {}: {error}", path.display()))
            } else {
                let _ = fs::remove_file(&temp);
                Err(format!("cannot replace {}: {first_error}", path.display()))
            }
        }
    }
}

fn read_file(path: &Path) -> Option<Vec<u8>> {
    fs::read(path).ok()
}

fn load_retail_key() -> Result<[u8; RETAIL_KEY_SIZE], String> {
    let mut candidates = Vec::new();
    if let Some(configured) = std::env::var_os("NS_AMIIBO_RETAIL_KEY") {
        if !configured.is_empty() {
            candidates.push(PathBuf::from(configured));
        }
    }
    candidates.push(library_root().join("key_retail.bin"));
    for path in candidates {
        let Some(bytes) = read_file(&path) else {
            continue;
        };
        if !validate_key(&bytes) {
            return Err(format!("{} is not a valid 160-byte key_retail.bin", path.display()));
        }
        return Ok(bytes
            .try_into()
            .expect("validated retail key is exactly 160 bytes"));
    }
    Err(format!(
        "Format Amiibo requires a valid 160-byte key_retail.bin at {} or NS_AMIIBO_RETAIL_KEY",
        library_root().join("key_retail.bin").display()
    ))
}

fn read_le32(bytes: &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(..4)?.try_into().ok()?))
}

fn template_bundle_candidates() -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(configured) = std::env::var_os("NS_AMIIBO_TEMPLATE_BUNDLE") {
        if !configured.is_empty() {
            candidates.push(PathBuf::from(configured));
        }
    }
    candidates.push(library_root().join("amiibo_templates.bin"));
    candidates
}

fn parse_template_bundle(bundle: &[u8], head: u32, tail: u32) -> Option<Vec<u8>> {
    const HEADER_SIZE: usize = 12;
    const ENTRY_SIZE: usize = 16;
    if bundle.len() < HEADER_SIZE
        || &bundle[..4] != b"NSAT"
        || bundle[4] != 1
        || bundle[5] != 0
        || bundle[6] != ENTRY_SIZE as u8
        || bundle[7] != 0
    {
        return None;
    }
    let count = usize::try_from(read_le32(&bundle[8..12])?).ok()?;
    if count > (bundle.len() - HEADER_SIZE) / ENTRY_SIZE {
        return None;
    }
    for index in 0..count {
        let start = HEADER_SIZE + index * ENTRY_SIZE;
        let entry = &bundle[start..start + ENTRY_SIZE];
        if read_le32(entry)? != head || read_le32(&entry[4..])? != tail {
            continue;
        }
        let offset = usize::try_from(read_le32(&entry[8..])?).ok()?;
        let length = usize::try_from(read_le32(&entry[12..])?).ok()?;
        if !is_supported_amiibo_dump_size(length)
            || offset > bundle.len()
            || length > bundle.len() - offset
        {
            return None;
        }
        return Some(bundle[offset..offset + length].to_vec());
    }
    None
}

fn bundled_template(head: u32, tail: u32) -> Option<Vec<u8>> {
    for path in template_bundle_candidates() {
        let Some(bundle) = read_file(&path) else {
            continue;
        };
        if let Some(template) = parse_template_bundle(&bundle, head, tail) {
            return Some(template);
        }
    }
    None
}

fn factory_template(head: u32, tail: u32, fallback_template: &[u8]) -> Option<Vec<u8>> {
    bundled_template(head, tail).or_else(|| {
        is_supported_amiibo_dump_size(fallback_template.len()).then(|| fallback_template.to_vec())
    })
}

fn stage_v3_read_prefix(state: &mut LibraryState, tag: &[u8]) -> OperationResult {
    state.pending_v3_read_prefix = None;
    if tag.len() != V3_SIZE {
        return OperationResult::ok(tag.len());
    }
    let prefix: [u8; 32] = tag[V3_SRAM_OFFSET..V3_SRAM_OFFSET + 32]
        .try_into()
        .expect("V3 read prefix is exactly 32 bytes");
    state.pending_v3_read_prefix = Some(Signature::from_bytes(prefix));
    OperationResult::ok(tag.len())
}

#[must_use]
pub fn generate_template(head: u32, tail: u32, retail_key: &[u8]) -> (OperationResult, Vec<u8>) {
    ensure_resolver_registered();
    if (head == 0 && tail == 0) || !validate_key(retail_key) {
        return (
            OperationResult::new(
                AmiiboLibraryResult::InvalidRequest,
                0,
                "invalid Amiibo ID or 160-byte build key",
            ),
            Vec::new(),
        );
    }
    let key: &[u8; RETAIL_KEY_SIZE] = retail_key
        .try_into()
        .expect("validated retail key is exactly 160 bytes");
    match generate_tag(head, tail, key) {
        Ok(tag) => (OperationResult::ok(tag.len()), tag),
        Err(error) => (
            OperationResult::new(AmiiboLibraryResult::GenerationError, 0, error),
            Vec::new(),
        ),
    }
}

#[must_use]
pub fn select(
    head: u32,
    tail: u32,
    console_port: usize,
    fallback_template: &[u8],
) -> (OperationResult, Vec<u8>) {
    ensure_resolver_registered();
    let mut state = library_state()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    state.pending_v3_read_prefix = None;
    let format_requested = tail & FORMAT_REQUEST_FLAG != 0;
    let tail = tail & !FORMAT_REQUEST_FLAG;
    if console_port >= state.selected_ids.len() || (head == 0 && tail == 0) {
        return (
            OperationResult::new(AmiiboLibraryResult::InvalidRequest, 0, "invalid Amiibo selection"),
            Vec::new(),
        );
    }
    let id = tag_id(head, tail);
    let path = tag_path(&id);

    if format_requested {
        let retail_key = match load_retail_key() {
            Ok(key) => key,
            Err(error) => {
                return (
                    OperationResult::new(AmiiboLibraryResult::GenerationError, 0, error),
                    Vec::new(),
                );
            }
        };
        let generated = match generate_tag(head, tail, &retail_key) {
            Ok(tag) => tag,
            Err(error) => {
                return (
                    OperationResult::new(AmiiboLibraryResult::GenerationError, 0, error),
                    Vec::new(),
                );
            }
        };
        let prefix = stage_v3_read_prefix(&mut state, &generated);
        if !prefix.is_ok() {
            return (prefix, Vec::new());
        }
        if let Err(error) = atomic_write(&path, &generated) {
            state.pending_v3_read_prefix = None;
            return (
                OperationResult::new(AmiiboLibraryResult::StorageError, 0, error),
                Vec::new(),
            );
        }
        state.selected_ids[console_port] = Some(id);
        return (OperationResult::ok(generated.len()), generated);
    }

    if let Some(stored) = read_file(&path).filter(|tag| is_supported_amiibo_dump_size(tag.len())) {
        let prefix = stage_v3_read_prefix(&mut state, &stored);
        if !prefix.is_ok() {
            return (prefix, Vec::new());
        }
        state.selected_ids[console_port] = Some(id);
        return (OperationResult::ok(stored.len()), stored);
    }

    let Some(factory) = factory_template(head, tail, fallback_template) else {
        return (
            OperationResult::new(
                AmiiboLibraryResult::GenerationError,
                0,
                "this build has no factory template for the selected Amiibo",
            ),
            Vec::new(),
        );
    };
    let prefix = stage_v3_read_prefix(&mut state, &factory);
    if !prefix.is_ok() {
        return (prefix, Vec::new());
    }
    if let Err(error) = atomic_write(&path, &factory) {
        state.pending_v3_read_prefix = None;
        return (
            OperationResult::new(AmiiboLibraryResult::StorageError, 0, error),
            Vec::new(),
        );
    }
    state.selected_ids[console_port] = Some(id);
    (OperationResult::ok(factory.len()), factory)
}

#[must_use]
pub fn clear() -> OperationResult {
    ensure_resolver_registered();
    let mut state = library_state()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let root = library_root();
    if let Err(error) = fs::remove_dir_all(&root) {
        if error.kind() != std::io::ErrorKind::NotFound {
            return OperationResult::new(
                AmiiboLibraryResult::StorageError,
                0,
                format!("cannot clear {}: {error}", root.display()),
            );
        }
    }
    state.selected_ids.fill(None);
    state.pending_v3_read_prefix = None;
    OperationResult::ok(0)
}

pub fn store_writeback(console_port: usize, data: &[u8]) -> Result<(), String> {
    ensure_resolver_registered();
    let state = library_state()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let Some(Some(id)) = state.selected_ids.get(console_port) else {
        return Err("no persistent Amiibo is selected for this controller".to_owned());
    };
    if !is_supported_amiibo_dump_size(data.len()) {
        return Err("no persistent Amiibo is selected for this controller".to_owned());
    }
    atomic_write(&tag_path(id), data)
}
