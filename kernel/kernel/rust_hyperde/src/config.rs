/* ───────────────────────── HyperDE Configuration ─────────────────────────
 * Minimal no_std configuration parser ported from upstream HyperDE's
 * hyperde.toml.  Accepts a simple key=value format that the kernel
 * can load from a memory buffer at boot.
 *
 * Features ported from upstream:
 *   - Workspace names (custom labels for each workspace)
 *   - Color scheme (accent, border colors, glass colors)
 *   - Border width and focus-follow-mouse
 *   - Terminal and launcher commands
 *   - Floating window classes
 */

use core::ffi::c_char;
use core::sync::atomic::{AtomicBool, Ordering};

extern "C" {
    fn kprintf(fmt: *const c_char, ...);
}

/* ───────────────────────── Config values ───────────────────────── */

const MAX_WORKSPACES: usize = 9;
const MAX_CMD_LEN: usize = 64;
const MAX_FLOAT_CLASSES: usize = 8;
const MAX_CLASS_LEN: usize = 32;

pub struct HyperdeConfig {
    /* workspace names */
    pub workspace_names: [[u8; 16]; MAX_WORKSPACES],
    pub workspace_count: usize,

    /* colors (XRGB8888) */
    pub accent: u32,
    pub focused_border: u32,
    pub normal_border: u32,
    pub glass_top: u32,
    pub glass_bottom: u32,
    pub glass_alpha: u32,

    /* window manager */
    pub border_width: u32,
    pub focus_follow_mouse: bool,
    pub panel_height: u32,

    /* commands */
    pub terminal_cmd: [u8; MAX_CMD_LEN],
    pub launcher_cmd: [u8; MAX_CMD_LEN],

    /* floating classes */
    pub floating_classes: [[u8; MAX_CLASS_LEN]; MAX_FLOAT_CLASSES],
    pub floating_class_count: usize,
}

impl HyperdeConfig {
    fn workspace_name_str(&self, idx: usize) -> &str {
        if idx >= self.workspace_count {
            return "";
        }
        let name = &self.workspace_names[idx];
        let mut len = 0;
        while len < name.len() && name[len] != 0 {
            len += 1;
        }
        core::str::from_utf8(&name[..len]).unwrap_or("")
    }
}

/* Global config instance */
static mut CONFIG: HyperdeConfig = HyperdeConfig {
    workspace_names: {
        let mut ws = [[0u8; 16]; MAX_WORKSPACES];
        let mut i = 0;
        while i < MAX_WORKSPACES {
            ws[i][0] = b'1' + i as u8;
            i += 1;
        }
        ws
    },
    workspace_count: 9,
    accent: 0x0040D9F0,
    focused_border: 0x0040D9F0,
    normal_border: 0x003c3836,
    glass_top: 0x00242428,
    glass_bottom: 0x0017171B,
    glass_alpha: 0xC8,
    border_width: 2,
    focus_follow_mouse: true,
    panel_height: 28,
    terminal_cmd: {
        let mut buf = [0u8; MAX_CMD_LEN];
        let src = b"xterm";
        let mut i = 0;
        while i < src.len() && i < MAX_CMD_LEN {
            buf[i] = src[i];
            i += 1;
        }
        buf
    },
    launcher_cmd: [0; MAX_CMD_LEN],
    floating_classes: [[0; MAX_CLASS_LEN]; MAX_FLOAT_CLASSES],
    floating_class_count: 2,
};
static CONFIG_LOADED: AtomicBool = AtomicBool::new(false);

pub fn config() -> &'static mut HyperdeConfig {
    unsafe { &mut CONFIG }
}

pub fn config_loaded() -> bool {
    CONFIG_LOADED.load(Ordering::Relaxed)
}

/* ───────────────────────── Minimal Lua parser ─────────────────────────
 * Parses the upstream HyperDE conf.lua format:
 *   return {
 *       compositor = { panel_height = 28 },
 *       window_manager = {
 *           workspaces = { "1", "2", ... },
 *           terminal_command = "xterm",
 *           ...
 *       },
 *       applications = {
 *           commands = {
 *               { name = "Browser", key = "M-b", command = "firefox" },
 *           },
 *       },
 *   }
 */

