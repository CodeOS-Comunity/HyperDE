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

## Configuration

Edit `hyperde.toml` in the working directory, or point `HYPERDE_CONFIG` at a
different file. The configuration is declarative and does not require Lua or
Rust knowledge:

- `compositor.panel_height` controls the Chroma panel geometry.
- `window_manager.workspaces` sets workspace names.
- `window_manager.terminal_command` is launched by `Mod+Enter`.
- `window_manager.launcher_command` is launched by `Mod+d` when non-empty.
- `window_manager.normal_border` and `focused_border` use RGBA hex colors.
- `window_manager.border_width` sets Penrose border width in pixels.
- `window_manager.focus_follow_mouse` controls pointer focus behavior.
- `window_manager.floating_classes` lists window classes that should float.

`conf.lua` remains as a readable profile for users coming from Lua-based
desktop configuration, while `hyperde.toml` is the file currently loaded by
the Rust runtime.

Start Chroma before HWMS in an X11 session. Only one compositor can own each
screen, so an existing compositor must be stopped first. HWMS uses `Mod` as
the modifier: `Mod+j/k` changes focus, `Mod+1..9` selects workspaces,
`Mod+Shift+1..9` moves windows, `Mod+Space` changes layout, `Mod+Shift+q`
closes a window, and `Mod+q` exits HWMS. `Mod+Shift` plus left or right mouse
buttons moves or resizes the focused window.
