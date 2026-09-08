use std::error::Error;

use x11rb::{connection::Connection, protocol::xproto::ConnectionExt, rust_connection::RustConnection};

pub fn refresh(connection: &RustConnection, root: u32) -> Result<(), Box<dyn Error>> {
	connection.get_geometry(root)?.reply()?;
	Ok(())
}
