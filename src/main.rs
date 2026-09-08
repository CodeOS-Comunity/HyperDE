mod backend;
mod ui;

use std::{env, process::ExitCode};

fn print_usage() {
	eprintln!("usage: hyperde <chroma|hwms>");
}

fn main() -> ExitCode {
	let mut arguments = env::args().skip(1);
	let result = match arguments.next().as_deref() {
		Some("chroma") => backend::chroma::run(),
		Some("hwms") => backend::hwms::run(),
		Some("help" | "--help" | "-h") => {
			print_usage();
			Ok(())
		}
		_ => {
			print_usage();
			Err("a component must be selected")
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
