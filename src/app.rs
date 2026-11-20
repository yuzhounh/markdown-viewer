use super::*;
use serde_json::{Value, json};
use std::{collections::BTreeMap, time::SystemTime};
use tao::platform::windows::WindowExtWindows;
use tao::{event_loop::EventLoopWindowTarget, window::WindowId};

struct Page {
    document: Document,
    source: Option<PathBuf>,
    position: (f64, f64),
    toc_open: bool,
    chrome: Value,
}

type Pages = Arc<RwLock<BTreeMap<u64, Page>>>;

struct ReaderWindow {
    // Drop WebView before its native parent window.
    webview: WebView,
    caption: caption::NativeCaption,
    window: Window,
    tabs: Vec<u64>,
    active: Arc<RwLock<u64>>,
    normal: Option<WindowState>,
}

struct Browser {
    pages: Pages,
    watchers: BTreeMap<u64, watcher::DocumentWatcher>,
    windows: Vec<ReaderWindow>,
    next_id: u64,
    zoom: f64,
    proxy: EventLoopProxy<UserEvent>,
    initial_state: Option<WindowState>,
    recent: Vec<PathBuf>,
}

pub fn run() -> Result<()> {
    let requested: Vec<PathBuf> = env::args_os()
        .skip(1)
        .map(PathBuf::from)
        .map(|path| {
            path.canonicalize()
                .with_context(|| format!("找不到文件：{}", path.display()))
        })
        .collect::<Result<_>>()?;
    let mut instance = match claim_browser_instance(&requested)? {
        InstanceClaim::Secondary => return Ok(()),
        InstanceClaim::Primary(instance) => instance,
    };
    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    let proxy = event_loop.create_proxy();
    let initial_state = load_window_state().filter(|state| {
        event_loop
            .available_monitors()
            .any(|monitor| state_intersects_monitor(*state, monitor.position(), monitor.size()))
    });
    let mut browser = Browser {
        pages: Arc::new(RwLock::new(BTreeMap::new())),
        watchers: BTreeMap::new(),
        windows: Vec::new(),
        next_id: 1,
        zoom: load_zoom_factor(),
        proxy: proxy.clone(),
        initial_state,
        recent: config_dir()
            .and_then(|dir| fs::read(dir.join("recent-documents-v1.json")).ok())
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default(),
    };
    if requested.is_empty() {
        browser.open(None, &event_loop)?;
    } else {
        for path in requested {
            browser.open(Some(path), &event_loop)?;
        }
    }
    instance.start_listener(proxy.clone());
    // Covers secondary launches that arrived before the listener was ready.
    let _ = proxy.send_event(UserEvent::Incoming);
    event_loop.run(move |event, target, control_flow| {
        *control_flow = ControlFlow::Wait;
        let result = match event {
            Event::UserEvent(UserEvent::Incoming) => browser.incoming(target),
            Event::UserEvent(UserEvent::Command(window, command)) => {
                browser.command(window, &command, target)
            }
            Event::UserEvent(UserEvent::TabDrop { source, drop }) => {
                let result = browser.drop_tab(source, drop, target);
                for reader in &browser.windows {
                    reader.caption.finish_drop(drop.document);
                }
                result
            }
            Event::UserEvent(UserEvent::Reload(id, markdown)) => {
                let mut pages = browser.pages.write().unwrap();
                if let Some(page) = pages.get_mut(&id) {
                    page.document.html = render::document(&markdown, &page.document.title);
                }
                drop(pages);
                // Hidden documents are rendered when next selected.
                for index in 0..browser.windows.len() {
                    if *browser.windows[index].active.read().unwrap() == id {
                        browser.refresh(index, true);
                    }
                }
                Ok(())
            }
            Event::WindowEvent {
                window_id,
                event: WindowEvent::CloseRequested,
                ..
            } => {
                browser.close_window(window_id);
                Ok(())
            }
            Event::WindowEvent {
                window_id,
                event: WindowEvent::Focused(true),
                ..
            } => {
                // Last focused window is the destination for newly opened documents.
                if let Some(index) = browser.index(window_id) {
                    let window = browser.windows.remove(index);
                    browser.windows.push(window);
                }
                Ok(())
            }
            Event::WindowEvent {
                window_id,
                event:
                    window_event @ (WindowEvent::Moved(_)
                    | WindowEvent::Resized(_)
                    | WindowEvent::ScaleFactorChanged { .. }),
                ..
            } => {
                if let Some(index) = browser.index(window_id) {
                    let reader = &mut browser.windows[index];
                    if !matches!(window_event, WindowEvent::Moved(_)) {
                        if let Err(error) = reader.caption.resize(&reader.window, &reader.webview) {
                            eprintln!("无法调整阅读区域：{error}");
                        }
                    }
                    if !reader.window.is_maximized() && !reader.window.is_minimized() {
                        reader.normal = capture_window_state(&reader.window);
                    }
                }
                Ok(())
            }
            Event::WindowEvent {
                event: WindowEvent::ThemeChanged(_),
                ..
            } => {
                browser.sync_captions();
                Ok(())
            }
            _ => Ok(()),
        };
        if let Err(error) = result {
            eprintln!("Markdown Viewer: {error:#}");
            if let Some(reader) = browser.windows.last() {
                let message = script_json(&json!(format!("{error:#}")));
                let _ = reader
                    .webview
                    .evaluate_script(&format!("window.showReaderError({message})"));
            }
        }
        if browser.windows.is_empty() {
            *control_flow = ControlFlow::Exit;
        }
    });
}

