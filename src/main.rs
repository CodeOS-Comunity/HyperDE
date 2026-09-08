mod backend;
mod config;
mod ui;

use std::{env, error::Error, process::ExitCode};

fn print_usage() {
	eprintln!("usage: hyperde <chroma|hwms>\n\nSet HYPERDE_CONFIG to use a different TOML file.");
}

fn main() -> ExitCode {
	let mut arguments = env::args().skip(1);
	let configuration = match config::load() {
		Ok(configuration) => configuration,
		Err(error) => {
			eprintln!("hyperde: could not load configuration: {error}");
			return ExitCode::FAILURE;
		}
	};
	let result = match arguments.next().as_deref() {
		Some("chroma") => backend::chroma::run(&configuration),
		Some("hwms") => backend::hwms::run(&configuration),
		Some("help" | "--help" | "-h") => {
			print_usage();
			Ok(())
		}
		_ => {
			print_usage();
			Err::<(), Box<dyn Error>>("a component must be selected".into())
		}
	};

	match result {
		Ok(()) => ExitCode::SUCCESS,
		Err(error) => {
			eprintln!("hyperde: {error}");
			ExitCode::FAILURE
		}
	}
}
