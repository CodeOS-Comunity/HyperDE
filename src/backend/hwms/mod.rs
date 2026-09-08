use std::{collections::HashMap, error::Error};

use penrose::{
    core::{bindings::{KeyBindings, MouseBindings}, WindowManager},
    x11rb::RustConn,
};

use crate::config::HyperdeConfig;

pub fn run(configuration: &HyperdeConfig) -> Result<(), Box<dyn Error>> {
    let connection = RustConn::new()?;
    let key_bindings: KeyBindings<RustConn> = HashMap::new();
    let mouse_bindings: MouseBindings<RustConn> = HashMap::new();
    let window_manager = WindowManager::new(configuration.penrose_config()?, key_bindings, mouse_bindings, connection)?;
    window_manager.run()?;
    Ok(())
}