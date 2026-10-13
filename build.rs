use base64::{Engine, engine::general_purpose::STANDARD};
use std::{env, fs, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=assets/mdviewer.ico");
    embed_katex_fonts();

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

fn embed_katex_fonts() {
    let vendor = PathBuf::from("assets/vendor/katex");
    println!("cargo:rerun-if-changed=assets/vendor/katex/katex.min.css");
    println!("cargo:rerun-if-changed=assets/vendor/katex/fonts");
    let mut css =
        fs::read_to_string(vendor.join("katex.min.css")).expect("failed to read KaTeX stylesheet");
    let prefix = "src:url(fonts/";
    let mut position = 0;
    while let Some(offset) = css[position..].find(prefix) {
        let start = position + offset;
        let font_start = start + prefix.len();
        let font_end = font_start + css[font_start..].find(')').expect("invalid KaTeX font URL");
        let name = &css[font_start..font_end];
        assert!(name.ends_with(".woff2"), "expected a KaTeX WOFF2 font");
        let bytes = fs::read(vendor.join("fonts").join(name)).expect("failed to read KaTeX font");
        let end = start
            + css[start..]
                .find('}')
                .expect("invalid KaTeX font-face declaration");
        // WebView2 supports WOFF2; omit the unused WOFF/TTF fallbacks.
        let source = format!(
            "src:url(data:font/woff2;base64,{}) format(\"woff2\")",
            STANDARD.encode(bytes)
        );
        css.replace_range(start..end, &source);
        position = start + source.len();
    }
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("missing build output directory"));
    fs::write(output.join("katex.min.css"), css)
        .expect("failed to write embedded KaTeX stylesheet");
}
