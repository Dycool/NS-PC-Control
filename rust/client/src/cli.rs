use ns_shared::protocol::{DEFAULT_PORT, DEFAULT_SECRET};
use std::path::PathBuf;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CliOptions {
    server: String,
    port: u16,
    secret: String,
    macro_file: Option<PathBuf>,
    open_web: bool,
    interactive: bool,
}

impl Default for CliOptions {
    fn default() -> Self {
        Self { server: "127.0.0.1".to_string(), port: DEFAULT_PORT, secret: DEFAULT_SECRET.to_string(), macro_file: None, open_web: false, interactive: true }
    }
}

impl CliOptions {
    pub fn parse(arguments: impl Iterator<Item = String>) -> Result<Self, String> {
        let mut options = Self::default();
        let mut arguments = arguments.peekable();
        while let Some(argument) = arguments.next() {
            match argument.as_str() {
                "--server" | "-s" => options.server = next_value(&mut arguments, "--server")?,
                "--port" | "-p" => options.port = next_value(&mut arguments, "--port")?.parse().map_err(|_| "--port expects a valid u16".to_string())?,
                "--secret" => options.secret = next_value(&mut arguments, "--secret")?,
                "--macro" => options.macro_file = Some(PathBuf::from(next_value(&mut arguments, "--macro")?)),
                "--open-web" => options.open_web = true,
                "--neutral" => options.interactive = false,
                "--help" | "-h" => return Err(help_text().to_string()),
                other => return Err(format!("unknown argument: {other}\n{}", help_text())),
            }
        }
        Ok(options)
    }
    pub fn endpoint(&self) -> String { format!("{}:{}", self.server, self.port) }
    pub fn web_url(&self) -> String { format!("http://{}:8080/", self.server) }
    pub fn secret(&self) -> &str { &self.secret }
    pub fn macro_file(&self) -> Option<&PathBuf> { self.macro_file.as_ref() }
    pub fn open_web(&self) -> bool { self.open_web }
    pub fn interactive(&self) -> bool { self.interactive }
}

pub fn help_text() -> &'static str {
    "ns-client [--server HOST] [--port PORT] [--secret SECRET] [--macro FILE] [--open-web] [--neutral]"
}

fn next_value(arguments: &mut impl Iterator<Item = String>, name: &str) -> Result<String, String> {
    arguments.next().ok_or_else(|| format!("{name} requires a value"))
}
