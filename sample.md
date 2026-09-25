# MDViewer 阅读样例

这是一个以 **Viewer first** 为原则的轻量 Markdown 阅读器。它使用 Rust 读取和解析文件，再交给 Windows WebView2 排版。

> 产品目标：像打开 PDF 一样双击 Markdown，然后安静地阅读。

## 当前能力

- [x] GFM 表格
- [x] task list
- [x] ~~删除线~~
- [x] 脚注[^1]
- [x] 自动深浅色
- [ ] KaTeX 与 Mermaid（后续阶段）

## 中英文排版

正文采用适合 Windows 的中英文系统字体栈。English words, `inline code`, 标点和长段落可以自然混排。

```rust
fn main() {
    println!("Hello, Markdown!");
}
```

## 表格

| 方向 | 选择 | 原因 |
| --- | --- | --- |
| 桌面外壳 | Rust + tao/wry | 小而直接 |
| Markdown | pulldown-cmark | Rust 侧解析 |
| 排版 | WebView2 | CSS、表格和复杂布局成熟 |

## 快捷键

- `Ctrl+Shift+O`：打开或关闭目录
- `Ctrl+F`：页内查找
- `Ctrl++` / `Ctrl+-`：缩放
- `Esc`：关闭目录；目录关闭时退出程序

[^1]: 脚注也由 Rust 侧 Markdown 解析器生成。
