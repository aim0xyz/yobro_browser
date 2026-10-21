(() => {
  const handler = window.webkit?.messageHandlers?.yobroSignals;
  if (!handler) return;
  // crypto.randomUUID only exists on secure pages.
  const doc = Math.random().toString(36).slice(2) + Date.now().toString(36);
  const post = message => { try { handler.postMessage(message); } catch (_) {} };

  // Audio: an element is audible while it plays unmuted with some volume.
  const kinds = ['play', 'playing', 'pause', 'ended', 'emptied', 'volumechange'];
  const media = new Set();
  const watched = new WeakSet();
  let audible = false;
  const update = () => {
    let now = false;
    for (const element of media) {
      if (element.paused || element.ended) { media.delete(element); continue; }
      if (!element.muted && element.volume > 0) now = true;
    }
    if (now === audible) return;
    audible = now;
    post({kind: 'audio', doc, audible});
  };
  const track = event => {
    const element = event.target;
    if (!(element instanceof HTMLMediaElement)) return;
    media.add(element);
    if (!watched.has(element)) {
      watched.add(element);
      // Events of an element removed from the document no longer reach the
      // window, so listen on the element as well.
      for (const kind of kinds) element.addEventListener(kind, track);
    }
    update();
  };
  for (const kind of kinds) window.addEventListener(kind, track, true);

  // Links: the destination under the pointer or keyboard focus.
  let link = '';
  const show = href => {
    if (href === link) return;
    link = href;
    post({kind: 'link', href});
  };
  const anchorIn = nodes => nodes.find(node => node instanceof Element && node.matches('a[href], area[href]'));
  const hrefOf = anchor => {
    if (!anchor) return '';
    // SVG links keep their target in an SVGAnimatedString.
    const raw = typeof anchor.href === 'string' ? anchor.href : anchor.href?.baseVal || '';
    try { return new URL(raw, document.baseURI).href; } catch (_) { return ''; }
  };
  const ancestors = node => {
    const path = [];
    while (node) {
      path.push(node);
      node = node instanceof ShadowRoot ? node.host : node.parentNode;
    }
    return path;
  };
  const enter = event => {
    if (event.isTrusted) show(hrefOf(anchorIn(event.composedPath())));
  };
  // Leaving one link for another shows the new one; anything else clears.
  const leave = event => {
    if (event.isTrusted) show(hrefOf(anchorIn(ancestors(event.relatedTarget))));
  };
  window.addEventListener('mouseover', enter, true);
  window.addEventListener('focusin', enter, true);
  window.addEventListener('mouseout', leave, true);
  window.addEventListener('focusout', leave, true);
  // A tab switched away from gets no mouseout; don't bring back a stale link.
  document.addEventListener('visibilitychange', () => { if (document.hidden) show(''); });

  window.addEventListener('pagehide', () => {
    show('');
    media.clear();
    if (audible) { audible = false; post({kind: 'audio', doc, audible}); }
  }, true);
})();
