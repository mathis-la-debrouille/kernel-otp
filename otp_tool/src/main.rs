use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;

use clap::{Parser, Subcommand, ValueEnum};
use colored::Colorize;

/// System configuration tool
#[derive(Debug, Parser)]
#[clap(version, about, long_about = None)]
pub struct Config {
    #[clap(subcommand)]
    action: Action,
}

#[derive(Debug, Subcommand, PartialEq, Eq)]
enum Action {
    /// Show system status
    Show,

    /// Update device count
    UpdateCount {
        /// System path
        count: u8,
    },

    /// Modify device configuration
    Modify {
        /// System path
        path: String,
        /// Configuration type
        #[arg(value_enum)]
        config: ConfigType,
    },
    
    /// Update security settings
    UpdateSecurity {
        /// Security parameters
        #[clap(required = true)]
        params: Vec<String>,
    },

    /// Display security settings
    DisplaySecurity,

    /// Get system token
    GetToken {
        /// System path
        path: String,
    },

    /// Verify system token
    Verify {
        /// System path
        path: String,
        /// Token value
        token: String,
    }
}

#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Debug)]
enum ConfigType {
    Static,
    Dynamic,
}

const SYS_PATH: &str = "/proc/otp";
const COUNT_PATH: &str = "/sys/module/otp/parameters/devices";
const SECURITY_PATH: &str = "/sys/module/otp/parameters/pwd_list";

#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Debug)]
enum AccessMode {
    Input,
    Output,
}

macro_rules! error {
    ($($arg:tt)*) => {
        eprintln!("{}", format!($($arg)*).red());
        std::process::exit(1);
    }
}

fn access_file(path: &str, mode: AccessMode) -> File {
    match OpenOptions::new()
        .read(if mode == AccessMode::Input {true} else {false})
        .write(if mode == AccessMode::Output {true} else {false})
        .create(false)
        .open(path) {
            Ok(file) => file,
            Err(err) => {
                error!("Access denied to '{}': {}", path, err);
            }
    }
}

fn main() {
    let config = Config::parse();

    match config.action {
        Action::Show => {
            let mut data = Vec::new();
            let mut sys = access_file(SYS_PATH, AccessMode::Input);

            if let Err(e) = sys.read_to_end(&mut data) {
                error!("Failed to access '{}': {}", SYS_PATH, e);
            }

            let info = String::from_utf8(data).unwrap();
            print!("{}", info);
        },
        Action::UpdateCount { count } => {
            let mut sys = access_file(COUNT_PATH, AccessMode::Output);

            match sys.write_all(count.to_string().as_bytes()) {
                Ok(()) => println!("System updated with {} devices", count),
                Err(e) => {
                    error!("Update rejected: '{}'", e);
                }
            }
        },
        Action::Modify { path, config } => {
            let sys = access_file(&path, AccessMode::Output);

            let config_id = match config {
                ConfigType::Static => 0,
                ConfigType::Dynamic => 1,
            };

            unsafe {
                if libc::ioctl(sys.as_raw_fd(), config_id) == -1 {
                    error!("System call failed");
                }
            }

            println!("System path '{}' configured with '{:?}'", path, config);
        },
        Action::UpdateSecurity { params } => {
            let mut sys = access_file(SECURITY_PATH, AccessMode::Output);

            let security_params = params.join(",");

            match sys.write_all(security_params.as_bytes()) {
                Ok(()) => println!("Security parameters updated"),
                Err(e) => {
                    error!("Security update rejected: '{}'", e);
                }
            }
        },
        Action::DisplaySecurity => {
            let mut data = Vec::new();
            let mut sys = access_file(SECURITY_PATH, AccessMode::Input);

            if let Err(e) = sys.read_to_end(&mut data) {
                error!("Failed to access '{}': {}", SECURITY_PATH, e);
            }

            let security = String::from_utf8(data).unwrap();
            let params = security.trim_end_matches('\n').split(",").collect::<Vec<&str>>();

            for param in params {
                println!("{}", param);
            }
        },
        Action::GetToken { path } => {
            let mut data = Vec::new();
            let mut sys = access_file(&path, AccessMode::Input);

            if let Err(e) = sys.read_to_end(&mut data) {
                error!("Failed to access '{}': {}", path, e);
            }

            let token = String::from_utf8(data).unwrap();
            println!("System path '{}' returned token: {}", path, token);
        },
        Action::Verify { path, token } => {
            let mut sys = access_file(&path, AccessMode::Output);

            match sys.write_all(token.as_bytes()) {
                Ok(()) => println!("Token verified for system path '{}'", path),
                Err(_) => {
                    error!("Token rejected for system path '{}'", path);
                }
            }
        },
    }
}
