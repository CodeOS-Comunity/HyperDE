# HyperDE

HyperDE is a portable X11 desktop environment written in Rust.

It is split into two independent processes:

- `chroma` owns the X11 compositor selection and the Composite redirection path.
- `hwms` runs the Penrose window manager through its `x11rb` backend.

Neither component calls Linux-specific APIs. The display connection, window
ownership, event loop, and window management path are all X11 protocol APIs.

## Build

Install a Rust toolchain, then run:

```sh
cargo build --release
```

The resulting executable accepts one component name:

```sh
./target/release/hyperde chroma
./target/release/hyperde hwms
```

Start Chroma before HWMS in an X11 session. Only one compositor can own each
screen, so an existing compositor must be stopped first. The current HWMS
configuration intentionally starts with Penrose defaults and empty bindings;
key and mouse bindings can be added in `src/backend/hwms/mod.rs` without
changing Chroma.
