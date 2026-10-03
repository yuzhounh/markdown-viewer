(() => {
  'use strict';

  const content = document.getElementById('content');
  const panel = document.getElementById('toc-panel');
  const backdrop = document.getElementById('toc-backdrop');
  const toc = document.getElementById('toc');
  const usedIds = new Map();
  let wheelDelta = 0;

  function requestZoom(command) {
    if (window.ipc) {
      window.ipc.postMessage(command);
    }
  }

  function slug(text) {
    const base = text.trim().toLocaleLowerCase()
      .replace(/[\s\u3000]+/g, '-')
      .replace(/[^\p{Letter}\p{Number}\-_]/gu, '') || 'section';
    const count = usedIds.get(base) || 0;
    usedIds.set(base, count + 1);
    return count ? `${base}-${count + 1}` : base;
  }

  content.querySelectorAll('h1, h2, h3, h4, h5, h6').forEach((heading) => {
    heading.id = heading.id || slug(heading.textContent);
    const link = document.createElement('a');
    link.href = `#${encodeURIComponent(heading.id)}`;
    link.dataset.level = heading.tagName.slice(1);
    link.textContent = heading.textContent;
    link.addEventListener('click', closeToc);
    toc.appendChild(link);
  });

  if (!toc.children.length) {
    const empty = document.createElement('span');
    empty.textContent = '本文档没有标题。';
    empty.style.color = 'var(--muted)';
    empty.style.fontSize = '14px';
    toc.appendChild(empty);
  }

  content.querySelectorAll('.math-inline, .math-display').forEach((element) => {
    const source = element.textContent;
    try {
      katex.render(source, element, {
        displayMode: element.classList.contains('math-display'),
        output: 'mathml',
        throwOnError: true,
        strict: 'ignore',
        trust: false,
      });
    } catch (error) {
      element.classList.add('math-error');
      element.title = error instanceof Error ? error.message : '公式无法解析';
    }
  });

  const COPY_ICON = '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round" aria-hidden="true"><rect x="5.5" y="5.5" width="8" height="8" rx="1.6"/><path d="M10.5 3.5v-.4A1.6 1.6 0 0 0 8.9 1.5H3.1a1.6 1.6 0 0 0-1.6 1.6v5.8a1.6 1.6 0 0 0 1.6 1.6h.4"/></svg>';
  const CHECK_ICON = '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 8.5l3.2 3.2L13 4.8"/></svg>';

  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise((resolve, reject) => {
      const area = document.createElement('textarea');
      area.value = text;
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      const ok = document.execCommand('copy');
      area.remove();
      ok ? resolve() : reject(new Error('copy failed'));
    });
  }

  content.querySelectorAll('pre').forEach((pre) => {
    const wrapper = document.createElement('div');
    wrapper.className = 'code-block';
    pre.replaceWith(wrapper);
    wrapper.appendChild(pre);

    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'copy-button';
    button.title = '复制';
    button.setAttribute('aria-label', '复制');
    button.innerHTML = COPY_ICON;
    let timer = 0;
    button.addEventListener('click', () => {
      const code = pre.querySelector('code') || pre;
      const text = code.textContent.replace(/\n$/, '');
      copyText(text).then(() => true, () => false).then((ok) => {
        button.innerHTML = ok ? CHECK_ICON : COPY_ICON;
        button.classList.toggle('done', ok);
        clearTimeout(timer);
        timer = setTimeout(() => {
          button.innerHTML = COPY_ICON;
          button.classList.remove('done');
        }, 1500);
      });
    });
    wrapper.appendChild(button);
  });

  function openToc() {
    panel.classList.add('open');
    panel.setAttribute('aria-hidden', 'false');
    backdrop.hidden = false;
  }

  function closeToc() {
    panel.classList.remove('open');
    panel.setAttribute('aria-hidden', 'true');
    backdrop.hidden = true;
  }

  document.getElementById('toc-button').addEventListener('click', openToc);
  document.getElementById('toc-close').addEventListener('click', closeToc);
  backdrop.addEventListener('click', closeToc);

  window.addEventListener('wheel', (event) => {
    if (!event.ctrlKey) {
      wheelDelta = 0;
      return;
    }

    event.preventDefault();
    wheelDelta += event.deltaY;
    const threshold = event.deltaMode === WheelEvent.DOM_DELTA_PIXEL ? 80 : 1;
    if (Math.abs(wheelDelta) < threshold) {
      return;
    }

    requestZoom(wheelDelta < 0 ? 'zoom-in' : 'zoom-out');
    wheelDelta = 0;
  }, { passive: false });

  document.addEventListener('keydown', (event) => {
    if (event.ctrlKey && event.shiftKey && event.code === 'KeyO') {
      event.preventDefault();
      panel.classList.contains('open') ? closeToc() : openToc();
      return;
    }

    if (event.ctrlKey && !event.altKey) {
      if (event.key === '+' || event.key === '=' || event.code === 'NumpadAdd') {
        event.preventDefault();
        requestZoom('zoom-in');
        return;
      }
      if (event.key === '-' || event.code === 'NumpadSubtract') {
        event.preventDefault();
        requestZoom('zoom-out');
        return;
      }
      if (event.key === '0' || event.code === 'Numpad0') {
        event.preventDefault();
        requestZoom('zoom-reset');
        return;
      }
    }

    if (event.key === 'Escape') {
      if (panel.classList.contains('open')) {
        closeToc();
      } else if (window.ipc) {
        window.ipc.postMessage('close');
      }
    }
  });

  document.addEventListener('click', (event) => {
    const link = event.target.closest('a');
    const samePageAnchor = link && link.hash && link.origin === location.origin
      && link.pathname === location.pathname;
    if (!link || samePageAnchor) {
      return;
    }

    // Rust validates navigation and hands safe links to the default Windows app.
    event.preventDefault();
    location.href = link.href;
  });
})();