impl Browser {
    fn index(&self, id: WindowId) -> Option<usize> {
        self.windows
            .iter()
            .position(|reader| reader.window.id() == id)
    }

    fn open(
        &mut self,
        source: Option<PathBuf>,
        target: &EventLoopWindowTarget<UserEvent>,
    ) -> Result<()> {
        let existing = {
            let pages = self.pages.read().unwrap();
            pages
                .iter()
                .find(|(_, page)| match (&source, &page.source) {
                    (Some(a), Some(b)) => {
                        normalized_document_path(a) == normalized_document_path(b)
                    }
                    (None, None) => true,
                    _ => false,
                })
                .map(|(id, _)| *id)
        };
        if let Some(id) = existing {
            if let Some(index) = self
                .windows
                .iter()
                .position(|reader| reader.tabs.contains(&id))
            {
                self.select(index, id);
                self.focus(index);
            }
            if let Some(source) = source {
                self.remember(source);
            }
            self.sync_captions();
            return Ok(());
        }
        let mut document = load_document(source.clone())?;
        let id = self.next_id;
        self.next_id += 1;
        let watcher = document
            .watch_source
            .take()
            .map(|(path, initial)| {
                let proxy = self.proxy.clone();
                watcher::start(&path, initial, move |text| {
                    proxy.send_event(UserEvent::Reload(id, text)).is_ok()
                })
            })
            .transpose()
            .context("无法监听 Markdown 文件变化")?;
        self.pages.write().unwrap().insert(
            id,
            Page {
                document,
                source: source.clone(),
                position: (0.0, 0.0),
                toc_open: false,
                chrome: Value::Null,
            },
        );
        if let Some(watcher) = watcher {
            self.watchers.insert(id, watcher);
        }
        if !self.windows.is_empty() {
            let index = self.windows.len() - 1;
            self.windows[index].tabs.push(id);
            self.select(index, id);
            self.focus(index);
        } else {
            match self.create_window(vec![id], id, target) {
                Ok(reader) => {
                    self.windows.push(reader);
                    self.refresh(self.windows.len() - 1, true);
                }
                Err(error) => {
                    self.pages.write().unwrap().remove(&id);
                    self.watchers.remove(&id);
                    return Err(error);
                }
            }
        }
        if let Some(source) = source {
            self.remember(source);
        }
        self.sync_captions();
        Ok(())
    }

