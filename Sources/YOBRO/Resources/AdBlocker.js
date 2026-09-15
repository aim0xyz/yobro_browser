(() => {
  "use strict";

  const isYouTube = /(^|\.)youtube\.com$/i.test(location.hostname);
  let enabled = false;
  let aggressive = false;
  let observer = null;
  let timer = null;
  let style = null;
  let changedPlaybackRate = false;
  let layoutRefresh = null;
  let lastSkippedShortAd = null;
  let detectionCheckTimer = null;

  const selectors = [
    "ins.adsbygoogle",
    "iframe[id^='google_ads_']",
    "iframe[src*='doubleclick.net']",
    "[data-ad-client]",
    "[data-ad-slot]",
    "[aria-label='Advertisement']",
    "[aria-label='Werbung']",
    ".advertisement",
    ".advertising",
    ".ad-container",
    ".ad-wrapper",
    ".sponsored-content",
    "[id^='taboola-']",
    ".taboola",
    ".OUTBRAIN",
    "ytd-ad-slot-renderer",
    "ytd-in-feed-ad-layout-renderer",
    "ytd-banner-promo-renderer",
    "ytd-promoted-sparkles-web-renderer",
    "ytd-display-ad-renderer",
    "ytd-promoted-video-renderer",
    "#masthead-ad",
    "#player-ads",
    ".ytp-ad-overlay-container",
    ".ytp-ad-message-container"
  ];
  const feedAdSelectors = [
    "ytd-ad-slot-renderer",
    "ytd-in-feed-ad-layout-renderer",
    "ytd-banner-promo-renderer",
    "ytd-promoted-sparkles-web-renderer",
    "ytd-display-ad-renderer",
    "ytd-promoted-video-renderer"
  ];
  const emptyAdWrappers = [
    "ytd-rich-item-renderer:has(ytd-ad-slot-renderer)",
    "ytd-rich-item-renderer:has(ytd-in-feed-ad-layout-renderer)",
    "ytd-rich-item-renderer:has(ytd-display-ad-renderer)",
    "ytd-rich-item-renderer:has(ytd-promoted-video-renderer)",
    "ytd-video-renderer:has(ytd-promoted-sparkles-web-renderer)"
  ];

  function ensureStyle() {
    if (style?.isConnected) return;
    style = document.createElement("style");
    style.id = "yobro-adblock-style";
    style.textContent = `
      ${selectors.join(",")} { display: none !important; }
      ${emptyAdWrappers.join(",")} { display: none !important; margin: 0 !important; }
    `;
    (document.head || document.documentElement)?.appendChild(style);
  }

  function compactHomeGrid() {
    if (!enabled || !aggressive || !isYouTube) return;
    const rows = [...document.querySelectorAll("ytd-browse[page-subtype='home'] ytd-rich-grid-row")];
    for (let index = 0; index < rows.length; index += 1) {
      const row = rows[index].querySelector(":scope > #contents") || rows[index].querySelector("#contents");
      if (!row) continue;
      const capacity = Number.parseInt(getComputedStyle(document.documentElement)
        .getPropertyValue("--ytd-rich-grid-items-per-row"), 10) ||
        Math.max(1, ...rows.map(candidate => candidate.querySelectorAll(":scope > #contents > ytd-rich-item-renderer").length));

      while (row.querySelectorAll(":scope > ytd-rich-item-renderer").length < capacity) {
        const nextRow = rows.slice(index + 1).find(candidate =>
          candidate.querySelector(":scope > #contents > ytd-rich-item-renderer")
        );
        const nextItem = nextRow?.querySelector(":scope > #contents > ytd-rich-item-renderer");
        if (!nextItem) break;
        row.appendChild(nextItem);
      }
    }
    rows.forEach(row => {
      if (!row.querySelector(":scope > #contents > ytd-rich-item-renderer")) row.remove();
    });
    window.dispatchEvent(new Event("resize"));
  }

  function scheduleGridCompaction() {
    if (layoutRefresh) clearTimeout(layoutRefresh);
    layoutRefresh = setTimeout(() => {
      layoutRefresh = null;
      compactHomeGrid();
    }, 80);
  }

  function skipShortsAd() {
    if (!aggressive) return;
    const reel = document.querySelector([
      "ytd-reel-video-renderer[is-active]",
      "ytd-reel-video-renderer[active]",
      "ytd-shorts[is-active] ytd-reel-video-renderer"
    ].join(","));
    if (!reel) return;

    const explicitAdMarker = reel.matches("[is-ad], [data-is-ad='true'], .ad-showing") || Boolean(reel.querySelector([
      "ytd-ad-slot-renderer",
      "ytd-in-feed-ad-layout-renderer",
      "[is-ad]",
      "[data-is-ad='true']",
      ".ytp-ad-player-overlay",
      ".ytp-ad-text",
      ".ytp-ad-preview-container"
    ].join(",")));
    const adLabel = [...reel.querySelectorAll("#metadata-line, #metapanel, yt-formatted-string, button, a")]
      .some(element => /^(anzeige|gesponsert|sponsored|ad|learn more|mehr erfahren|shop now|jetzt kaufen)$/i
        .test((element.textContent || "").trim()));
    if (!explicitAdMarker && !adLabel) return;

    const identity = reel.getAttribute("video-id") || reel.querySelector("a[href*='/shorts/']")?.href || reel;
    if (identity === lastSkippedShortAd) return;
    lastSkippedShortAd = identity;

    const video = reel.querySelector("video");
    if (video) video.muted = true;
    const next = document.querySelector([
      "#navigation-button-down button",
      "button[aria-label='Next video']",
      "button[aria-label='Nächstes Video']",
      "button[aria-label='Next']",
      "button[aria-label='Weiter']"
    ].join(","));
    if (next) next.click();
    else window.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", code: "ArrowDown", bubbles: true }));
    setTimeout(() => { lastSkippedShortAd = null; }, 1200);
  }

  function handleAdBlockerDetection() {
    if (!enabled || !isYouTube) return;
    
    // Aggressively remove any enforcement/detection overlays
    const selectorsToRemove = [
      "ytd-enforcement-message-view-model",
      ".ytd-enforcement-message-view-model", 
      "tp-yt-iron-overlay-backdrop",
      "ytd-popup-container",
      ".ytd-popup-container",
      "ytd-mealbar-promo-renderer",
      "ytd-confirm-dialog-renderer",
      "yt-confirm-dialog-renderer",
      "yt-feedback-confirmation"
    ];
    
    selectorsToRemove.forEach(selector => {
      document.querySelectorAll(selector).forEach(element => {
        // Check if it's an ad blocker warning by content
        const text = (element.textContent || "").toLowerCase();
        if (text.includes("werbeblocker") || text.includes("ad blocker") || 
            text.includes("adblock") || text.includes("werbung") ||
            text.includes("nutzungsbedingungen") || text.includes("terms of service") ||
            text.includes("premium") || text.includes("video playback") ||
            text.includes("videowiedergabe")) {
          element.remove();
        }
      });
    });
    
    // Remove any overlay backdrop
    document.querySelectorAll("tp-yt-iron-overlay-backdrop, .iron-overlay-backdrop").forEach(el => el.remove());
    
    // Re-enable the player if it was disabled
    const player = document.querySelector("#movie_player, #player-container");
    if (player) {
      player.style.removeProperty("pointer-events");
      player.style.removeProperty("opacity");
      player.style.removeProperty("display");
      player.classList.remove("blocked");
      
      // Remove any disabled attributes
      player.removeAttribute("disabled");
    }
    
    // Re-enable the video element
    const video = document.querySelector("video");
    if (video) {
      video.style.removeProperty("pointer-events");
      video.style.removeProperty("opacity");
      video.style.removeProperty("display");
      video.removeAttribute("disabled");
      
      // Try to resume playback if paused due to detection
      if (video.paused && !video.ended && video.currentTime > 0) {
        try { 
          video.play().catch(() => {}); 
        } catch (_) {}
      }
    }
    
    // Remove any "ad-blocker detected" flags from the page
    if (window.ytplayer && window.ytplayer.config) {
      window.ytplayer.config.args = window.ytplayer.config.args || {};
      window.ytplayer.config.args.ad_device = 0;
      window.ytplayer.config.args.ad_flags = 0;
    }
    
    // Dispatch events that might help unblock the player
    window.dispatchEvent(new Event("resize"));
    document.dispatchEvent(new Event("DOMContentLoaded"));
  }
  
  // Mobile User-Agent Spoofing for YouTube
  // YouTube doesn't show ad blocker warnings on mobile devices
  function spoofMobileUserAgent() {
    if (!enabled || !isYouTube) return;
    
    // Override navigator.userAgent for YouTube
    const mobileUA = "Mozilla/5.0 (Linux; Android 10; SM-G981B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";
    
    try {
      Object.defineProperty(navigator, 'userAgent', {
        get: function() { return mobileUA; },
        configurable: true
      });
      
      Object.defineProperty(navigator, 'platform', {
        get: function() { return 'Linux armv81'; },
        configurable: true
      });
      
      Object.defineProperty(navigator, 'maxTouchPoints', {
        get: function() { return 5; },
        configurable: true
      });
    } catch (e) {
      // Already defined, ignore
    }
  }

  function cleanYouTubeAds() {
    if (!enabled || !isYouTube) return;
    ensureStyle();
    
    // First, handle any ad blocker detection popups
    handleAdBlockerDetection();

    // YouTube wraps feed ads in ordinary grid items. Removing only the inner ad
    // renderer leaves a black/empty tile, so remove that dedicated wrapper too.
    document.querySelectorAll(feedAdSelectors.join(",")).forEach(element => {
      const wrapper = element.closest("ytd-rich-item-renderer, ytd-video-renderer");
      if (wrapper) {
        wrapper.remove();
        scheduleGridCompaction();
      }
      else element.remove();
    });
    document.querySelectorAll(selectors.filter(selector => !feedAdSelectors.includes(selector)).join(","))
      .forEach(element => element.remove());
    if (!aggressive) return;
    skipShortsAd();

    const player = document.querySelector("#movie_player");
    const adPlaying = player?.classList.contains("ad-showing") || player?.classList.contains("ad-interrupting");
    if (!adPlaying) {
      const video = document.querySelector("video");
      if (changedPlaybackRate && video) video.playbackRate = 1;
      changedPlaybackRate = false;
      return;
    }

    document.querySelector([
      ".ytp-ad-skip-button-modern",
      ".ytp-ad-skip-button",
      ".videoAdUiSkipButton",
      "button[id^='skip-button']"
    ].join(","))?.click();

    const video = document.querySelector("video");
    if (!video) return;
    video.muted = true;
    video.playbackRate = 16;
    changedPlaybackRate = true;
    if (Number.isFinite(video.duration) && video.duration > 0) {
      video.currentTime = Math.max(video.currentTime, video.duration - 0.05);
    }
  }

  function cleanPage() {
    if (!enabled) return;
    ensureStyle();
    if (isYouTube) {
      spoofMobileUserAgent();
      cleanYouTubeAds();
      // Run detection handler frequently
      handleAdBlockerDetection();
    }
  }

  function configure(value, aggressiveValue) {
    enabled = Boolean(value);
    aggressive = Boolean(aggressiveValue);
    if (enabled) {
      ensureStyle();
      cleanPage();
      if (!observer) {
        var queued = false;
        observer = new MutationObserver(() => {
          if (queued) return;
          queued = true;
          requestAnimationFrame(() => { queued = false; cleanPage(); });
        });
        observer.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ["class"] });
      }
      if (isYouTube && aggressive && !timer) timer = setInterval(cleanPage, 500);
      if ((!isYouTube || !aggressive) && timer) { clearInterval(timer); timer = null; }
    } else {
      observer?.disconnect();
      observer = null;
      if (timer) clearInterval(timer);
      timer = null;
      if (layoutRefresh) clearTimeout(layoutRefresh);
      layoutRefresh = null;
      style?.remove();
      style = null;
      const video = document.querySelector("video");
      if (changedPlaybackRate && video) video.playbackRate = 1;
      changedPlaybackRate = false;
    }
  }

  window.__yobroAdBlock = Object.freeze({ configure });
  window.webkit?.messageHandlers?.yobroAdBlock?.postMessage("ready");
})();
