// Runs in an isolated content/application world, never on a page-exposed window object.
(() => {
  if (globalThis.__yobro) return;
  const makeDocumentID = () => typeof crypto.randomUUID === 'function' ? crypto.randomUUID()
    : Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, '0')).join('');
  let documentID = makeDocumentID();
  let references = new Map();
  let identities = new WeakMap();
  let next = 0;
  const resetDocumentIdentity = () => {
    documentID = makeDocumentID();
    references = new Map();
    identities = new WeakMap();
    next = 0;
  };
  addEventListener('pageshow', event => {
    if (event.persisted) resetDocumentIdentity();
  });
  const ref = element => {
    if (!identities.has(element)) {
      const id = `e${++next}`;
      identities.set(element, id);
    }
    const id = identities.get(element);
    references.set(id, element);
    return id;
  };
  const visible = el => {
    const style = getComputedStyle(el);
    return !!el.getClientRects().length && style.visibility !== 'hidden' && style.display !== 'none';
  };
  const disabled = el => {
    if (el.matches(':disabled')) return true;
    for (let node = el; node; node = node.parentElement || node.getRootNode()?.host) {
      if (node.inert || node.getAttribute('aria-disabled') === 'true') return true;
    }
    return false;
  };
  function all(root = document) {
    const elements = [];
    for (const el of root.querySelectorAll('*')) {
      elements.push(el);
      if (el.shadowRoot) elements.push(...all(el.shadowRoot));
    }
    return elements;
  }
  function label(el) {
    const root = el.getRootNode();
    const labelledBy = (el.getAttribute('aria-labelledby') || '').split(' ').map(id =>
      root.getElementById?.(id)?.textContent || document.getElementById(id)?.textContent || ''
    ).join(' ').trim();
    return (el.getAttribute('aria-label') || labelledBy ||
      [...(el.labels || [])].map(l => l.innerText).join(' ') ||
      el.innerText || el.getAttribute('alt') || el.getAttribute('placeholder') || el.getAttribute('title') || el.name || '').trim().slice(0, 250);
  }
  function snapshot() {
    for (const [id, element] of references) if (!element.isConnected) references.delete(id);
    const nodes = all();
    const interactive = nodes.filter(el => visible(el) && el.matches('a[href],button,input,textarea,select,[role="button"],[role="link"],[role="textbox"],[role="searchbox"],[role="combobox"],[role="checkbox"],[role="radio"],[role="switch"],[role="tab"],[role="menuitem"],[contenteditable="true"],summary,[tabindex]:not([tabindex="-1"])'));
    return {
      document: documentID, url: location.href, title: document.title,
      ready: document.readyState,
      viewport: {width: innerWidth, height: innerHeight, scrollX, scrollY,
        scrollWidth: document.scrollingElement?.scrollWidth ?? 0,
        scrollHeight: document.scrollingElement?.scrollHeight ?? 0},
      media: nodes.filter(el => el.matches('video,audio')).slice(0, 20).map(el => ({
        tag: el.tagName.toLowerCase(), paused: el.paused, ended: el.ended,
        currentTime: el.currentTime, duration: Number.isFinite(el.duration) ? el.duration : null,
        readyState: el.readyState, networkState: el.networkState,
        muted: el.muted, volume: el.volume, playbackRate: el.playbackRate,
        videoWidth: el.videoWidth || 0, videoHeight: el.videoHeight || 0,
        decodedFrames: el.getVideoPlaybackQuality?.().totalVideoFrames ?? null,
        error: el.error ? {code: el.error.code, message: el.error.message} : null
      })),
      text: (document.body?.innerText || '').slice(0, 30000),
      headings: nodes.filter(el => visible(el) && /^H[1-3]$/.test(el.tagName)).slice(0, 100).map(el => ({level: +el.tagName[1], text: el.innerText})),
      elements: interactive.slice(0, 500).map(el => ({
        ref: ref(el), tag: el.tagName.toLowerCase(), role: el.getAttribute('role'), label: label(el),
        type: el.getAttribute('type'), href: el.href || null,
        value: el.type === 'password' ? '[redacted]' : (el.value ?? (el.isContentEditable ? el.innerText : null)),
        disabled: disabled(el), checked: typeof el.checked === 'boolean' ? el.checked : null,
        options: el.tagName === 'SELECT' ? [...el.options].map(o => ({value: o.value, label: o.text})) : undefined
      })),
      frames: nodes.filter(el => el.tagName === 'IFRAME').map(el => ({src: el.src, title: el.title, note: 'Frame content is not included.'})),
      truncated: interactive.length > 500 || (document.body?.innerText.length || 0) > 30000
    };
  }
  function act(request) {
    if (request.document !== documentID) throw new Error('The page changed. Read a fresh snapshot before acting.');
    const el = references.get(request.ref);
    if (!el) throw new Error('Unknown element ref. Read a fresh snapshot and use one of its refs.');
    if (!el.isConnected) {
      throw new Error('The page changed. Read a fresh snapshot before acting.');
    }
    if (!visible(el) || disabled(el)) throw new Error('Element is hidden or disabled.');
    el.scrollIntoView({block: 'center', behavior: 'instant'});
    if (request.action === 'click') {
      el.focus();
      const opts = {bubbles: true, cancelable: true, view: window};
      try { el.dispatchEvent(new PointerEvent('pointerdown', opts)); } catch (_) {}
      try { el.dispatchEvent(new MouseEvent('mousedown', opts)); } catch (_) {}
      try { el.dispatchEvent(new PointerEvent('pointerup', opts)); } catch (_) {}
      try { el.dispatchEvent(new MouseEvent('mouseup', opts)); } catch (_) {}
      el.click();
    } else if (request.action === 'fill') {
      if (el.readOnly) throw new Error('Field is read-only.');
      el.focus();
      if (el.isContentEditable) {
        try {
          const selection = window.getSelection();
          const range = document.createRange();
          range.selectNodeContents(el);
          selection.removeAllRanges();
          selection.addRange(range);
        } catch (_) {}
        let inserted = false;
        try {
          inserted = document.execCommand('insertText', false, request.value);
        } catch (_) {}
        if (!inserted || (el.innerText || el.textContent || '').trim() !== request.value.trim()) {
          el.innerText = request.value;
          try { el.dispatchEvent(new InputEvent('beforeinput', {bubbles: true, cancelable: true, inputType: 'insertText', data: request.value})); } catch (_) {}
          try { el.dispatchEvent(new InputEvent('input', {bubbles: true, cancelable: true, inputType: 'insertText', data: request.value})); } catch (_) {}
          el.dispatchEvent(new Event('change', {bubbles: true}));
        }
      } else if (el.tagName === 'SELECT') {
        const options = [...el.options];
        // Values are case-sensitive identifiers. Fall back to a unique visible label.
        let opt = options.find(o => o.value === request.value);
        if (!opt) {
          const matches = options.filter(o => o.text.trim().toLowerCase() === String(request.value).trim().toLowerCase());
          if (matches.length !== 1) throw new Error('Select option is missing or ambiguous. Read the available options first.');
          opt = matches[0];
        }
        if (opt.disabled || opt.parentElement?.disabled) throw new Error('Select option is disabled.');
        el.value = opt.value;
        el.dispatchEvent(new Event('input', {bubbles: true}));
        el.dispatchEvent(new Event('change', {bubbles: true}));
      } else if (['INPUT', 'TEXTAREA'].includes(el.tagName)) {
        if (['file', 'checkbox', 'radio', 'submit', 'button', 'reset', 'image', 'hidden'].includes(el.type)) throw new Error('Use click for this control; file uploads are not supported via fill.');
        const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
        const setter = Object.getOwnPropertyDescriptor(proto, 'value')?.set;
        if (!el.dispatchEvent(new InputEvent('beforeinput', {bubbles: true, cancelable: true, inputType: 'insertText', data: request.value}))) {
          throw new Error('The page prevented editing this field.');
        }
        if (setter) setter.call(el, request.value); else el.value = request.value;
        try { el.dispatchEvent(new InputEvent('input', {bubbles: true, cancelable: true, inputType: 'insertText', data: request.value})); } catch (_) {}
        el.dispatchEvent(new Event('change', {bubbles: true}));
      } else {
        throw new Error('Element is not a text field, editor, or select.');
      }
    } else if (request.action === 'press') {
      el.focus();
      const keyName = request.key || 'Enter';
      const code = keyName === 'Enter' ? 'Enter' : keyName === 'Tab' ? 'Tab' : keyName === 'Escape' ? 'Escape' : keyName;
      const keyCode = keyName === 'Enter' ? 13 : keyName === 'Tab' ? 9 : keyName === 'Escape' ? 27 : keyName === 'Backspace' ? 8 : 0;
      const keyOpts = {key: keyName, code: code, keyCode: keyCode, which: keyCode, bubbles: true, cancelable: true, view: window};
      const allowed = el.dispatchEvent(new KeyboardEvent('keydown', keyOpts));
      const pressAllowed = allowed && el.dispatchEvent(new KeyboardEvent('keypress', keyOpts));
      el.dispatchEvent(new KeyboardEvent('keyup', keyOpts));
      // A cancelled keyboard event must never cause an extra form submission.
      if (allowed && pressAllowed && keyName === 'Enter' && el.tagName === 'INPUT' && !['button', 'reset', 'checkbox', 'radio', 'file', 'image'].includes(el.type)) {
        const form = el.form || el.closest('form');
        if (form) {
          try {
            if (typeof form.requestSubmit === 'function') form.requestSubmit(el.type === 'submit' ? el : undefined);
          } catch (_) {}
        }
      }
    } else throw new Error('Unsupported action.');
    return {acted: request.action, ref: request.ref, document: documentID};
  }
  globalThis.__yobro = {snapshot, act};
})();