    fn create_window(
        &self,
        tabs: Vec<u64>,
        id: u64,
        target: &EventLoopWindowTarget<UserEvent>,
    ) -> Result<ReaderWindow> {
        let icon = Icon::from_rgba(
            include_bytes!("../assets/mdviewer-icon-32.rgba").to_vec(),
            32,
            32,
        )?;
        let mut builder = WindowBuilder::new()
            .with_title("Markdown Viewer")
            .with_inner_size(Size::Logical(LogicalSize::new(1080.0, 760.0)))
            .with_min_inner_size(Size::Logical(LogicalSize::new(520.0, 400.0)))
            .with_visible(false)
            .with_window_icon(Some(icon));
        if let Some(state) = self.initial_state {
        // Cascade detached windows, while preserving the saved geometry.
            let offset = (self.windows.len() % 8) as i32 * 24;
            builder = builder
                .with_position(PhysicalPosition::new(state.x + offset, state.y + offset))
                .with_inner_size(PhysicalSize::new(state.width, state.height))
                .with_maximized(state.maximized);
        }
        let window = builder
            .build(target)
            .context("无法创建 Markdown Viewer 窗口")?;
        let window_id = window.id();
        let caption = caption::NativeCaption::new(&window, self.proxy.clone())?;
        let active = Arc::new(RwLock::new(id));
        let protocol_pages = self.pages.clone();
        let navigation_pages = self.pages.clone();
        let navigation_active = active.clone();
        let navigation_proxy = self.proxy.clone();
        let drop_proxy = self.proxy.clone();
        let proxy = self.proxy.clone();
        let webview = WebViewBuilder::new()
            .with_custom_protocol("mdviewer".into(), move |_, request| {
                serve_page(&request.uri().to_string(), &protocol_pages)
            })
            .with_navigation_handler(move |url| {
                navigate(
                    &url,
                    *navigation_active.read().unwrap(),
                    &navigation_pages,
                    window_id,
                    &navigation_proxy,
                )
            })
            .with_ipc_handler(move |request| {
                let _ = proxy.send_event(UserEvent::Command(window_id, request.body().clone()));
            })
            .with_drag_drop_handler(move |event| {
                if let wry::DragDropEvent::Drop { paths, .. } = event {
                    for path in paths {
                        let _ = drop_proxy.send_event(UserEvent::Command(
                            window_id,
                            format!("open:{}", serde_json::to_string(&path).unwrap()),
                        ));
                    }
                }
                true
            })
            .with_hotkeys_zoom(false)
            .with_devtools(cfg!(debug_assertions))
            .build_as_child(&window)
            .context("无法启动 WebView2；请确认 Runtime 已安装")?;
        webview.zoom(self.zoom)?;
        caption.resize(&window, &webview)?;
        let normal = self
            .initial_state
            .map(|state| WindowState {
                maximized: false,
                ..state
            })
            .or_else(|| capture_window_state(&window));
        Ok(ReaderWindow {
            webview,
            caption,
            window,
            tabs,
            active,
            normal,
        })
    }

    fn focus(&self, index: usize) {
        let window = &self.windows[index].window;
        window.set_minimized(false);
        window.set_visible(true);
        window.set_focus();
        let _ = self.windows[index].webview.focus();
    }

    fn refresh(&mut self, index: usize, reload: bool) {
        let reader = &self.windows[index];
        let id = *reader.active.read().unwrap();
        let mut pages = self.pages.write().unwrap();
        let tabs: Vec<Value> = reader
            .tabs
            .iter()
            .filter_map(|id| {
                pages.get(id).map(|page| {
                    json!({
                        "id": id, "title": page.document.title,
                        "path": page.source.as_ref().map(|path| path.display().to_string())
                    })
                })
            })
            .collect();
        let Some(page) = pages.get_mut(&id) else {
            return;
        };
        page.chrome = json!({ "id": id, "tabs": tabs, "nativeCaption": true,
            "x": page.position.0, "y": page.position.1, "tocOpen": page.toc_open });
        reader
            .window
            .set_title(&format!("{} — Markdown Viewer", page.document.title));
        let state = script_json(&page.chrome);
        drop(pages);
        if reload {
            // Unlike WebViewBuilder::with_url, Wry's Windows load_url does not
            // rewrite custom schemes. Navigate to the registered HTTP alias.
            let _ = reader
                .webview
                .load_url(&format!("http://mdviewer.localhost/doc/{id}/index.html"));
        } else {
            let _ = reader
                .webview
                .evaluate_script(&format!("window.setBrowserState({state})"));
        }
        reader.window.set_visible(true);
        self.sync_captions();
    }

    fn select(&mut self, index: usize, id: u64) {
        let old = *self.windows[index].active.read().unwrap();
        *self.windows[index].active.write().unwrap() = id;
        self.refresh(index, old != id);
    }

    fn close_page(&mut self, index: usize, id: u64) {
        let reader = &mut self.windows[index];
        let Some(position) = reader.tabs.iter().position(|tab| *tab == id) else {
            return;
        };
        reader.tabs.remove(position);
        let was_active = *reader.active.read().unwrap() == id;
        if reader.tabs.is_empty() {
            save_window_state(&reader.window, reader.normal);
            self.windows.remove(index);
        } else {
            if was_active {
                *reader.active.write().unwrap() = reader.tabs[position.min(reader.tabs.len() - 1)];
            }
            self.refresh(index, was_active);
        }
        self.watchers.remove(&id);
        self.pages.write().unwrap().remove(&id);
        self.sync_captions();
    }

    fn close_window(&mut self, window: WindowId) {
        if let Some(index) = self.index(window) {
            let reader = self.windows.remove(index);
            save_window_state(&reader.window, reader.normal);
            for id in &reader.tabs {
                self.watchers.remove(id);
                self.pages.write().unwrap().remove(id);
            }
        }
        self.sync_captions();
    }

