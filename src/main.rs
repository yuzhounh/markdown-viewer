#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod caption;
mod render;
mod watcher;

use std::{
    borrow::Cow,
    env,
    ffi::c_void,
    fs,
    path::{Component, Path, PathBuf},
    sync::{Arc, RwLock},
    thread,
};

use anyhow::{Context, Result};
use http::{Response, StatusCode, header};
use percent_encoding::percent_decode_str;
use tao::{
    dpi::{LogicalSize, PhysicalPosition, PhysicalSize, Size},
    event::{Event, WindowEvent},
    event_loop::{ControlFlow, EventLoopBuilder, EventLoopProxy},
    window::{Icon, Window, WindowBuilder},
};
use url::Url;
use windows::{
    Win32::{
        Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, GetLastError, HANDLE, WAIT_OBJECT_0},
        System::Threading::{CreateEventW, INFINITE, SetEvent, WaitForSingleObject},
    },
    core::HSTRING,
};
use wry::{WebView, WebViewBuilder};

#[derive(Debug)]
enum UserEvent {
    Incoming,
    Command(tao::window::WindowId, String),
    TabDrop { source: tao::window::WindowId, drop: caption::TabDrop },
    Reload(u64, String),
}

struct Document {
    html: String,
    root: PathBuf,
    title: String,
    watch_source: Option<(PathBuf, String)>,
}

struct ChangeDebouncer {
    accepted: String,
    pending: Option<String>,
}

struct PrimaryInstance {
    event: Option<HANDLE>,
}

enum InstanceClaim {
    Primary(PrimaryInstance),
    Secondary,
}

impl PrimaryInstance {
    fn start_listener(&mut self, proxy: EventLoopProxy<UserEvent>) {
        let Some(event) = self.event.take() else {
            return;
        };
        let raw_event = event.0 as usize;
        thread::spawn(move || {
            let event = HANDLE(raw_event as *mut c_void);
            loop {
                if unsafe { WaitForSingleObject(event, INFINITE) } != WAIT_OBJECT_0
                    || proxy.send_event(UserEvent::Incoming).is_err()
                {
                    break;
                }
            }
            let _ = unsafe { CloseHandle(event) };
        });
    }
}

impl Drop for PrimaryInstance {
    fn drop(&mut self) {
        if let Some(event) = self.event.take() {
            let _ = unsafe { CloseHandle(event) };
        }
    }
}

impl ChangeDebouncer {
    fn new(initial: String) -> Self {
        Self {
            accepted: initial,
            pending: None,
        }
    }

