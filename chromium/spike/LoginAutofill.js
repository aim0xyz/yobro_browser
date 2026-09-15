// Runs in an isolated world, so page scripts can neither read nor call it.
// Events are pushed to the shell over the QWebChannel object that is bound to
// this world; the shell no longer polls. fill() is still only ever called by
// the shell. This script never submits a form and never sends credentials
// anywhere on its own.
(() => {
  if (window.top !== window || location.protocol !== 'https:') return;
  const documentId = (crypto.randomUUID && crypto.randomUUID())
    || String(Date.now()) + Math.random().toString(16).slice(2);

  // The channel handshake is asynchronous, so the first events of a page are
  // held back and flushed once the shell is reachable. The queue is tiny on
  // purpose: an unread event is not worth keeping around.
  let host = null;
  const queued = [];
  const send = event => {
    const payload = JSON.stringify(Object.assign({origin: location.origin, documentId}, event));
    if (!host) {
      queued.push(payload);
      if (queued.length > 4) queued.shift();
      return;
    }
    host.report(payload);
  };
  try {
    new QWebChannel(qt.webChannelTransport, channel => {
      host = (channel.objects && channel.objects.yobroLoginHost) || null;
      if (!host) return;
      while (queued.length) host.report(queued.shift());
    });
  } catch (error) {
    // Without a channel the page simply gets no credential handling. Never
    // throw into the page, and never fall back to leaking events elsewhere.
  }

  const visible = el => el instanceof HTMLInputElement
    && !el.disabled && !el.readOnly && el.type !== 'hidden'
    && el.getClientRects().length > 0
    && getComputedStyle(el).visibility !== 'hidden';
  const fields = root => Array.from(root.querySelectorAll('input')).filter(visible);
  const usernameField = inputs => inputs.find(el => el.autocomplete === 'username')
    || inputs.find(el => el.type === 'email')
    || inputs.find(el => el.type === 'text' && /user|email|login|identifier/i.test(el.name + ' ' + el.id));
  const currentPasswords = inputs => inputs.filter(
    el => el.type === 'password' && el.autocomplete !== 'new-password'
  );
  // Signup and password-change forms are deliberately out of scope.
  const unsupported = inputs => inputs.some(el => el.autocomplete === 'new-password')
    || currentPasswords(inputs).length > 1;
  const sameOriginForm = form => {
    if (!form) return true;
    try {
      return new URL(form.action || location.href, location.href).origin === location.origin;
    } catch (error) {
      return false;
    }
  };

  let lastFocusReport = 0;
  let lastGesture = 0;
  let lastCapture = '';
  for (const name of ['pointerdown', 'keydown']) {
    document.addEventListener(name, event => {
      if (event.isTrusted) lastGesture = performance.now();
    }, true);
  }

  document.addEventListener('focusin', event => {
    if (!event.isTrusted || !(event.target instanceof HTMLInputElement)) return;
    const inputs = fields(document);
    if (unsupported(inputs)) return;
    const passwords = currentPasswords(inputs);
    if (event.target !== usernameField(inputs) && !passwords.includes(event.target)) return;
    // Tabbing through a form must not push one event per field.
    if (lastFocusReport && performance.now() - lastFocusReport < 400) return;
    lastFocusReport = performance.now();
    send({kind: 'focus'});
  }, true);

  const capture = form => {
    // Only collect right after a real click or key press by the user.
    if (!lastGesture || performance.now() - lastGesture > 1500) return;
    if (!sameOriginForm(form)) return;
    const inputs = fields(form || document);
    if (unsupported(inputs)) return;
    const user = usernameField(inputs)?.value || '';
    const password = currentPasswords(inputs)[0]?.value || '';
    if (!user || !password) return;
    const signature = JSON.stringify([user, password]);
    if (signature === lastCapture) return;
    lastCapture = signature;
    send({kind: 'save', username: user, password});
  };

  document.addEventListener('submit', event => {
    if (event.isTrusted) capture(event.target);
  }, true);
  document.addEventListener('click', event => {
    if (!event.isTrusted) return;
    const button = event.target.closest('button, input[type="submit"]');
    if (!button) return;
    const label = button.textContent || button.value || '';
    if (button.type === 'submit' || /sign.?in|log.?in|anmelden|weiter|next|continue/i.test(label))
      capture(button.form);
  }, true);

  const setValue = (el, value) => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, value);
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
  };

  window.__yobroLogin = {
    documentId,
    fill(origin, expectedDocument, user, password) {
      if (location.origin !== origin || documentId !== expectedDocument) return false;
      const inputs = fields(document);
      if (unsupported(inputs)) return false;
      const passwordInput = currentPasswords(inputs)[0];
      const scope = passwordInput?.form ? fields(passwordInput.form) : inputs;
      const userInput = usernameField(scope);
      if (!passwordInput && !userInput) return false;
      const form = passwordInput?.form || userInput?.form;
      if (!sameOriginForm(form)) return false;
      if (userInput) setValue(userInput, user);
      if (passwordInput) setValue(passwordInput, password);
      // Filling never submits; the user stays in control of the form.
      return true;
    }
  };
})();
