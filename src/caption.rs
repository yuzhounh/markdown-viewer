use super::*;
use tao::{platform::windows::WindowExtWindows, window::WindowId};

struct CallbackState {
    window: WindowId,
    proxy: EventLoopProxy<UserEvent>,
}

pub struct NativeCaption {
    native: std::ptr::NonNull<c_void>,
    _callback: Box<CallbackState>,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct TabDrop {
    pub document: u64,
    pub target: isize,
    pub slot: u32,
    pub screen_x: i32,
    pub screen_y: i32,
    pub anchor_x: i32,
    pub anchor_y: i32,
}

unsafe extern "C" {
    fn md_caption_create(
        parent: isize,
        callback: extern "C" fn(*mut c_void, u32, u64),
        context: *mut c_void,
        error: *mut i32,
    ) -> *mut c_void;
    fn md_caption_destroy(caption: *mut c_void);
    fn md_caption_resize(caption: *mut c_void) -> i32;
    fn md_caption_search(caption: *mut c_void);
    fn md_caption_begin(caption: *mut c_void, active: u64);
    fn md_caption_add(
        caption: *mut c_void,
        kind: u32,
        id: u64,
        title: *const u16,
        path: *const u16,
    );
    fn md_caption_commit(caption: *mut c_void);
    fn md_caption_take_drop(token: u64, drop: *mut TabDrop) -> bool;
}

extern "C" fn on_command(context: *mut c_void, command: u32, id: u64) {
    // The native caption owns no application data; all actions return to the Rust event loop.
    let state = unsafe { &*context.cast::<CallbackState>() };
    if command == 7 {
        let mut drop = TabDrop::default();
        if unsafe { md_caption_take_drop(id, &mut drop) } {
            let _ = state.proxy.send_event(UserEvent::TabDrop {
                source: state.window,
                drop,
            });
        }
        return;
    }
    let command = match command {
        1 => format!("activate:{id}"),
        2 => format!("close-tab:{id}"),
        3 => "open-dialog".into(),
        5 => "toggle-toc".into(),
        6 => format!("recent:{id}"),
        8 => "focus-reader".into(),
        9 => format!("press-tab:{id}"),
        10 => "zoom-in".into(),
        11 => "zoom-out".into(),
        12 => "zoom-reset".into(),
        _ => return,
    };
    let _ = state
        .proxy
        .send_event(UserEvent::Command(state.window, command));
}

impl NativeCaption {
    pub fn new(window: &Window, proxy: EventLoopProxy<UserEvent>) -> Result<Self> {
        let mut callback = Box::new(CallbackState {
            window: window.id(),
            proxy,
        });
        let mut error = 0;
        let native = unsafe {
            md_caption_create(
                window.hwnd(),
                on_command,
                (&mut *callback as *mut CallbackState).cast(),
                &mut error,
            )
        };
        let native = std::ptr::NonNull::new(native).with_context(|| {
            format!(
                "无法初始化 Windows App SDK 1.8 自绘标题栏（0x{:08X}）",
                error as u32
            )
        })?;
        Ok(Self {
            native,
            _callback: callback,
        })
    }

    pub fn resize(&self, window: &Window, webview: &WebView) -> Result<()> {
        let height = unsafe { md_caption_resize(self.native.as_ptr()) }.max(1) as u32;
        let size = window.inner_size();
        // Keep the WebView child entirely below the custom caption.
        webview.set_bounds(wry::Rect {
            position: wry::dpi::PhysicalPosition::new(0, height as i32).into(),
            size: wry::dpi::PhysicalSize::new(
                size.width.max(1),
                size.height.saturating_sub(height).max(1),
            )
            .into(),
        })?;
        Ok(())
    }

    pub fn search(&self) {
        unsafe { md_caption_search(self.native.as_ptr()); }
    }

    pub fn begin(&self, active: u64) {
        unsafe {
            md_caption_begin(self.native.as_ptr(), active);
        }
    }

    pub fn add(&self, kind: u32, id: u64, title: &str, path: Option<&Path>) {
        use std::os::windows::ffi::OsStrExt;
        let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
        let path: Vec<u16> = path
            .map(|p| p.as_os_str().encode_wide().collect::<Vec<_>>())
            .unwrap_or_default()
            .into_iter()
            .chain(Some(0))
            .collect();
        unsafe {
            md_caption_add(
                self.native.as_ptr(),
                kind,
                id,
                title.as_ptr(),
                path.as_ptr(),
            );
        }
    }

    pub fn commit(&self) {
        unsafe {
            md_caption_commit(self.native.as_ptr());
        }
    }
}

impl Drop for NativeCaption {
    fn drop(&mut self) {
        unsafe {
            md_caption_destroy(self.native.as_ptr());
        }
    }
}