    fn observe(&mut self, current: String) -> Option<String> {
        if current == self.accepted {
            self.pending = None;
            return None;
        }

        if self.pending.as_deref() != Some(current.as_str()) {
            self.pending = Some(current);
            return None;
        }

        self.accepted = current.clone();
        self.pending = None;
        Some(current)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct WindowState {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    maximized: bool,
}

const MIN_WINDOW_WIDTH: u32 = 520;
const MIN_WINDOW_HEIGHT: u32 = 400;
const MAX_WINDOW_DIMENSION: u32 = 32_768;
const DEFAULT_ZOOM_FACTOR: f64 = 1.0;
const ZOOM_FACTORS: &[f64] = &[
    0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0,
];

fn main() -> Result<()> {
    app::run()
}

fn normalized_document_path(path: &Path) -> String {
    path.to_string_lossy().to_lowercase()
}

fn instance_key(normalized_path: &str) -> String {
    format!(
        "{:016x}{:016x}",
        stable_hash(normalized_path.as_bytes(), 0xcbf29ce484222325),
        stable_hash(normalized_path.as_bytes(), 0x84222325cbf29ce4)
    )
}

fn stable_hash(bytes: &[u8], seed: u64) -> u64 {
    bytes.iter().fold(seed, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
    })
}

fn config_dir() -> Option<PathBuf> {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .map(|dir| dir.join("MarkdownViewer"))
}

fn window_state_path() -> Option<PathBuf> {
    config_dir().map(|dir| dir.join("window-state-v1.txt"))
}

fn load_window_state() -> Option<WindowState> {
    let contents = fs::read_to_string(window_state_path()?).ok()?;
    parse_window_state(&contents)
}

fn parse_window_state(contents: &str) -> Option<WindowState> {
    let mut fields = contents.split_whitespace();
    if fields.next()? != "1" {
        return None;
    }

    let state = WindowState {
        x: fields.next()?.parse().ok()?,
        y: fields.next()?.parse().ok()?,
        width: fields.next()?.parse().ok()?,
        height: fields.next()?.parse().ok()?,
        maximized: match fields.next()? {
            "0" => false,
            "1" => true,
            _ => return None,
        },
    };

    (fields.next().is_none()
        && state.width >= MIN_WINDOW_WIDTH
        && state.height >= MIN_WINDOW_HEIGHT
        && state.width <= MAX_WINDOW_DIMENSION
        && state.height <= MAX_WINDOW_DIMENSION)
        .then_some(state)
}

fn capture_window_state(window: &Window) -> Option<WindowState> {
    let position = window.outer_position().ok()?;
    let size = window.inner_size();
    (size.width >= MIN_WINDOW_WIDTH && size.height >= MIN_WINDOW_HEIGHT).then_some(WindowState {
        x: position.x,
        y: position.y,
        width: size.width,
        height: size.height,
        maximized: false,
    })
}

fn save_window_state(window: &Window, normal_state: Option<WindowState>) {
    let maximized = window.is_maximized();
    let state = if maximized || window.is_minimized() {
        normal_state.map(|state| WindowState { maximized, ..state })
    } else {
        capture_window_state(window)
    };
    let (Some(path), Some(state)) = (window_state_path(), state) else {
        return;
    };
    let Some(parent) = path.parent() else {
        return;
    };
    if fs::create_dir_all(parent).is_ok() {
        let contents = format!(
            "1\n{}\n{}\n{}\n{}\n{}\n",
            state.x,
            state.y,
            state.width,
            state.height,
            u8::from(state.maximized)
        );
        let _ = fs::write(path, contents);
    }
}

fn state_intersects_monitor(
    state: WindowState,
    monitor_position: PhysicalPosition<i32>,
    monitor_size: PhysicalSize<u32>,
) -> bool {
    let left = i64::from(state.x).max(i64::from(monitor_position.x));
    let top = i64::from(state.y).max(i64::from(monitor_position.y));
    let right = (i64::from(state.x) + i64::from(state.width))
        .min(i64::from(monitor_position.x) + i64::from(monitor_size.width));
    let bottom = (i64::from(state.y) + i64::from(state.height))
        .min(i64::from(monitor_position.y) + i64::from(monitor_size.height));

    right - left >= 64 && bottom - top >= 32
}

fn zoom_state_path() -> Option<PathBuf> {
    config_dir().map(|dir| dir.join("zoom-v1.txt"))
}

fn parse_zoom_factor(contents: &str) -> Option<f64> {
    let factor = contents.trim().parse::<f64>().ok()?;
    ZOOM_FACTORS
        .iter()
        .copied()
        .find(|candidate| (candidate - factor).abs() < f64::EPSILON)
}

fn load_zoom_factor() -> f64 {
    zoom_state_path()
        .and_then(|path| fs::read_to_string(path).ok())
        .and_then(|contents| parse_zoom_factor(&contents))
        .unwrap_or(DEFAULT_ZOOM_FACTOR)
}

fn save_zoom_factor(factor: f64) {
    let Some(path) = zoom_state_path() else {
        return;
    };
    let Some(parent) = path.parent() else {
        return;
    };
    if fs::create_dir_all(parent).is_ok() {
        let _ = fs::write(path, format!("{factor}\n"));
    }
}

fn stepped_zoom_factor(current: f64, step: isize) -> f64 {
    let current_index = ZOOM_FACTORS
        .iter()
        .enumerate()
        .min_by(|(_, left), (_, right)| {
            (*left - current).abs().total_cmp(&(*right - current).abs())
        })
        .map(|(index, _)| index)
        .unwrap_or(0);
    let next_index = (current_index as isize + step).clamp(0, ZOOM_FACTORS.len() as isize - 1);
    ZOOM_FACTORS[next_index as usize]
}

fn load_document(requested: Option<PathBuf>) -> Result<Document> {
    match requested {
        Some(absolute) => {
            let markdown = fs::read_to_string(&absolute)
                .with_context(|| format!("无法以 UTF-8 读取：{}", absolute.display()))?;
            let title = absolute
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("Markdown")
                .to_owned();
            let root = absolute.parent().unwrap_or(Path::new(".")).to_path_buf();

            Ok(Document {
                html: render::document(&markdown, &title),
                root,
                title,
                watch_source: Some((absolute, markdown)),
            })
        }
        None => {
            let markdown = include_str!("../sample.md");
            Ok(Document {
                html: render::document(markdown, "欢迎使用 Markdown Viewer"),
                root: env::current_dir().context("无法读取当前目录")?,
                title: "欢迎使用 Markdown Viewer".to_owned(),
                watch_source: None,
            })
        }
    }
}

fn serve_request(uri: String, document: &Document) -> Response<Cow<'static, [u8]>> {
    let path = Url::parse(&uri)
        .ok()
        .map(|url| {
            percent_decode_str(url.path())
                .decode_utf8_lossy()
                .into_owned()
        })
        .unwrap_or_default();

    if path.is_empty() || path == "/" || path == "/index.html" {
        return response(
            StatusCode::OK,
            "text/html; charset=utf-8",
            document.html.clone().into_bytes(),
        );
    }