    fn drop_tab(
        &mut self,
        source: WindowId,
        drop: caption::TabDrop,
        target: &EventLoopWindowTarget<UserEvent>,
    ) -> Result<()> {
        let Some(source_index) = self
            .index(source)
            .filter(|index| self.windows[*index].tabs.contains(&drop.document))
        else {
            return Ok(());
        };
        let destination = if drop.target == 0 {
            None
        } else {
            // A target closed after mouse-up cancels the transfer; it never creates another window.
            let Some(reader) = self
                .windows
                .iter()
                .find(|reader| reader.window.hwnd() == drop.target)
            else {
                return Ok(());
            };
            Some(reader.window.id())
        };
        if destination == Some(source) {
            if let Some(tabs) = reordered_tabs(
                &self.windows[source_index].tabs,
                drop.document,
                drop.slot as usize,
            ) {
                let was_active = *self.windows[source_index].active.read().unwrap() == drop.document;
                self.windows[source_index].tabs = tabs;
                *self.windows[source_index].active.write().unwrap() = drop.document;
                // Navigate only after mouse capture ended; keep the live DOM when already active.
                self.refresh(source_index, !was_active);
                self.focus(source_index);
            }
            return Ok(());
        }
        let destination = match destination {
            Some(id) => id,
            None if self.windows[source_index].tabs.len() == 1 => {
                self.position_detached(source_index, drop, target);
                self.focus(source_index);
                return Ok(());
            }
            None => {
                // Make a complete destination before touching the source's tab list.
                let mut reader = self.create_window(vec![], drop.document, target)?;
                // Match the source's current width so responsive line wrapping and
                // the saved scroll position describe the same place in the document.
                reader.normal = capture_window_state(&self.windows[source_index].window)
                    .or(self.windows[source_index].normal);
                let id = reader.window.id();
                self.windows.push(reader);
                let index = self.windows.len() - 1;
                self.position_detached(index, drop, target);
                id
            }
        };
        let source_index = self.index(source).unwrap();
        let destination_index = self.index(destination).unwrap();
        let source_active = *self.windows[source_index].active.read().unwrap();
        let Some(plan) = planned_transfer(
            &self.windows[source_index].tabs,
            source_active,
            &self.windows[destination_index].tabs,
            drop.document,
            drop.slot as usize,
        ) else {
            return Ok(());
        };
        self.windows[source_index].tabs = plan.source;
        self.windows[destination_index].tabs = plan.destination;
        *self.windows[destination_index].active.write().unwrap() = drop.document;
        if let Some(active) = plan.source_active {
            *self.windows[source_index].active.write().unwrap() = active;
            self.refresh(source_index, source_active == drop.document);
        } else {
            // Destroy only the empty container. Page, watcher, root and reading state stay intact.
            self.windows.remove(source_index);
        }
        let destination_index = self.index(destination).unwrap();
        self.refresh(destination_index, true);
        self.focus(destination_index);
        Ok(())
    }

    fn position_detached(
        &self,
        index: usize,
        drop: caption::TabDrop,
        target: &EventLoopWindowTarget<UserEvent>,
    ) {
        let reader = &self.windows[index];
        let mut size = reader
            .normal
            .map(|state| PhysicalSize::new(state.width, state.height))
            .unwrap_or_else(|| reader.window.inner_size());
        let mut x = drop.screen_x.saturating_sub(drop.anchor_x);
        let mut y = drop.screen_y.saturating_sub(drop.anchor_y);
        if let Some(monitor) = target.available_monitors().find(|monitor| {
            let position = monitor.position();
            let size = monitor.size();
            i64::from(drop.screen_x) >= i64::from(position.x)
                && i64::from(drop.screen_x) < i64::from(position.x) + i64::from(size.width)
                && i64::from(drop.screen_y) >= i64::from(position.y)
                && i64::from(drop.screen_y) < i64::from(position.y) + i64::from(size.height)
        }) {
            let bounds = monitor.size();
            let position = monitor.position();
            size.width = size.width.min(bounds.width);
            size.height = size.height.min(bounds.height);
            x = x.clamp(
                position.x,
                position
                    .x
                    .saturating_add(bounds.width.saturating_sub(size.width) as i32),
            );
            y = y.clamp(
                position.y,
                position
                    .y
                    .saturating_add(bounds.height.saturating_sub(size.height) as i32),
            );
        }
        reader.window.set_maximized(false);
        reader.window.set_inner_size(size);
        reader
            .window
            .set_outer_position(PhysicalPosition::new(x, y));
    }

