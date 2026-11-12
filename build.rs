use base64::{Engine, engine::general_purpose::STANDARD};
use std::{env, fs, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=assets/mdviewer.ico");
    embed_katex_fonts();

    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        build_native_caption();
        let manifest = format!(r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="MarkdownViewer" version="{}.{}.{}.0" />
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*" />
  </dependentAssembly></dependency>
</assembly>"#, env!("CARGO_PKG_VERSION_MAJOR"), env!("CARGO_PKG_VERSION_MINOR"), env!("CARGO_PKG_VERSION_PATCH"));
        winres::WindowsResource::new()
            .set_manifest(&manifest)
            .set_icon("assets/mdviewer.ico")
            .set("ProductName", "Markdown Viewer")
            .set("FileDescription", "Markdown Viewer")
            .set("InternalName", "MarkdownViewer.exe")
            .set("OriginalFilename", "MarkdownViewer.exe")
            .compile()
            .expect("failed to embed the MarkdownViewer Windows icon");
    }
}

fn build_native_caption() {
    use std::process::Command;
    println!("cargo:rerun-if-changed=src/native_caption.cpp");
    println!("cargo:rerun-if-changed=src/tab_layout.h");
    let packages = PathBuf::from(env::var_os("USERPROFILE").expect("missing user profile"))
        .join(".nuget/packages");
    let foundation = packages.join("microsoft.windowsappsdk.foundation/1.8.260803002");
    let interactive = packages.join("microsoft.windowsappsdk.interactiveexperiences/1.8.260708001");
    let kits =
        PathBuf::from(env::var_os("ProgramFiles(x86)").expect("missing Windows Kits location"))
            .join("Windows Kits/10");
    let mut versions: Vec<_> = fs::read_dir(kits.join("bin"))
        .expect("Windows SDK is required")
        .filter_map(Result::ok)
        .map(|entry| entry.file_name())
        .collect();
    versions.sort();
    let version = versions
        .into_iter()
        .rev()
        .find(|version| {
            kits.join("bin")
                .join(version)
                .join("x64/cppwinrt.exe")
                .is_file()
        })
        .expect("C++/WinRT compiler is required");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let generated = out.join("caption-winrt");
    fs::create_dir_all(&generated).unwrap();
    if !generated.join("winrt/Microsoft.UI.Windowing.h").exists() {
        let status = Command::new(kits.join("bin").join(&version).join("x64/cppwinrt.exe"))
            .arg("-input")
            .arg(interactive.join("metadata/10.0.18362.0"))
            .args(["-reference", "sdk", "-output"])
            .arg(&generated)
            .status()
            .expect("could not generate Windows App SDK headers");
        assert!(
            status.success(),
            "Windows App SDK projection generation failed"
        );
    }
    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .flag("/EHsc")
        .flag("/utf-8")
        .define("NOMINMAX", None)
        .define("WIN32_LEAN_AND_MEAN", None)
        .include(&generated)
        .include(kits.join("Include").join(&version).join("cppwinrt"))
        .include(interactive.join("include"))
        .include(foundation.join("include"))
        .file("src/native_caption.cpp")
        .compile("mdviewer_caption");
    println!(
        "cargo:rustc-link-search=native={}",
        foundation.join("lib/native/x64").display()
    );
    for library in [
        "Microsoft.WindowsAppRuntime.Bootstrap",
        "windowsapp",
        "gdiplus",
        "user32",
        "gdi32",
        "advapi32",
        "comctl32",
    ] {
        println!("cargo:rustc-link-lib={library}");
    }
    // The bootstrap uses the installed Windows App Runtime, just as xRemote's native Tall caption.
    let binary_dir = out.ancestors().nth(3).unwrap();
    let bootstrap =
        foundation.join("runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll");
    fs::copy(
        &bootstrap,
        binary_dir.join("Microsoft.WindowsAppRuntime.Bootstrap.dll"),
    )
    .unwrap();
    let deps = binary_dir.join("deps");
    fs::create_dir_all(&deps).unwrap();
    fs::copy(
        &bootstrap,
        deps.join("Microsoft.WindowsAppRuntime.Bootstrap.dll"),
    )
    .unwrap();
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
