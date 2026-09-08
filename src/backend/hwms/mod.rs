use std::{collections::HashMap, error::Error};

use penrose::{
    core::{bindings::{KeyEventHandler, MouseEventHandler, MouseState}, Config, WindowManager},
    x11rb::RustConn,
};

pub fn run() -> Result<(), Box<dyn Error>> {
    let connection = RustConn::new()?;
    let key_bindings: HashMap<String, Box<dyn KeyEventHandler<RustConn>>> = HashMap::new();
    let mouse_bindings: HashMap<MouseState, Box<dyn MouseEventHandler<RustConn>>> = HashMap::new();
    let window_manager = WindowManager::new(Config::default(), key_bindings, mouse_bindings, connection)?;
    window_manager.run()?;
    Ok(())
}