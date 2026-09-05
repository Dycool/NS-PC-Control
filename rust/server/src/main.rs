#![forbid(unsafe_code)]

fn main() -> Result<(), Box<dyn std::error::Error>> {
    ns_backend::server_runtime::run()
}
