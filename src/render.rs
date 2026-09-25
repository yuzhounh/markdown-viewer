use pulldown_cmark::{Event, Options, Parser, html};

const STYLE: &str = include_str!("frontend/style.css");
const SCRIPT: &str = include_str!("frontend/app.js");
const KATEX_SCRIPT: &str = include_str!("../assets/vendor/katex/katex.min.js");

fn markdown_options() -> Options {
    Options::ENABLE_TABLES
        | Options::ENABLE_FOOTNOTES
        | Options::ENABLE_STRIKETHROUGH
        | Options::ENABLE_TASKLISTS
        | Options::ENABLE_MATH
        | Options::ENABLE_SMART_PUNCTUATION
}

pub fn markdown_to_html(source: &str) -> String {
    // Raw HTML is rendered as text. A viewer commonly opens untrusted files, so
    // arbitrary scripts must never reach WebView2 or its native IPC bridge.
    let events = Parser::new_ext(source, markdown_options()).map(|event| match event {
        Event::Html(raw) | Event::InlineHtml(raw) => Event::Text(raw),
        other => other,
    });

    let mut output = String::new();
    html::push_html(&mut output, events);
    output
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
    fn raw_html_is_inert() {
        let rendered = markdown_to_html("<script>window.ipc.postMessage('close')</script>");

        assert!(!rendered.contains("<script>"));
        assert!(rendered.contains("&lt;script&gt;"));
    }

    #[test]
    fn escapes_document_title() {
        let rendered = document("# Hello", "a < b & c");

        assert!(rendered.contains("<title>a &lt; b &amp; c</title>"));
    }
}