    let relative = Path::new(path.trim_start_matches('/'));
    if relative
        .components()
        .any(|part| !matches!(part, Component::Normal(_) | Component::CurDir))
    {
        return response(
            StatusCode::FORBIDDEN,
            "text/plain; charset=utf-8",
            b"Forbidden".to_vec(),
        );
    }

    let target = document.root.join(relative);
    let Some(safe_target) = canonical_child(&document.root, &target) else {
        return response(
            StatusCode::NOT_FOUND,
            "text/plain; charset=utf-8",
            b"Not found".to_vec(),
        );
    };

    match fs::read(&safe_target) {
        Ok(bytes) => {
            let content_type = mime_guess::from_path(&safe_target)
                .first_or_octet_stream()
                .essence_str()
                .to_owned();
            response(StatusCode::OK, &content_type, bytes)
        }
        Err(_) => response(
            StatusCode::NOT_FOUND,
            "text/plain; charset=utf-8",
            b"Not found".to_vec(),
        ),
    }
}

fn canonical_child(root: &Path, target: &Path) -> Option<PathBuf> {
    let root = root.canonicalize().ok()?;
    let target = target.canonicalize().ok()?;
    target.starts_with(&root).then_some(target)
}

fn response(status: StatusCode, content_type: &str, body: Vec<u8>) -> Response<Cow<'static, [u8]>> {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, content_type)
        .header(header::CACHE_CONTROL, "no-store")
        .header(header::X_CONTENT_TYPE_OPTIONS, "nosniff")
        .body(Cow::Owned(body))
        .expect("static response headers are valid")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_parent_path_components() {
        let relative = Path::new("../secret.txt");
        assert!(
            relative
                .components()
                .any(|part| !matches!(part, Component::Normal(_) | Component::CurDir))
        );
    }

    #[test]
    fn identifies_internal_windows_protocol_url() {
        let url = Url::parse("http://mdviewer.localhost/index.html").unwrap();
        assert_eq!(url.host_str(), Some("mdviewer.localhost"));
    }

    #[test]
    fn parses_valid_window_state() {
        assert_eq!(
            parse_window_state("1\n240\n180\n1080\n760\n1\n"),
            Some(WindowState {
                x: 240,
                y: 180,
                width: 1080,
                height: 760,
                maximized: true,
            })
        );
    }

    #[test]
    fn rejects_invalid_window_state() {
        assert_eq!(parse_window_state("1 0 0 200 100 0"), None);
        assert_eq!(parse_window_state("2 0 0 1080 760 0"), None);
        assert_eq!(parse_window_state("1 0 0 1080 760 maybe"), None);
    }

    #[test]
    fn detects_state_visible_on_monitor() {
        let monitor_position = PhysicalPosition::new(0, 0);
        let monitor_size = PhysicalSize::new(1920, 1080);
        let visible = WindowState {
            x: 1800,
            y: 900,
            width: 800,
            height: 600,
            maximized: false,
        };
        let off_screen = WindowState {
            x: 2000,
            y: 1200,
            ..visible
        };

        assert!(state_intersects_monitor(
            visible,
            monitor_position,
            monitor_size
        ));
        assert!(!state_intersects_monitor(
            off_screen,
            monitor_position,
            monitor_size
        ));
    }

    #[test]
    fn parses_only_supported_zoom_factors() {
        assert_eq!(parse_zoom_factor("1.25\n"), Some(1.25));
        assert_eq!(parse_zoom_factor("4"), None);
        assert_eq!(parse_zoom_factor("not-a-number"), None);
    }

    #[test]
    fn steps_and_clamps_zoom_factors() {
        assert_eq!(stepped_zoom_factor(1.0, 1), 1.1);
        assert_eq!(stepped_zoom_factor(1.0, -1), 0.9);
        assert_eq!(stepped_zoom_factor(3.0, 1), 3.0);
        assert_eq!(stepped_zoom_factor(0.5, -1), 0.5);
    }

    #[test]
    fn reloads_only_after_changed_content_is_stable() {
        let mut changes = ChangeDebouncer::new("old".to_owned());

        assert_eq!(changes.observe("partial".to_owned()), None);
        assert_eq!(changes.observe("new".to_owned()), None);
        assert_eq!(changes.observe("new".to_owned()), Some("new".to_owned()));
        assert_eq!(changes.observe("new".to_owned()), None);
    }

    #[test]
    fn instance_keys_ignore_windows_path_case_and_distinguish_documents() {
        let upper = normalized_document_path(Path::new(r"C:\Notes\FILE.md"));
        let lower = normalized_document_path(Path::new(r"c:\notes\file.md"));
        let other = normalized_document_path(Path::new(r"c:\notes\other.md"));

        assert_eq!(instance_key(&upper), instance_key(&lower));
        assert_ne!(instance_key(&upper), instance_key(&other));
    }
}
