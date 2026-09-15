(() => {
  "use strict";

  if (!/(^|\.)youtube\.com$/i.test(location.hostname) || window.__yobroAdBlockPage) return;

  let enabled = false;
  const nativeParse = JSON.parse;
  const nativeResponseJSON = Response.prototype.json;

  function sanitize(value, depth = 0) {
    if (!enabled || !value || typeof value !== "object" || depth > 5) return value;

    // YouTube's ad block detection checks for adPlacements
    // Set them to empty but DON'T delete them (would trigger detection)
    if (value.adPlacements && Array.isArray(value.adPlacements)) {
      value.adPlacements = [];
    }
    
    if (value.playerAds && Array.isArray(value.playerAds)) {
      value.playerAds = [];
    }
    
    if (value.adSlots && Array.isArray(value.adSlots)) {
      value.adSlots = [];
    }

    // CRITICAL: Set these flags to simulate Premium user
    value.adPrerollAllowed = false;
    value.adCp2 = false;
    value.adCp1 = false;
    
    // Remove ad break heartbeat to prevent ad tracking
    delete value.adBreakHeartbeatParams;
    
    // Set player config to indicate premium-like state
    if (value.playerConfig) {
      value.playerConfig.webPlayerConfig = value.playerConfig.webPlayerConfig || {};
      value.playerConfig.webPlayerConfig.useAdBlockerDetection = false;
    }

    // Process nested objects
    if (value.playerResponse && typeof value.playerResponse === "object") {
      sanitize(value.playerResponse, depth + 1);
    }
    if (value.response && typeof value.response === "object") {
      sanitize(value.response, depth + 1);
    }
    
    return value;
  }

  // Intercept JSON.parse
  JSON.parse = function (...args) {
    const result = nativeParse.apply(this, args);
    return sanitize(result);
  };

  // Intercept Response.json
  Response.prototype.json = async function (...args) {
    const result = await nativeResponseJSON.apply(this, args);
    return sanitize(result);
  };

  // Override fetch to intercept YouTube API responses
  const nativeFetch = window.fetch;
  window.fetch = async function(...args) {
    const response = await nativeFetch.apply(this, args);
    
    // Check if this is a YouTube player or watch API call
    const url = args[0]?.url || args[0];
    if (typeof url === 'string' && (url.includes('player?') || url.includes('watch?'))) {
      // Clone the response so we can read it
      const clonedResponse = response.clone();
      try {
        const text = await clonedResponse.text();
        // Modify the response to remove ads
        const modified = text.replace(/"adPlacements":\[[\s\S]*?\]/g, '"adPlacements":[]');
        
        // Return new response with modified content
        return new Response(modified, {
          status: response.status,
          statusText: response.statusText,
          headers: response.headers
        });
      } catch (e) {
        // If parsing fails, return original response
        return response;
      }
    }
    
    return response;
  };

  function sanitizeGlobals() {
    if (!enabled) return;
    try { sanitize(window.ytInitialPlayerResponse); } catch (_) {}
    try { sanitize(window.ytplayer?.config?.args); } catch (_) {}
    try { sanitize(window.ytInitialData); } catch (_) {}
  }

  function setEnabled(value) {
    enabled = Boolean(value);
    if (enabled) {
      sanitizeGlobals();
      
      // Re-run sanitization when page state changes
      const observer = new MutationObserver(() => {
        sanitizeGlobals();
      });
      observer.observe(document.documentElement, { childList: true, subtree: true });
    }
  }

  window.__yobroAdBlockPage = Object.freeze({ setEnabled });
})();
