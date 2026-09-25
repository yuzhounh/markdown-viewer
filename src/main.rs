#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod render;

use std::{
    borrow::Cow,
    env,
    ffi::c_void,
    fs,
    path::{Component, Path, PathBuf},
    sync::{Arc, RwLock},
    thread,
    time::Duration,
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
    Activate,
    Close,
    Reload(String),
    ZoomIn,
    ZoomOut,
    ZoomReset,
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
                    || proxy.send_event(UserEvent::Activate).is_err()
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
    let requested = env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .map(|path| {
            path.canonicalize()
                .with_context(|| format!("找不到文件：{}", path.display()))
        })
        .transpose()?;
    let mut primary_instance = if let Some(path) = requested.as_deref() {
        match claim_document_instance(path)? {
            InstanceClaim::Primary(instance) => Some(instance),
            InstanceClaim::Secondary => return Ok(()),
        }
    } else {
        None
    };

    let mut document = load_document(requested)?;
    let watch_source = document.watch_source.take();
    let window_title = format!("{} — MDViewer", document.title);
    let navigation_root = document.root.clone();
    let document = Arc::new(RwLock::new(document));
    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    let proxy = event_loop.create_proxy();
    let saved_window_state = load_window_state().filter(|state| {
        event_loop
            .available_monitors()
            .any(|monitor| state_intersects_monitor(*state, monitor.position(), monitor.size()))
    });
    let app_icon = Icon::from_rgba(
        include_bytes!("../assets/mdviewer-icon-32.rgba").to_vec(),
        32,
        32,
    )
    .context("无法加载 MDViewer 图标")?;
    let mut window_builder = WindowBuilder::new()
        .with_title(window_title)
        .with_inner_size(Size::Logical(LogicalSize::new(1080.0, 760.0)))
        .with_min_inner_size(Size::Logical(LogicalSize::new(520.0, 400.0)))
        .with_window_icon(Some(app_icon));
    if let Some(state) = saved_window_state {
        window_builder = window_builder
            .with_position(PhysicalPosition::new(state.x, state.y))
            .with_inner_size(PhysicalSize::new(state.width, state.height))
            .with_maximized(state.maximized);
    }
    let window = window_builder
        .build(&event_loop)
        .context("无法创建 MDViewer 窗口")?;

    let protocol_document = Arc::clone(&document);
    let ipc_proxy = proxy.clone();

    let webview = WebViewBuilder::new()
        .with_custom_protocol("mdviewer".into(), move |_webview_id, request| {
            let document = protocol_document
                .read()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            serve_request(request.uri().to_string(), &document)
        })
        .with_url("mdviewer://localhost/index.html")
        .with_navigation_handler(move |url| handle_navigation(&url, &navigation_root))
        .with_ipc_handler(move |request| {
            let event = match request.body().as_str() {
                "close" => Some(UserEvent::Close),
                "zoom-in" => Some(UserEvent::ZoomIn),
                "zoom-out" => Some(UserEvent::ZoomOut),
                "zoom-reset" => Some(UserEvent::ZoomReset),
                _ => None,
            };
            if let Some(event) = event {
                let _ = ipc_proxy.send_event(event);
            }
        })
        .with_hotkeys_zoom(false)
        .with_devtools(cfg!(debug_assertions))
        .build(&window)
        .context("无法启动 WebView2；请确认 Microsoft Edge WebView2 Runtime 已安装")?;

    let mut zoom_factor = load_zoom_factor();
    let _ = webview.zoom(zoom_factor);

    if let Some((path, initial)) = watch_source {
        spawn_document_watcher(path, initial, proxy.clone());
    }
    if let Some(instance) = primary_instance.as_mut() {
        instance.start_listener(proxy.clone());
    }

    let mut normal_window_state = saved_window_state
        .map(|state| WindowState {
            maximized: false,
            ..state
        })
        .or_else(|| capture_window_state(&window));

    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::Wait;
        match event {
            Event::UserEvent(UserEvent::Activate) => {
                window.set_minimized(false);
                window.set_visible(true);
                window.set_focus();
            }
            Event::UserEvent(UserEvent::Close)
            | Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => {
                save_window_state(&window, normal_window_state);
                *control_flow = ControlFlow::Exit;
            }
            Event::UserEvent(UserEvent::ZoomIn) => {
                change_zoom(&webview, &mut zoom_factor, 1);
            }
            Event::UserEvent(UserEvent::ZoomOut) => {
                change_zoom(&webview, &mut zoom_factor, -1);
            }
            Event::UserEvent(UserEvent::ZoomReset) => {
                set_zoom(&webview, &mut zoom_factor, DEFAULT_ZOOM_FACTOR);
            }
            Event::UserEvent(UserEvent::Reload(markdown)) => {
                let mut document = document
                    .write()
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
                let title = document.title.clone();
                document.html = render::document(&markdown, &title);
                drop(document);
                let _ = webview.evaluate_script("window.location.reload()");
            }
            Event::WindowEvent {
                event: WindowEvent::Moved(_) | WindowEvent::Resized(_),
                ..
            } if !window.is_maximized() && !window.is_minimized() => {
                if let Some(state) = capture_window_state(&window) {
                    normal_window_state = Some(state);
                }
            }
            _ => {}
        }
    });
}