/* ── Lua tokenizer / recursive-descent parser ─────────────────── */

struct LuaParser<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> LuaParser<'a> {
    fn new(src: &'a [u8]) -> Self {
        Self { src, pos: 0 }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.src.len() {
            let c = self.src[self.pos];
            if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
                self.pos += 1;
            } else if c == b'-' && self.pos + 1 < self.src.len() && self.src[self.pos + 1] == b'-' {
                /* line comment */
                self.pos += 2;
                while self.pos < self.src.len() && self.src[self.pos] != b'\n' {
                    self.pos += 1;
                }
            } else {
                break;
            }
        }
    }

    fn peek(&mut self) -> u8 {
        self.skip_ws();
        if self.pos < self.src.len() { self.src[self.pos] } else { 0 }
    }

    fn eat(&mut self, c: u8) -> bool {
        self.skip_ws();
        if self.pos < self.src.len() && self.src[self.pos] == c {
            self.pos += 1;
            true
        } else {
            false
        }
    }

    fn read_string(&mut self) -> Option<&'a [u8]> {
        self.skip_ws();
        if self.pos >= self.src.len() || self.src[self.pos] != b'"' {
            return None;
        }
        self.pos += 1;
        let start = self.pos;
        while self.pos < self.src.len() && self.src[self.pos] != b'"' {
            if self.src[self.pos] == b'\\' { self.pos += 1; } /* skip escaped char */
            self.pos += 1;
        }
        let end = self.pos;
        if self.pos < self.src.len() { self.pos += 1; } /* skip closing " */
        Some(&self.src[start..end])
    }

    fn read_ident(&mut self) -> &'a [u8] {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len() {
            let c = self.src[self.pos];
            if c >= b'a' && c <= b'z' || c >= b'A' && c <= b'Z' || c >= b'0' && c <= b'9' || c == b'_' {
                self.pos += 1;
            } else { break; }
        }
        &self.src[start..self.pos]
    }

    fn read_number(&mut self) -> Option<i64> {
        self.skip_ws();
        let start = self.pos;
        if self.pos < self.src.len() && self.src[self.pos] == b'-' { self.pos += 1; }
        while self.pos < self.src.len() && self.src[self.pos] >= b'0' && self.src[self.pos] <= b'9' {
            self.pos += 1;
        }
        if self.pos == start { return None; }
        let s = core::str::from_utf8(&self.src[start..self.pos]).ok()?;
        s.parse::<i64>().ok()
    }
}

/// Copy bytes into a fixed buffer, null-terminated
fn buf_copy(dst: &mut [u8], src: &[u8]) {
    let n = src.len().min(dst.len().saturating_sub(1));
    let mut i = 0;
    while i < n { dst[i] = src[i]; i += 1; }
    if i < dst.len() { dst[i] = 0; }
}

/// Parse "#RRGGBB" or "#RRGGBBAA" into XRGB8888
fn hex_color(s: &[u8]) -> Option<u32> {
    if s.len() < 7 || s[0] != b'#' { return None; }
    let mut val: u32 = 0;
    let mut i = 1;
    while i < s.len() && i < 9 {
        let h = match s[i] {
            b'0'..=b'9' => s[i] - b'0',
            b'a'..=b'f' => s[i] - b'a' + 10,
            b'A'..=b'F' => s[i] - b'A' + 10,
            _ => return None,
        };
        val = (val << 4) | h as u32;
        i += 1;
    }
    if i == 7 { val = (val << 8) | 0xFF; }
    Some(val)
}

/* ── Flat key/value store returned by the Lua parser ──────────── */

const LV_STR: u8 = 0;
const LV_INT: u8 = 1;
const LV_BOOL: u8 = 2;
const LV_TABLE: u8 = 3;

const MAX_ENTRIES: usize = 64;

#[derive(Copy, Clone)]
struct LuaEntry {
    key: [u8; 40],
    kind: u8,
    sval: [u8; 80],
    ival: i64,
    bval: bool,
}

static mut ENTRIES: [LuaEntry; MAX_ENTRIES] = [LuaEntry {
    key: [0; 40], kind: LV_STR, sval: [0; 80], ival: 0, bval: false,
}; MAX_ENTRIES];
static mut ENTRY_COUNT: usize = 0;

