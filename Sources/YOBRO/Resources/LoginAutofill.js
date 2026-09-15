(() => {
  if (location.protocol !== 'https:') return;
  const documentID = crypto.randomUUID();
  const visible = el => el instanceof HTMLInputElement && !el.disabled && !el.readOnly && el.type !== 'hidden' && el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const fields = root => Array.from(root.querySelectorAll('input')).filter(visible);
  const username = inputs => inputs.find(el => el.autocomplete === 'username') || inputs.find(el => el.type === 'email') || inputs.find(el => el.type === 'text' && /user|email|login|identifier/i.test(el.name + ' ' + el.id));
  const currentPassword = inputs => inputs.filter(el => el.type === 'password' && el.autocomplete !== 'new-password');
  let lastGesture = 0;
  let lastCapture = '';
  for (const name of ['pointerdown', 'keydown']) document.addEventListener(name, e => { if (e.isTrusted) lastGesture = performance.now(); }, true);
  const capture = form => {
    if (performance.now() - lastGesture > 1500 || !lastGesture) return;
    if (form && new URL(form.action || location.href, location.href).origin !== location.origin) return;
    const inputs = fields(form || document);
    const passwords = currentPassword(inputs);
    if (inputs.some(el => el.autocomplete === 'new-password') || passwords.length > 1) return;
    const user = username(inputs)?.value || '';
    const password = passwords[0]?.value || '';
    if (!user && !password) return;
    const signature = JSON.stringify([user, password]);
    if (signature === lastCapture) return;
    lastCapture = signature;
    window.webkit.messageHandlers.yobroLogin.postMessage({username: user, password});
  };
  document.addEventListener('focusin', e => {
    if (!e.isTrusted || !(e.target instanceof HTMLInputElement)) return;
    const inputs = fields(document);
    const passwords = currentPassword(inputs);
    if (inputs.some(el => el.autocomplete === 'new-password') || passwords.length > 1) return;
    if (e.target === username(inputs) || passwords.includes(e.target)) {
      window.webkit.messageHandlers.yobroLogin.postMessage({event: 'focus'});
    }
  }, true);
  document.addEventListener('submit', e => capture(e.target), true);
  document.addEventListener('click', e => {
    if (!e.isTrusted) return;
    const button = e.target.closest('button, input[type="submit"]');
    if (button && (button.type === 'submit' || /sign.?in|log.?in|anmelden|weiter|next|continue/i.test(button.textContent || button.value || ''))) capture(button.form);
  }, true);
  const setValue = (el, value) => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, value);
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
  };
  window.yobroLogin = {
    documentID,
    fill(origin, expectedDocument, user, password) {
      if (location.origin !== origin || documentID !== expectedDocument) return false;
      const inputs = fields(document);
      const passwords = currentPassword(inputs);
      if (inputs.some(el => el.autocomplete === 'new-password') || passwords.length > 1) return false;
      const pass = passwords[0];
      const userField = username(pass?.form ? fields(pass.form) : inputs);
      if (!userField && !pass) return false;
      const form = pass?.form || userField?.form;
      if (form && new URL(form.action || location.href, location.href).origin !== location.origin) return false;
      if (userField) setValue(userField, user);
      if (pass) setValue(pass, password);
      return true;
    }
  };
  window.webkit.messageHandlers.yobroLogin.postMessage({event: 'ready', document: documentID});
})();