    fn command(
        &mut self,
        window: WindowId,
        command: &str,
        target: &EventLoopWindowTarget<UserEvent>,
    ) -> Result<()> {
        let Some(index) = self.index(window) else {
            return Ok(());
        };
        let id = *self.windows[index].active.read().unwrap();
        if let Some(value) = command.strip_prefix("state:") {
            if let Ok(state) = serde_json::from_str::<Value>(value) {
                // A final pagehide message can arrive after another tab was selected.
                if let Some(page_id) = state["id"]
                    .as_u64()
                    .filter(|id| self.windows[index].tabs.contains(id))
                {
                    if let Some(page) = self.pages.write().unwrap().get_mut(&page_id) {
                        page.position = (coordinate(&state["x"]), coordinate(&state["y"]));
                        page.toc_open = state["tocOpen"].as_bool().unwrap_or(false);
                    }
                }
            }
        } else if let Some(value) = command.strip_prefix("press-tab:") {
            if let Ok(next) = value.parse() {
                if self.windows[index].tabs.contains(&next) {
                    self.select(index, next);
                }
            }
        } else if let Some(value) = command.strip_prefix("activate:") {
            if let Ok(next) = value.parse() {
                if let Some(owner) = self
                    .windows
                    .iter()
                    .position(|reader| reader.tabs.contains(&next))
                {
                    self.select(owner, next);
                    self.focus(owner);
                }
            }
        } else if let Some(value) = command.strip_prefix("recent:") {
            if let Some(path) = value
                .parse::<usize>()
                .ok()
                .and_then(|index| self.recent.get(index))
                .cloned()
            {
                self.open(Some(path.canonicalize()?), target)?;
            }
        } else if let Some(value) = command.strip_prefix("select:") {
            if let Ok(next) = value.parse() {
                if self.windows[index].tabs.contains(&next) {
                    self.select(index, next);
                }
            }
        } else if let Some(value) = command.strip_prefix("close-tab:") {
            if let Ok(closed) = value.parse() {
                self.close_page(index, closed);
            }
        } else if let Some(value) = command.strip_prefix("open:") {
            let path: PathBuf = serde_json::from_str(value)?;
            self.open(Some(path.canonicalize()?), target)?;
        } else {
            match command {
                "search-tabs" => self.windows[index].caption.search(),
                "focus-reader" => {
                    self.windows[index].webview.focus()?;
                }
                "toggle-toc" => {
                    self.windows[index]
                        .webview
                        .evaluate_script("window.toggleToc?.()")?;
                }
                "open-dialog" => {
                    let paths = choose_documents(&self.windows[index].window)?;
                    for path in paths {
                        self.open(Some(path.canonicalize()?), target)?;
                    }
                }
                "close" => self.close_page(index, id),
                "next-tab" | "previous-tab" => {
                    let reader = &self.windows[index];
                    let current = reader.tabs.iter().position(|tab| *tab == id).unwrap_or(0);
                    let next = if command == "next-tab" {
                        (current + 1) % reader.tabs.len()
                    } else {
                        (current + reader.tabs.len() - 1) % reader.tabs.len()
                    };
                    self.select(index, reader.tabs[next]);
                }
                "zoom-in" | "zoom-out" | "zoom-reset" => {
                    let zoom = if command == "zoom-reset" {
                        DEFAULT_ZOOM_FACTOR
                    } else {
                        stepped_zoom_factor(self.zoom, if command == "zoom-in" { 1 } else { -1 })
                    };
                    for reader in &self.windows {
                        reader.webview.zoom(zoom)?;
                    }
                    self.zoom = zoom;
                    save_zoom_factor(zoom);
                }
                _ => {}
            }
        }
        Ok(())
    }

