use std::env;

fn main() {
    println!("cargo:rerun-if-changed=assets/mdviewer.ico");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        winres::WindowsResource::new()
            .set_icon("assets/mdviewer.ico")
            .set("ProductName", "Markdown Viewer")
            .set("FileDescription", "Markdown Viewer")
            .set("InternalName", "MarkdownViewer.exe")
            .set("OriginalFilename", "MarkdownViewer.exe")
            .compile()
            .expect("failed to embed the MarkdownViewer Windows icon");
    }
}
