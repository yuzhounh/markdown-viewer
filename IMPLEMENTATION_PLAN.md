# MDViewer implementation plan

目标是做一个“SumatraPDF 的使用方式 + WebView2 的 Markdown 排版质量”的 Windows 阅读器。每一步保持可独立验证，暂不引入编辑功能。

## P0 — 可运行 MVP（当前）

- [x] Rust + tao/wry + WebView2 外壳
- [x] 命令行路径打开 UTF-8 Markdown
- [x] pulldown-cmark 解析 GFM 表格、任务列表、删除线、脚注
- [x] 内嵌 CSS/JS，无 Node.js 和前端构建链
- [x] 深浅色排版、自动目录、外链和相对图片
- [x] 基础安全边界：转义原始 HTML、限制本地资源到文档目录
- [x] 在 Windows MSVC 环境完成 `cargo test`、release 编译和 WebView2 启动验证

完成标准：`MDViewer.exe sample.md` 可稳定打开，所有单元测试通过，关闭窗口无残留进程。

## P1 — 真正双击即看

- [ ] 增加应用图标和 Windows version resource
- [ ] 提供按用户安装/卸载的 `.md` 文件关联脚本
- [ ] 处理第二次启动传入文件（单实例可延后）
- [ ] 非 UTF-8 文件给出可理解的错误页面

完成标准：安装后双击 `.md` 能打开，卸载文件关联可完整恢复。

## P2 — 日常阅读流

- [ ] `Ctrl+O` 文件选择
- [ ] 文件拖放打开
- [ ] 文件变化自动刷新并保持滚动位置
- [ ] 字号与正文宽度设置
- [ ] 记忆窗口位置、最近文件和阅读位置

## P3 — 高级 Markdown

- [ ] Rust 侧代码高亮，避免远程 CDN
- [ ] KaTeX 数学公式
- [ ] Mermaid 图表
- [ ] 图片点击放大与保存

## 明确不做（现阶段）

- WYSIWYG / Muya 编辑器
- 多标签和工作区
- 云同步、账号系统、插件市场
- 打包 Chromium 或 Node.js runtime
