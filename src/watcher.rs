use std::{
    fs, io,
    path::Path,
    sync::mpsc::{self, Receiver, RecvTimeoutError, Sender},
    thread::{self, JoinHandle},
    time::Duration,
};

use notify::{Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};

use crate::ChangeDebouncer;

const SETTLE_TIME: Duration = Duration::from_millis(200);
const MAX_READ_ATTEMPTS: usize = 10;

enum Message {
    Changed,
    Stop,
}

pub struct DocumentWatcher {
    watcher: Option<RecommendedWatcher>,
    sender: Sender<Message>,
    worker: Option<JoinHandle<()>>,
}

impl Drop for DocumentWatcher {
    fn drop(&mut self) {
        let _ = self.sender.send(Message::Stop);
        self.watcher.take();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

pub fn start(
    path: &Path,
    initial: String,
    reload: impl FnMut(String) -> bool + Send + 'static,
) -> notify::Result<DocumentWatcher> {
    let source = path.to_owned();
    start_with_reader(path, initial, move || fs::read_to_string(&source), reload)
}

fn start_with_reader(
    path: &Path,
    initial: String,
    read: impl FnMut() -> io::Result<String> + Send + 'static,
    reload: impl FnMut(String) -> bool + Send + 'static,
) -> notify::Result<DocumentWatcher> {
    let (sender, receiver) = mpsc::channel();
    let events = sender.clone();
    let source = path.to_owned();
    let mut watcher = notify::recommended_watcher(move |result: notify::Result<Event>| {
        match result {
            Ok(event) if affects_document(&event, &source) => {
                let _ = events.send(Message::Changed);
            }
            Err(error) => {
                eprintln!("文件变化监听异常: {error}");
                // Reconcile once after an error; do not fall back to idle polling.
                let _ = events.send(Message::Changed);
            }
            _ => {}
        }
    })?;
    // Watch the directory so deleting/replacing the file does not detach the watch.
    watcher.watch(
        path.parent().unwrap_or(Path::new(".")),
        RecursiveMode::NonRecursive,
    )?;
    let worker = thread::spawn(move || run(receiver, initial, read, reload));
    Ok(DocumentWatcher {
        watcher: Some(watcher),
        sender,
        worker: Some(worker),
    })
}

fn affects_document(event: &Event, path: &Path) -> bool {
    event.need_rescan()
        || (!matches!(event.kind, EventKind::Access(_))
            && event.paths.iter().any(|changed| {
                // The watch is non-recursive and belongs to this file's directory.
                match (changed.file_name(), path.file_name()) {
                    (Some(a), Some(b)) => {
                        a.to_string_lossy().to_lowercase() == b.to_string_lossy().to_lowercase()
                    }
                    _ => false,
                }
            }))
}

fn run(
    receiver: Receiver<Message>,
    initial: String,
    mut read: impl FnMut() -> io::Result<String>,
    mut reload: impl FnMut(String) -> bool,
) {
    let mut changes = ChangeDebouncer::new(initial);
    // One startup reconciliation closes the gap between initial load and watch setup.
    let mut attempts = MAX_READ_ATTEMPTS;
    loop {
        let message = if attempts == 0 {
            receiver.recv().map_err(|_| RecvTimeoutError::Disconnected)
        } else {
            receiver.recv_timeout(SETTLE_TIME)
        };
        match message {
            Ok(Message::Stop) | Err(RecvTimeoutError::Disconnected) => break,
            Ok(Message::Changed) => {
                attempts = MAX_READ_ATTEMPTS;
                changes.pending = None;
            }
            Err(RecvTimeoutError::Timeout) => {
                attempts -= 1;
                match read() {
                    Ok(current) => {
                        if let Some(markdown) = changes.observe(current) {
                            if !reload(markdown) {
                                break;
                            }
                        }
                        if changes.pending.is_none() {
                            attempts = 0;
                        }
                    }
                    Err(_) => {
                        // A save may temporarily remove, lock, or partially encode a file.
                        // Keep the last rendered document and retry only this save burst.
                        changes.pending = None;
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    struct Fixture(std::path::PathBuf);
    impl Fixture {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let dir = std::env::temp_dir().join(format!(
                "mdviewer-watch-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&dir).unwrap();
            Self(dir)
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn native_events_reload_edits_and_replacements_without_idle_reads() {
        let fixture = Fixture::new();
        let path = fixture.0.join("document.md");
        fs::write(&path, "old").unwrap();
        let reads = Arc::new(AtomicUsize::new(0));
        let count = reads.clone();
        let source = path.clone();
        let (read_tx, read_rx) = mpsc::channel();
        let (tx, rx) = mpsc::channel();
        let watcher = start_with_reader(
            &path,
            "old".into(),
            move || {
                count.fetch_add(1, Ordering::SeqCst);
                let value = fs::read_to_string(&source);
                let _ = read_tx.send(());
                value
            },
            move |s| tx.send(s).is_ok(),
        )
        .unwrap();
        read_rx.recv_timeout(Duration::from_secs(3)).unwrap();
        let baseline = reads.load(Ordering::SeqCst);
        fs::write(fixture.0.join("unrelated.md"), "ignore me").unwrap();
        thread::sleep(Duration::from_millis(650));
        assert_eq!(
            reads.load(Ordering::SeqCst),
            baseline,
            "idle/unrelated changes must not read the document"
        );

        fs::write(&path, "new").unwrap();
        assert_eq!(rx.recv_timeout(Duration::from_secs(4)).unwrap(), "new");
        fs::write(&path, "new").unwrap();
        assert!(
            rx.recv_timeout(Duration::from_millis(650)).is_err(),
            "same content must not reload"
        );

        let replacement = fixture.0.join("save.tmp");
        fs::write(&replacement, "replaced").unwrap();
        fs::remove_file(&path).unwrap();
        thread::sleep(Duration::from_millis(250));
        fs::rename(&replacement, &path).unwrap();
        assert_eq!(rx.recv_timeout(Duration::from_secs(4)).unwrap(), "replaced");
        fs::write(&path, "").unwrap();
        assert_eq!(rx.recv_timeout(Duration::from_secs(4)).unwrap(), "");
        drop(watcher);
    }

    #[test]
    fn locked_save_recovers_and_startup_gap_is_reconciled() {
        use std::{io::Write, os::windows::fs::OpenOptionsExt};
        let fixture = Fixture::new();
        let path = fixture.0.join("document.md");
        fs::write(&path, "saved before watch setup").unwrap();
        let (tx, rx) = mpsc::channel();
        let watcher = start(&path, "initial load".into(), move |s| tx.send(s).is_ok()).unwrap();
        assert_eq!(
            rx.recv_timeout(Duration::from_secs(4)).unwrap(),
            "saved before watch setup"
        );
        let mut file = fs::OpenOptions::new()
            .write(true)
            .truncate(true)
            .share_mode(0)
            .open(&path)
            .unwrap();
        file.write_all(b"locked save").unwrap();
        file.flush().unwrap();
        assert!(rx.recv_timeout(Duration::from_millis(650)).is_err());
        drop(file);
        assert_eq!(
            rx.recv_timeout(Duration::from_secs(4)).unwrap(),
            "locked save"
        );
        drop(watcher);
    }
}
