<h1 align="center">Markdown Viewer</h1>

<p align="center"><a href="README.md">简体中文</a> · English</p>

<p align="center"><strong>Lightweight Windows Markdown reader with offline math formulas and automatic outline table of contents.</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-f59e0b?style=flat" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/Platform-Windows-0078d4?style=flat" alt="Platform: Windows">
  <img src="https://img.shields.io/badge/Rust-WebView2-dea584?style=flat&amp;logo=rust&amp;logoColor=white" alt="Rust: WebView2">
</p>

Markdown Viewer is a dedicated Windows desktop reader for Markdown documents, designed with minimal visual clutter, high-speed rendering via WebView2, Rust `pulldown-cmark` parsing, KaTeX math rendering, and dark mode.

## Features

- **Instant Rendering**: Native Windows WebView2 shell with low latency.
- **Math & Code**: Built-in offline KaTeX formula rendering and syntax-highlighted code blocks.
- **Table of Contents**: Automatically generated outline navigation.
- **Keyboard Driven**: SumatraPDF-inspired intuitive shortcuts.

## Build & Run

```powershell
cargo build --release
```

## License

[MIT](LICENSE)