fn entry_add(key: &[u8]) -> &'static mut LuaEntry {
    unsafe {
        if ENTRY_COUNT >= MAX_ENTRIES { return &mut ENTRIES[0]; }
        let e = &mut ENTRIES[ENTRY_COUNT];
        buf_copy(&mut e.key, key);
        ENTRY_COUNT += 1;
        e
    }
}

fn entry_find(prefix: &[u8], suffix: &[u8]) -> Option<&'static LuaEntry> {
    unsafe {
        for i in 0..ENTRY_COUNT {
            let k = &ENTRIES[i].key;
            let plen = prefix.len();
            let slen = suffix.len();
            if k.len() >= plen + slen
                && &k[..plen] == prefix
                && &k[plen..][..slen] == suffix
            {
                return Some(&ENTRIES[i]);
            }
        }
    }
    None
}

/// Find an entry by full key (e.g. "window_manager.workspaces.1")
fn entry_find_key(key: &[u8]) -> Option<&'static LuaEntry> {
    unsafe {
        for i in 0..ENTRY_COUNT {
            if &ENTRIES[i].key[..key.len()] == key {
                return Some(&ENTRIES[i]);
            }
        }
    }
    None
}

fn entry_str(prefix: &[u8], suffix: &[u8], default: &[u8], dst: &mut [u8]) {
    if let Some(e) = entry_find(prefix, suffix) {
        buf_copy(dst, &e.sval);
    } else {
        buf_copy(dst, default);
    }
}

fn entry_int(prefix: &[u8], suffix: &[u8], default: i64) -> i64 {
    if let Some(e) = entry_find(prefix, suffix) { e.ival } else { default }
}

fn entry_bool(prefix: &[u8], suffix: &[u8], default: bool) -> bool {
    if let Some(e) = entry_find(prefix, suffix) { e.bval } else { default }
}

/* ── Recursive Lua value parser ─────────────── */

fn lua_skip(p: &mut LuaParser) {
    p.skip_ws();
}

/// Parse a Lua value (string, number, bool, or table). Returns true if parsed.
fn lua_value(p: &mut LuaParser) -> bool {
    lua_skip(p);
    if p.peek() == b'"' {
        /* string */
        true /* caller reads with read_string */
    } else if p.peek() == b'{' {
        p.eat(b'{');
        lua_skip(p);
        while p.peek() != b'}' && p.peek() != 0 {
            lua_skip(p);
            /* skip numeric array entries like { "a", "b" } */
            if p.peek() == b'"' || p.peek() == b'{' {
                if p.peek() == b'"' { p.read_string(); }
                else { lua_skip_table(p); }
                lua_skip(p);
                if p.peek() == b',' { p.eat(b','); }
                continue;
            }
            /* key = value */
            let key = p.read_ident();
            if key.is_empty() { break; }
            lua_skip(p);
            if !p.eat(b'=') { break; }
            lua_skip(p);
            if p.peek() == b'"' {
                /* string value — store it (caller needs context to know prefix) */
                /* We'll handle this after returning to the caller */
            } else if p.peek() == b'{' {
                lua_skip_table(p);
            } else {
                /* number or bool */
                lua_skip(p);
                let ident = p.read_ident();
                if ident == b"true" || ident == b"false" { /* bool */ }
                else if !ident.is_empty() { /* identifier, skip */ }
                else { p.read_number(); }
            }
            lua_skip(p);
            if p.peek() == b',' { p.eat(b','); }
        }
        p.eat(b'}');
        true
    } else {
        /* number or ident or bool */
        lua_skip(p);
        let ident = p.read_ident();
        if !ident.is_empty() { return true; }
        p.read_number().is_some()
    }
}

/// Skip a Lua table value (for nested tables we don't care about)
fn lua_skip_table(p: &mut LuaParser) {
    if !p.eat(b'{') { return; }
    let mut depth = 1i32;
    while p.pos < p.src.len() && depth > 0 {
        let c = p.src[p.pos];
        if c == b'{' { depth += 1; }
        else if c == b'}' { depth -= 1; }
        else if c == b'"' {
            p.pos += 1;
            while p.pos < p.src.len() && p.src[p.pos] != b'"' {
                if p.src[p.pos] == b'\\' { p.pos += 1; }
                p.pos += 1;
            }
            if p.pos < p.src.len() { p.pos += 1; }
            continue;
        }
        else if c == b'-' && p.pos + 1 < p.src.len() && p.src[p.pos + 1] == b'-' {
            p.pos += 2;
            while p.pos < p.src.len() && p.src[p.pos] != b'\n' { p.pos += 1; }
            continue;
        }
        p.pos += 1;
    }
}