    fn incoming(&mut self, target: &EventLoopWindowTarget<UserEvent>) -> Result<()> {
        let dir = request_dir()?;
        if !dir.exists() {
            return Ok(());
        }
        let mut first_error = None;
        for entry in fs::read_dir(dir)? {
            let path = entry?.path();
            if path.extension().is_none_or(|ext| ext != "json") {
                continue;
            }
            let request: Vec<PathBuf> = match fs::read(&path)
                .ok()
                .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            {
                Some(request) => request,
                None => continue,
            };
            // Delete only the consumed request, never user documents.
            fs::remove_file(&path)?;
            if request.is_empty() {
                if !self.windows.is_empty() {
                    self.focus(self.windows.len() - 1);
                }
            }
            for source in request {
                if let Err(error) = self.open(Some(source), target) {
                    first_error = Some(error);
                }
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    fn remember(&mut self, source: PathBuf) {
        let normalized = normalized_document_path(&source);
        self.recent
            .retain(|path| normalized_document_path(path) != normalized);
        self.recent.insert(0, source);
        self.recent.truncate(32);
        if let Some(dir) = config_dir() {
            let result = fs::create_dir_all(&dir).and_then(|_| {
                fs::write(
                    dir.join("recent-documents-v1.json"),
                    serde_json::to_vec(&self.recent).unwrap(),
                )
            });
            if let Err(error) = result {
                eprintln!("无法保存最近文档：{error}");
            }
        }
    }

    fn sync_captions(&self) {
        let pages = self.pages.read().unwrap();
        for reader in &self.windows {
            reader
                .caption
                .begin(*reader.active.read().unwrap());
            for id in &reader.tabs {
                if let Some(page) = pages.get(id) {
                    reader
                        .caption
                        .add(0, *id, &page.document.title, page.source.as_deref());
                }
            }
            let opened = reader.tabs.iter().chain(
                self.windows
                    .iter()
                    .filter(|other| other.window.id() != reader.window.id())
                    .flat_map(|other| other.tabs.iter()),
            );
            for id in opened {
                if let Some(page) = pages.get(id) {
                    reader
                        .caption
                        .add(1, *id, &page.document.title, page.source.as_deref());
                }
            }
            for (index, source) in self
                .recent
                .iter()
                .enumerate()
                .filter(|(_, source)| {
                    !pages.values().any(|page| {
                        page.source.as_ref().is_some_and(|open| {
                            normalized_document_path(open) == normalized_document_path(source)
                        })
                    })
                })
                .take(8)
            {
                let title = source.file_name().unwrap_or_default().to_string_lossy();
                reader.caption.add(2, index as u64, &title, Some(source));
            }
            reader.caption.commit();
        }
    }
}

fn coordinate(value: &Value) -> f64 {
    value
        .as_f64()
        .filter(|n| n.is_finite() && *n >= 0.0)
        .unwrap_or(0.0)
}

fn reordered_tabs(tabs: &[u64], document: u64, slot: usize) -> Option<Vec<u64>> {
    let old = tabs.iter().position(|id| *id == document)?;
    let mut reordered = tabs.to_vec();
    reordered.remove(old);
    let slot = slot.min(tabs.len());
    reordered.insert(if slot > old { slot - 1 } else { slot }, document);
    Some(reordered)
}

struct TabTransfer {
    source: Vec<u64>,
    source_active: Option<u64>,
    destination: Vec<u64>,
}

fn planned_transfer(
    source: &[u64],
    active: u64,
    destination: &[u64],
    document: u64,
    slot: usize,
) -> Option<TabTransfer> {
    if destination.contains(&document) {
        return None;
    }
    let old = source.iter().position(|id| *id == document)?;
    let mut remaining = source.to_vec();
    remaining.remove(old);
    let source_active = if active != document {
        Some(active)
    } else {
        remaining
            .get(old.min(remaining.len().saturating_sub(1)))
            .copied()
    };
    let mut destination = destination.to_vec();
    destination.insert(slot.min(destination.len()), document);
    Some(TabTransfer {
        source: remaining,
        source_active,
        destination,
    })
}

fn choose_documents(window: &Window) -> Result<Vec<PathBuf>> {
    use std::{ffi::OsString, os::windows::ffi::OsStringExt};
    use tao::platform::windows::WindowExtWindows;
    use windows::{
        Win32::{Foundation::HWND, UI::Controls::Dialogs::*},
        core::{PCWSTR, PWSTR},
    };
    let filter: Vec<u16> = "Markdown 文件\0*.md;*.markdown;*.mdown;*.mkd\0所有文件\0*.*\0\0"
        .encode_utf16()
        .collect();
    let mut buffer = vec![0u16; 65_536];
    let mut dialog = OPENFILENAMEW {
        lStructSize: std::mem::size_of::<OPENFILENAMEW>() as u32,
        hwndOwner: HWND(window.hwnd() as *mut c_void),
        lpstrFilter: PCWSTR(filter.as_ptr()),
        nFilterIndex: 1,
        lpstrFile: PWSTR(buffer.as_mut_ptr()),
        nMaxFile: buffer.len() as u32,
        Flags: OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR,
        ..Default::default()
    };
    if !unsafe { GetOpenFileNameW(&mut dialog) }.as_bool() {
        let error = unsafe { CommDlgExtendedError() }.0;
        anyhow::ensure!(error == 0, "无法打开文件选择器（错误 {error}）");
        return Ok(Vec::new());
    }
    let parts: Vec<PathBuf> = buffer
        .split(|ch| *ch == 0)
        .take_while(|part| !part.is_empty())
        .map(|part| PathBuf::from(OsString::from_wide(part)))
        .collect();
    Ok(if parts.len() <= 1 {
        parts
    } else {
        parts[1..].iter().map(|name| parts[0].join(name)).collect()
    })
}

fn request_dir() -> Result<PathBuf> {
    config_dir()
        .map(|dir| dir.join("open-requests-v1"))
        .context("无法定位 Markdown Viewer 用户配置目录")
}

fn claim_browser_instance(requested: &[PathBuf]) -> Result<InstanceClaim> {
    // One owner routes documents to their tab hosts. Per-file events cannot carry paths.
    let user = format!(
        "{}\\{}",
        env::var("USERDOMAIN").unwrap_or_default(),
        env::var("USERNAME").unwrap_or_default()
    );
    let name = HSTRING::from(format!(
        "Local\\MarkdownViewer-Browser-v1-{}",
        instance_key(&user)
    ));
    let (event, exists) = unsafe {
        let event = CreateEventW(None, false, false, &name)?;
        (event, GetLastError() == ERROR_ALREADY_EXISTS)
    };
    if !exists {
        return Ok(InstanceClaim::Primary(PrimaryInstance {
            event: Some(event),
        }));
    }
    let result = (|| -> Result<()> {
        let dir = request_dir()?;
        fs::create_dir_all(&dir)?;
        let stamp = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)?
            .as_nanos();
        let path = dir.join(format!("{}-{stamp}.tmp", std::process::id()));
        fs::write(&path, serde_json::to_vec(requested)?)?;
        fs::rename(&path, path.with_extension("json"))?;
        unsafe { SetEvent(event) }.context("无法通知已有的 Markdown Viewer 窗口")?;
        Ok(())
    })();
    let _ = unsafe { CloseHandle(event) };
    result?;
    Ok(InstanceClaim::Secondary)
}

fn script_json(value: &Value) -> String {
    // A document filename must never terminate the trusted state script element.
    value
        .to_string()
        .replace('<', "\\u003c")
        .replace('>', "\\u003e")
        .replace('&', "\\u0026")
}

fn document_route(url: &Url) -> Option<(u64, &str)> {
    let tail = url.path().strip_prefix("/doc/")?;
    let (id, relative) = tail.split_once('/')?;
    Some((id.parse().ok()?, relative))
}

fn serve_page(uri: &str, pages: &Pages) -> Response<Cow<'static, [u8]>> {
    let url = Url::parse(uri).ok();
    let route = url.as_ref().and_then(document_route);
    let pages = pages.read().unwrap();
    if let Some((id, relative)) = route {
        if let Some(page) = pages.get(&id) {
            if relative == "index.html" {
                let state = format!(
                    "<script id=\"browser-state\" type=\"application/json\">{}</script>",
                    script_json(&page.chrome)
                );
                return response(
                    StatusCode::OK,
                    "text/html; charset=utf-8",
                    page.document
                        .html
                        .replacen("<!--browser-state-->", &state, 1)
                        .into_bytes(),
                );
            }
            return serve_request(format!("mdviewer://localhost/{relative}"), &page.document);
        }
    }
    response(StatusCode::NOT_FOUND, "text/plain", b"Not found".to_vec())
}

fn navigate(
    raw: &str,
    active: u64,
    pages: &Pages,
    window: WindowId,
    proxy: &EventLoopProxy<UserEvent>,
) -> bool {
    let Ok(url) = Url::parse(raw) else {
        return false;
    };
    if url.scheme() == "mdviewer" || url.host_str() == Some("mdviewer.localhost") {
        if let Some((id, relative)) = document_route(&url) {
            if id != active {
                return false;
            }
            if relative == "index.html" {
                return true;
            }
            let decoded = percent_decode_str(relative).decode_utf8_lossy();
            let pages = pages.read().unwrap();
            if let Some(page) = pages.get(&id) {
                if let Some(path) = canonical_child(
                    &page.document.root,
                    &page.document.root.join(decoded.as_ref()),
                ) {
                    if path.extension().is_some_and(|ext| {
                        matches!(
                            ext.to_string_lossy().to_lowercase().as_str(),
                            "md" | "markdown" | "mdown" | "mkd"
                        )
                    }) {
                        let _ = proxy.send_event(UserEvent::Command(
                            window,
                            format!("open:{}", serde_json::to_string(&path).unwrap()),
                        ));
                    } else {
                        let _ = open::that_detached(path);
                    }
                }
            }
        }
        return false;
    }
    if matches!(url.scheme(), "http" | "https" | "mailto") {
        let _ = open::that_detached(raw);
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tab_moves_reorder_in_both_directions_without_changing_other_tabs() {
        assert_eq!(reordered_tabs(&[1, 2, 3], 1, 3), Some(vec![2, 3, 1]));
        assert_eq!(reordered_tabs(&[1, 2, 3], 3, 0), Some(vec![3, 1, 2]));
        assert_eq!(reordered_tabs(&[1, 2, 3], 2, 2), Some(vec![1, 2, 3]));
        assert_eq!(reordered_tabs(&[1, 2, 3], 9, 0), None);
        for id in 1..=3 {
            for slot in 0..=3 {
                let reordered = reordered_tabs(&[1, 2, 3], id, slot).unwrap();
                assert_eq!(
                    reordered
                        .iter()
                        .filter(|other| **other != id)
                        .copied()
                        .collect::<Vec<_>>(),
                    (1..=3).filter(|other| *other != id).collect::<Vec<_>>()
                );
                assert_eq!(reordered.iter().filter(|other| **other == id).count(), 1);
            }
        }
    }

    #[test]
    fn tab_moves_across_windows_preserve_all_documents_and_source_selection() {
        for document in 1..=3 {
            for active in 1..=3 {
                for slot in 0..=2 {
                    let plan =
                        planned_transfer(&[1, 2, 3], active, &[4, 5], document, slot).unwrap();
                    let mut all = plan
                        .source
                        .iter()
                        .chain(&plan.destination)
                        .copied()
                        .collect::<Vec<_>>();
                    all.sort_unstable();
                    assert_eq!(all, vec![1, 2, 3, 4, 5]);
                    assert_eq!(plan.destination[slot], document);
                    assert!(plan.source.contains(&plan.source_active.unwrap()));
                    if active != document {
                        assert_eq!(plan.source_active, Some(active));
                    }
                }
            }
        }
        let plan = planned_transfer(&[1], 1, &[2, 3], 1, 1).unwrap();
        assert!(plan.source.is_empty());
        assert_eq!(plan.source_active, None);
        assert_eq!(plan.destination, vec![2, 1, 3]);
        assert!(planned_transfer(&[1], 1, &[2], 9, 0).is_none());
        assert!(planned_transfer(&[1], 1, &[1, 2], 1, 0).is_none());
    }

    #[test]
    fn each_document_serves_resources_from_its_own_directory() {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("tmp")
            .join(format!("tab-routes-{}", std::process::id()));
        fs::create_dir_all(&base).unwrap();
        let pages: Pages = Arc::new(RwLock::new(BTreeMap::new()));
        for id in [1, 2] {
            let root = base.join(id.to_string());
            fs::create_dir_all(&root).unwrap();
            fs::write(root.join("图片.txt"), format!("document {id}")).unwrap();
            pages.write().unwrap().insert(
                id,
                Page {
                    document: Document {
                        html: "<!--browser-state-->".into(),
                        root,
                        title: format!("{id}.md"),
                        watch_source: None,
                    },
                    source: None,
                    position: (0.0, 0.0),
                    toc_open: false,
                    chrome: json!({"id": id}),
                },
            );
        }
        for id in [1, 2] {
            let response = serve_page(
                &format!("http://mdviewer.localhost/doc/{id}/%E5%9B%BE%E7%89%87.txt"),
                &pages,
            );
            assert_eq!(response.status(), StatusCode::OK);
            assert_eq!(
                response.body().as_ref(),
                format!("document {id}").as_bytes()
            );
        }
        pages.write().unwrap().remove(&1);
        assert_eq!(
            serve_page("mdviewer://localhost/doc/1/index.html", &pages).status(),
            StatusCode::NOT_FOUND
        );
        assert_ne!(
            serve_page(
                "mdviewer://localhost/doc/2/%2e%2e/1/%E5%9B%BE%E7%89%87.txt",
                &pages
            )
            .status(),
            StatusCode::OK
        );
        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn tab_metadata_cannot_terminate_trusted_state_script() {
        let metadata = json!({"title": "</script><script>window.ipc.postMessage('close')</script>",
            "path": "C:\\笔记\\引号\".md", "id": 4, "x": 0, "y": 1200});
        let encoded = script_json(&metadata);
        assert!(!encoded.contains('<'));
        assert_eq!(serde_json::from_str::<Value>(&encoded).unwrap(), metadata);
    }
}
