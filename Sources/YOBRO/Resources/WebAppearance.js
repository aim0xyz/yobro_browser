(() => {
    if (window.__yobroAppearance) return;
    const media = matchMedia('(prefers-color-scheme: dark)');
    let options = {enabled: false, excludedHosts: []};
    let timer, state = 'original';
    const engine = DarkReader;
    // Standard CORS and credentials rules remain in force. No native network proxy.
    engine.setFetchMethod((url) => fetch(url, {credentials: 'omit'}));
    function background(element) {
        const layers = [];
        while (element) {
            const c = getComputedStyle(element).backgroundColor.match(/[\d.]+/g)?.map(Number);
            if (c) {
                const alpha = c.length < 4 ? 1 : c[3];
                layers.push({rgb: c.slice(0, 3), alpha});
                if (alpha === 1) break;
            }
            element = element.parentElement;
        }
        return layers.reverse().reduce((base, {rgb, alpha}) =>
            rgb.map((value, i) => value * alpha + base[i] * (1 - alpha)), [255, 255, 255]);
    }
    function nativeDark() {
        // Sample page surfaces, not merely a dark header or an advertised color-scheme.
        const points = [[.2,.3],[.5,.3],[.8,.3],[.2,.7],[.5,.7],[.8,.7]];
        const colors = points.map(([x,y]) => background(document.elementFromPoint(innerWidth*x, innerHeight*y) || document.body));
        return colors.filter(c => (c[0]*.2126 + c[1]*.7152 + c[2]*.0722) < 105).length >= 4;
    }
    function apply(recheckNative = false) {
        clearTimeout(timer);
        const excluded = options.excludedHosts.includes(location.hostname.toLowerCase());
        if (!options.enabled || !(options.systemDark ?? media.matches) || excluded || !/^https?:$/.test(location.protocol)) {
            if (engine.isEnabled()) engine.disable();
            state = 'original'; return;
        }
        // Keep the existing engine and its stylesheet cache on redundant host updates.
        // Native detection must run with the original styles, never our converted colors.
        if (engine.isEnabled() && !recheckNative) return;
        if (engine.isEnabled()) engine.disable();
        if (document.querySelector('meta[name="darkreader-lock"]') || nativeDark()) { state = 'native'; return; }
        engine.enable({brightness: 100, contrast: 100, sepia: 0, darkSchemeBackgroundColor: '#191e1b', darkSchemeTextColor: '#e3e9e0'}, {
            invert: [], css: '', ignoreInlineStyle: [], ignoreImageAnalysis: ['img', 'video', 'canvas'],
            disableStyleSheetsProxy: true, disableCustomElementRegistryProxy: true
        });
        state = 'converted';
    }
    function schedule() { clearTimeout(timer); timer = setTimeout(() => apply(true), 120); }
    function inheritedOptions() {
        return {...options, enabled: options.enabled && !options.excludedHosts.includes(location.hostname.toLowerCase())};
    }
    function configure(value) {
        options = {enabled: value.enabled === true, excludedHosts: Array.isArray(value.excludedHosts) ? value.excludedHosts : [], systemDark: typeof value.systemDark === 'boolean' ? value.systemDark : undefined};
        apply();
        // Update already loaded cross-origin frames as well; this message grants no privileges.
        for (let i = 0; i < frames.length; i++) frames[i].postMessage({yobroAppearance: inheritedOptions()}, '*');
    }
    window.addEventListener('message', event => {
        if (event.source === parent && window !== parent && event.data?.yobroAppearance) configure(event.data.yobroAppearance);
        if (event.data?.yobroAppearanceRequest && Array.from({length: frames.length}, (_, i) => frames[i]).includes(event.source)) {
            event.source.postMessage({yobroAppearance: inheritedOptions()}, '*');
        }
    });
    media.addEventListener('change', () => {
        // A native override is an initial snapshot, not a permanent media-query lock.
        configure({...options, systemDark: media.matches});
    });
    window.addEventListener('pageshow', schedule);
    window.addEventListener('load', schedule, {once: true});
    // React to native theme switches without observing the styles written by our engine.
    const observer = new MutationObserver(schedule);
    for (const node of [document.documentElement, document.body]) if (node) observer.observe(node, {attributes: true, attributeFilter: ['class', 'data-theme', 'data-color-mode', 'data-dark-mode', 'data-bs-theme', 'data-color-scheme', 'color-scheme']});
    if (window !== parent) parent.postMessage({yobroAppearanceRequest: true}, '*');
    window.__yobroAppearance = {configure, status: () => ({state, systemDark: options.systemDark ?? media.matches, enabled: engine.isEnabled()})};
})();
