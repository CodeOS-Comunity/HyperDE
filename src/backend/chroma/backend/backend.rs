use std::error::Error;

use x11rb::{connection::Connection, rust_connection::RustConnection};

use super::connector::ChromaConnection;

pub fn run() -> Result<(), Box<dyn Error>> {
	let (connection, screen_number) = RustConnection::connect(None)?;
	let chroma = ChromaConnection::new(connection, screen_number)?;
	chroma.run()?;
	Ok(())
}
