use std::sync::LazyLock;
use ammonia::Builder;
use pulldown_cmark::{Options, Parser, html};

const STYLE: &str = include_str!("frontend/style.css");
const SCRIPT: &str = include_str!("frontend/app.js");
const KATEX_SCRIPT: &str = include_str!("../assets/vendor/katex/katex.min.js");

static SANITIZER: LazyLock<Builder<'static>> = LazyLock::new(|| {
    let mut builder = Builder::default();
    builder.add_tags(&["input", "picture", "source", "section"]);
    builder.add_generic_attributes(&["class", "id", "align", "style"]);
    builder.add_tag_attributes("input", &["type", "disabled", "checked"]);
    builder.add_tag_attributes("source", &["srcset", "media", "type"]);
    builder.add_url_schemes(&["data", "mdviewer"]);
    builder
});

fn markdown_options() -> Options {
    Options::ENABLE_TABLES
        | Options::ENABLE_FOOTNOTES
        | Options::ENABLE_STRIKETHROUGH
        | Options::ENABLE_TASKLISTS
        | Options::ENABLE_MATH
        | Options::ENABLE_SMART_PUNCTUATION
}

pub fn markdown_to_html(source: &str) -> String {
    let events = Parser::new_ext(source, markdown_options());
    let mut raw_html = String::new();
    html::push_html(&mut raw_html, events);
    SANITIZER.clean(&raw_html).to_string()
}

pub fn document(markdown: &str, title: &str) -> String {
    let body = markdown_to_html(markdown);
    let safe_title = escape_html(title);

    format!(
        r#"<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src 'self' data: https: http:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; font-src 'self' data:">
  <title>{safe_title}</title>
  <style>{STYLE}</style>
</head>
<body>
  <button id="toc-button" type="button" aria-label="打开目录" title="目录 (Ctrl+Shift+O)">☰</button>
  <div id="toc-backdrop" hidden></div>
  <aside id="toc-panel" aria-label="文档目录" aria-hidden="true">
    <header><strong>目录</strong><button id="toc-close" type="button" aria-label="关闭目录">×</button></header>
    <nav id="toc"></nav>
  </aside>
  <main id="reader"><article id="content" class="markdown-body">{body}</article></main>
  <script>{KATEX_SCRIPT}</script>
  <script>{SCRIPT}</script>
</body>
</html>"#
    )
}

fn escape_html(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for ch in value.chars() {
        match ch {
            '&' => escaped.push_str("&amp;"),
            '<' => escaped.push_str("&lt;"),
            '>' => escaped.push_str("&gt;"),
            '"' => escaped.push_str("&quot;"),
            '\'' => escaped.push_str("&#39;"),
            _ => escaped.push(ch),
        }
    }
    escaped
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_gfm_features() {
        let source = "~~old~~\n\n- [x] done\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\nFootnote[^1]\n\n[^1]: note";
        let rendered = markdown_to_html(source);

        assert!(rendered.contains("<del>old</del>"));
        assert!(rendered.contains("type=\"checkbox\""));
        assert!(rendered.contains("<table>"));
        assert!(rendered.contains("footnote-reference"));
    }

    #[test]
    fn emits_inline_and_display_math_for_katex() {
        let rendered = markdown_to_html("Inline $Q=P^T P$.\n\n$$w^TQw=\\Vert Pw\\rVert_2^2$$");

        assert!(rendered.contains(r#"<span class="math math-inline">Q=P^T P</span>"#));
        assert!(
            rendered.contains(r#"<span class="math math-display">w^TQw=\Vert Pw\rVert_2^2</span>"#)
        );
    }

    #[test]
    fn unsafe_html_is_sanitized() {
        let rendered = markdown_to_html(
            "<script>window.ipc.postMessage('close')</script><img src=\"x\" onerror=\"alert(1)\">",
        );

        assert!(!rendered.contains("<script>"));
        assert!(!rendered.contains("window.ipc.postMessage"));
        assert!(!rendered.contains("onerror"));
    }

    #[test]
    fn safe_html_is_preserved() {
        let source = "<p align=\"center\">\n  <img src=\"assets/mdviewer-icon.png\" width=\"104\" alt=\"icon\" />\n</p>\n\n<h1 align=\"center\">MDViewer</h1>";
        let rendered = markdown_to_html(source);

        assert!(rendered.contains(r#"<p align="center">"#));
        assert!(rendered.contains(r#"<img src="assets/mdviewer-icon.png" width="104" alt="icon">"#));
        assert!(rendered.contains(r#"<h1 align="center">MDViewer</h1>"#));
    }

    #[test]
    fn escapes_document_title() {
        let rendered = document("# Hello", "a < b & c");

        assert!(rendered.contains("<title>a &lt; b &amp; c</title>"));
    }

    #[test]
    fn renders_readme_header() {
        let readme = include_str!("../README.md");
        let rendered = markdown_to_html(readme);

        assert!(rendered.contains(r#"<p align="center">"#));
        assert!(rendered.contains(r#"<img src="assets/mdviewer-icon.png" width="104" alt="MDViewer brand icon">"#));
        assert!(rendered.contains(r#"<h1 align="center">MDViewer</h1>"#));
        assert!(rendered.contains(r#"<strong>面向 Windows 的轻量 Markdown 极速阅读器。</strong>"#));
        assert!(rendered.contains(r#"<img src="screenshots/ss_1.png" width="800" alt="MDViewer 界面截图">"#));
    }
}