/// Full Lua parser: reads `return { ... }` and fills the flat ENTRIES table
fn lua_parse(data: &[u8]) {
    unsafe { ENTRY_COUNT = 0; }
    let mut p = LuaParser::new(data);

    /* skip `return` keyword */
    p.skip_ws();
    let kw = p.read_ident();
    if kw != b"return" { return; }
    if !p.eat(b'{') { return; }

    lua_parse_table(&mut p, b"");
}

fn lua_parse_table(p: &mut LuaParser, prefix: &[u8]) {
    loop {
        lua_skip(p);
        if p.peek() == b'}' || p.peek() == 0 { break; }

        /* skip array entries (positional tables/strings) */
        if p.peek() == b'"' || p.peek() == b'{' {
            if p.peek() == b'{' {
                lua_skip_table(p);
            } else {
                p.read_string();
            }
            lua_skip(p);
            if p.peek() == b',' { p.eat(b','); }
            continue;
        }

        let key = p.read_ident();
        if key.is_empty() { break; }
        lua_skip(p);
        if !p.eat(b'=') { break; }
        lua_skip(p);

        if p.peek() == b'"' {
            /* string value */
            if let Some(s) = p.read_string() {
                let mut full = [0u8; 80];
                let plen = prefix.len();
                let klen = key.len();
                let total = plen + klen;
                if total < 80 {
                    let mut i = 0;
                    while i < plen { full[i] = prefix[i]; i += 1; }
                    let mut j = 0;
                    while j < klen && i < 80 { full[i] = key[j]; i += 1; j += 1; }
                    let e = entry_add(&full[..total]);
                    e.kind = LV_STR;
                    buf_copy(&mut e.sval, s);
                }
            }
        } else if p.peek() == b'{' {
            /* sub-table: recurse with expanded prefix */
            let mut sub_prefix = [0u8; 80];
            let plen = prefix.len();
            let klen = key.len();
            let total = plen + klen + 1;
            if total < 80 {
                let mut i = 0;
                while i < plen { sub_prefix[i] = prefix[i]; i += 1; }
                let mut j = 0;
                while j < klen && i < 79 { sub_prefix[i] = key[j]; i += 1; j += 1; }
                sub_prefix[i] = b'.'; i += 1;
                p.eat(b'{');
                lua_parse_table(p, &sub_prefix[..i]);
                p.eat(b'}');
            } else {
                lua_skip_table(p);
            }
        } else {
            /* number or bool */
            lua_skip(p);
            let ident = p.read_ident();
            let mut full = [0u8; 80];
            let plen = prefix.len();
            let klen = key.len();
            let total = plen + klen;
            if total < 80 {
                let mut i = 0;
                while i < plen { full[i] = prefix[i]; i += 1; }
                let mut j = 0;
                while j < klen && i < 80 { full[i] = key[j]; i += 1; j += 1; }
                let e = entry_add(&full[..total]);
                if ident == b"true" {
                    e.kind = LV_BOOL;
                    e.bval = true;
                } else if ident == b"false" {
                    e.kind = LV_BOOL;
                    e.bval = false;
                } else {
                    e.kind = LV_INT;
                    e.ival = p.read_number().unwrap_or(0);
                }
            }
        }

        lua_skip(p);
        if p.peek() == b',' { p.eat(b','); }
    }
}

/* ── Apply parsed Lua entries to the global config ─────────── */

