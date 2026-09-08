use std::{env, error::Error, fs, path::PathBuf};

use penrose::{core::Config, x11rb::RustConn, Color};
use serde::Deserialize;

#[derive(Debug, Deserialize)]
#[serde(default)]
pub struct HyperdeConfig {
	pub compositor: CompositorConfig,
	pub window_manager: WindowManagerConfig,
}

#[derive(Debug, Deserialize)]
#[serde(default)]
pub struct CompositorConfig {
	pub panel_height: u16,
	pub launcher_command: String,
}

#[derive(Debug, Deserialize)]
#[serde(default)]
pub struct WindowManagerConfig {
	pub workspaces: Vec<String>,
	pub normal_border: String,
	pub focused_border: String,
	pub border_width: u32,
	pub focus_follow_mouse: bool,
	pub floating_classes: Vec<String>,
}

impl Default for HyperdeConfig {
	fn default() -> Self {
		Self {
			compositor: CompositorConfig::default(),
			window_manager: WindowManagerConfig::default(),
		}
	}
}

impl Default for CompositorConfig {
	fn default() -> Self {
		Self { panel_height: 28, launcher_command: String::from("hyperde-launcher") }
	}
}

impl Default for WindowManagerConfig {
	fn default() -> Self {
		Self {
			workspaces: (1..=9).map(|number| number.to_string()).collect(),
			normal_border: String::from("#3c3836ff"),
			focused_border: String::from("#cc241dff"),
			border_width: 2,
			focus_follow_mouse: true,
			floating_classes: vec![String::from("dmenu"), String::from("dunst")],
		}
	}
}

impl HyperdeConfig {
	pub fn penrose_config(&self) -> Result<Config<RustConn>, Box<dyn Error>> {
		let mut config = Config::default();
		config.tags = self.window_manager.workspaces.clone();
		config.normal_border = Color::try_from(self.window_manager.normal_border.as_str())?;
		config.focused_border = Color::try_from(self.window_manager.focused_border.as_str())?;
		config.border_width = self.window_manager.border_width;
		config.focus_follow_mouse = self.window_manager.focus_follow_mouse;
		config.floating_classes = self.window_manager.floating_classes.clone();
		Ok(config)
	}
}

pub fn load() -> Result<HyperdeConfig, Box<dyn Error>> {
	let path = env::var_os("HYPERDE_CONFIG").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("hyperde.toml"));
	if !path.exists() {
		return Ok(HyperdeConfig::default());
	}

	let contents = fs::read_to_string(&path)?;
	Ok(toml::from_str(&contents)?)
}