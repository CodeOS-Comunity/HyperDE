use std::error::Error;

use x11rb::{protocol::xproto::ConnectionExt, rust_connection::RustConnection};

pub fn create(connection: &RustConnection, root: u32, command: &str) -> Result<(), Box<dyn Error>> {
	connection.get_geometry(root)?.reply()?;
	if command.trim().is_empty() {
		return Err("launcher_command cannot be empty".into());
	}
	Ok(())
}
