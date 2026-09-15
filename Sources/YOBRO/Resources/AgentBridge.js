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
    const interactive = nodes.filter(el => visible(el) && el.matches('a[href],button,input,textarea,select,[role="button"],[role="link"],[role="textbox"],[contenteditable="true"],summary'));
    return {
      document: documentID, url: location.href, title: document.title,
      ready: document.readyState,
      text: (document.body?.innerText || '').slice(0, 30000),
      headings: nodes.filter(el => visible(el) && /^H[1-3]$/.test(el.tagName)).slice(0, 100).map(el => ({level: +el.tagName[1], text: el.innerText})),
      elements: interactive.slice(0, 500).map(el => ({
        ref: ref(el), tag: el.tagName.toLowerCase(), role: el.getAttribute('role'), label: label(el),
        type: el.getAttribute('type'), href: el.href || null,
        value: el.type === 'password' ? '[redacted]' : (el.value ?? null),
        disabled: !!el.disabled, checked: typeof el.checked === 'boolean' ? el.checked : null,
        options: el.tagName === 'SELECT' ? [...el.options].map(o => ({value: o.value, label: o.text})) : undefined
      })),
      frames: nodes.filter(el => el.tagName === 'IFRAME').map(el => ({src: el.src, title: el.title, note: 'Frame content is not included.'})),
      truncated: interactive.length > 500 || (document.body?.innerText.length || 0) > 30000
    };
  }
  function act(request) {
    if (request.document !== documentID) throw new Error('The page changed. Read a fresh snapshot before acting.');
    const el = references.get(request.ref);
    if (!el?.isConnected) throw new Error('Element is stale. Read a fresh snapshot.');
    if (!visible(el) || el.disabled) throw new Error('Element is hidden or disabled.');
    el.scrollIntoView({block: 'center', behavior: 'instant'});
    if (request.action === 'click') el.click();
    else if (request.action === 'fill') {
      if (el.readOnly) throw new Error('Field is read-only.');
      if (el.isContentEditable) { el.focus(); el.textContent = request.value; }
      else {
        if (!['INPUT', 'TEXTAREA', 'SELECT'].includes(el.tagName)) throw new Error('Element is not a text field or select.');
        if (['file', 'checkbox', 'radio', 'submit', 'button', 'reset', 'image'].includes(el.type)) throw new Error('Use click for this control; file uploads are not supported.');
        const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : el.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
        Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, request.value);
        el.focus();
      }
      el.dispatchEvent(new Event('input', {bubbles: true}));
      el.dispatchEvent(new Event('change', {bubbles: true}));
    } else throw new Error('Unsupported action.');
    return {acted: request.action, ref: request.ref, document: documentID};
  }
  globalThis.__yobro = {snapshot, act};
})();
