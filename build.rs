use std::env;

fn main() {
    println!("cargo:rerun-if-changed=assets/mdviewer.ico");

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        winres::WindowsResource::new()
            .set_icon("assets/mdviewer.ico")
            .set("ProductName", "MDViewer")
            .set("FileDescription", "MDViewer")
            .set("InternalName", "MDViewer.exe")
            .set("OriginalFilename", "MDViewer.exe")
            .compile()
            .expect("failed to embed the MDViewer Windows icon");
    }
}
