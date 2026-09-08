use std::error::Error;

use crate::ui::panels::PANEL_HEIGHT;
use x11rb::{protocol::xproto::ConnectionExt, rust_connection::RustConnection};

pub fn refresh(connection: &RustConnection, root: u32) -> Result<(), Box<dyn Error>> {
	let geometry = connection.get_geometry(root)?.reply()?;
	let _panel_height = PANEL_HEIGHT.min(geometry.height);
	Ok(())
}