pub fn config_parse(data: &[u8]) {
    /* Check if this is a Lua file (starts with "return" or contains "--") */
    let is_lua = {
        let trimmed = {
            let mut s = 0;
            while s < data.len() && (data[s] == b' ' || data[s] == b'\t' || data[s] == b'\n' || data[s] == b'\r') { s += 1; }
            &data[s..]
        };
        trimmed.starts_with(b"return ") || trimmed.starts_with(b"return{")
    };

    if is_lua {
        lua_parse(data);
    }

    let cfg = unsafe { &mut CONFIG };

    /* window_manager.* */
    let ws = b"window_manager.";

    /* workspaces: only read if Lua parsed them */
    {
        let mut count = 0usize;
        /* look for window_manager.workspaces.1, .2, etc */
        let mut idx = 1u8;
        loop {
            let mut suffix = [0u8; 4];
            suffix[0] = b'w'; suffix[1] = b'o'; suffix[2] = b'r'; suffix[3] = b'k';
            /* build "workspaces.N" */
            let mut full = [0u8; 40];
            let base = b"window_manager.workspaces.";
            let mut i = 0;
            while i < base.len() { full[i] = base[i]; i += 1; }
            full[i] = idx; i += 1;
            if let Some(e) = entry_find_key(&full[..i]) {
                let mut name = [0u8; 16];
                buf_copy(&mut name, &e.sval);
                cfg.workspace_names[count] = name;
                count += 1;
            } else {
                break;
            }
            idx += 1;
            if count >= MAX_WORKSPACES { break; }
        }
        if count > 0 { cfg.workspace_count = count; }
    }

    entry_str(ws, b"terminal_command", b"xterm", &mut cfg.terminal_cmd);
    entry_str(ws, b"launcher_command", b"", &mut cfg.launcher_cmd);

    if let Some(e) = entry_find(ws, b"normal_border") {
        if let Some(c) = hex_color(&e.sval) { cfg.normal_border = c; }
    }
    if let Some(e) = entry_find(ws, b"focused_border") {
        if let Some(c) = hex_color(&e.sval) { cfg.focused_border = c; }
    }
    cfg.border_width = entry_int(ws, b"border_width", 2) as u32;
    cfg.focus_follow_mouse = entry_bool(ws, b"focus_follow_mouse", true);

    /* floating_classes */
    {
        let mut count = 0usize;
        let mut idx = 1u8;
        loop {
            let mut full = [0u8; 40];
            let base = b"window_manager.floating_classes.";
            let mut i = 0;
            while i < base.len() { full[i] = base[i]; i += 1; }
            full[i] = idx; i += 1;
            if let Some(e) = entry_find_key(&full[..i]) {
                buf_copy(&mut cfg.floating_classes[count], &e.sval);
                count += 1;
            } else {
                break;
            }
            idx += 1;
            if count >= MAX_FLOAT_CLASSES { break; }
        }
        if count > 0 { cfg.floating_class_count = count; }
    }

    /* compositor.* */
    cfg.panel_height = entry_int(b"compositor.", b"panel_height", 28) as u32;

    CONFIG_LOADED.store(true, Ordering::Relaxed);

    unsafe {
        kprintf(
            b"HYPERDE CONFIG: loaded %u workspaces, border=#%06x/%06x bw=%u\0".as_ptr() as *const c_char,
            cfg.workspace_count as u32,
            cfg.normal_border,
            cfg.focused_border,
            cfg.border_width,
        );
    }
}

/* ───────────────────────── FFI export ───────────────────────── */

/// Called from C with a config buffer and length
#[no_mangle]
pub unsafe extern "C" fn hyperde_config_load(data: *const u8, len: usize) {
    if data.is_null() || len == 0 {
        return;
    }
    let slice = core::slice::from_raw_parts(data, len);
    config_parse(slice);
}

/// Get workspace count
#[no_mangle]
pub unsafe extern "C" fn hyperde_config_workspace_count() -> u32 {
    config().workspace_count as u32
}

/// Get workspace name by index (writes to out buffer)
#[no_mangle]
pub unsafe extern "C" fn hyperde_config_workspace_name(idx: u32, out: *mut u8, max_len: u32) {
    let cfg = config();
    let i = idx as usize;
    if i >= cfg.workspace_count {
        return;
    }
    let name = &cfg.workspace_names[i];
    let mut j = 0;
    while j < name.len() && j < max_len as usize && name[j] != 0 {
        *out.add(j) = name[j];
        j += 1;
    }
    if j < max_len as usize {
        *out.add(j) = 0;
    }
}