fn claim_document_instance(path: &Path) -> Result<InstanceClaim> {
    let normalized = normalized_document_path(path);
    let event_name = HSTRING::from(format!("Local\\MDViewer-{}", instance_key(&normalized)));
    let (event, already_exists) = unsafe {
        let event = CreateEventW(None, false, false, &event_name)
            .context("无法创建 MDViewer 窗口激活事件")?;
        (event, GetLastError() == ERROR_ALREADY_EXISTS)
    };

    if already_exists {
        let activation = unsafe { SetEvent(event) };
        let _ = unsafe { CloseHandle(event) };
        activation.context("无法激活已有的 MDViewer 窗口")?;
        Ok(InstanceClaim::Secondary)
    } else {
        Ok(InstanceClaim::Primary(PrimaryInstance {
            event: Some(event),
        }))
    }
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

fn spawn_document_watcher(path: PathBuf, initial: String, proxy: EventLoopProxy<UserEvent>) {
    thread::spawn(move || {
        let mut changes = ChangeDebouncer::new(initial);
        loop {
            thread::sleep(Duration::from_millis(200));
            let Ok(markdown) = fs::read_to_string(&path) else {
                // Editors may briefly remove, replace, or lock a file while saving.
                continue;
            };
            let Some(markdown) = changes.observe(markdown) else {
                continue;
            };
            if proxy.send_event(UserEvent::Reload(markdown)).is_err() {
                break;
            }
        }
    });
}

fn config_dir() -> Option<PathBuf> {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .map(|dir| dir.join("MDViewer"))
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

fn change_zoom(webview: &WebView, current: &mut f64, step: isize) {
    let next = stepped_zoom_factor(*current, step);
    set_zoom(webview, current, next);
}

fn set_zoom(webview: &WebView, current: &mut f64, next: f64) {
    if (*current - next).abs() < f64::EPSILON {
        return;
    }
    if webview.zoom(next).is_ok() {
        *current = next;
        save_zoom_factor(next);
    }
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
                html: render::document(markdown, "欢迎使用 MDViewer"),
                root: env::current_dir().context("无法读取当前目录")?,
                title: "欢迎使用 MDViewer".to_owned(),
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

fn handle_navigation(raw_url: &str, root: &Path) -> bool {
    let Ok(url) = Url::parse(raw_url) else {
        return false;
    };

    // Wry rewrites custom protocols to http://<scheme>.localhost on Windows.
    if url.scheme() == "mdviewer" || url.host_str() == Some("mdviewer.localhost") {
        if matches!(url.path(), "" | "/" | "/index.html") {
            return true;
        }

        let decoded = percent_decode_str(url.path()).decode_utf8_lossy();
        let relative = Path::new(decoded.trim_start_matches('/'));
        if let Some(path) = canonical_child(root, &root.join(relative)) {
            let _ = open::that_detached(path);
        }
        return false;
    }

    if matches!(url.scheme(), "http" | "https" | "mailto") {
        let _ = open::that_detached(raw_url);
    }
    false
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
