// Makes a passkey request fail immediately instead of hanging forever.
//
// Qt WebEngine 6.11.2 accepts `navigator.credentials.create()` and
// `navigator.credentials.get()` with a `publicKey` option, but the returned
// promise never settles and no `webAuthUxRequested` signal is raised. A website
// then waits for a passkey that will never arrive, and its "use a password
// instead" fallback never appears, because that fallback runs in the promise's
// rejection handler.
//
// Rejecting with `NotAllowedError` is what a browser reports when the user
// declines or nothing can answer, so sites treat it as a normal decline and
// offer another way in. Password credentials are not touched: only requests
// that actually ask for a public key are refused.
//
// This whole file is switched off by `QtBrowserProfile::passkeysWork()` as soon
// as Qt raises the UX request, at which point the real dialog takes over.
(function () {
    "use strict";
    if (window.__yobroPasskeyShim) return;

    const credentials = navigator.credentials;
    if (!credentials) return;
    const originalCreate = credentials.create ? credentials.create.bind(credentials) : null;
    const originalGet = credentials.get ? credentials.get.bind(credentials) : null;

    const refuse = function () {
        // The name is what feature detection looks at; the message explains the
        // situation to anyone reading the console.
        const error = new DOMException(
            "This build cannot answer passkey requests. Choose another sign-in method.",
            "NotAllowedError"
        );
        return Promise.reject(error);
    };

    const wrap = function (original) {
        if (!original) return original;
        return function (options) {
            if (options && options.publicKey) return refuse();
            return original(options);
        };
    };

    try {
        if (originalCreate) credentials.create = wrap(originalCreate);
        if (originalGet) credentials.get = wrap(originalGet);
        // `isUserVerifyingPlatformAuthenticatorAvailable` promises a working
        // authenticator, so it has to say no as well; otherwise a site picks the
        // passkey path on purpose.
        if (window.PublicKeyCredential) {
            window.PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable =
                function () { return Promise.resolve(false); };
            if (window.PublicKeyCredential.isConditionalMediationAvailable) {
                window.PublicKeyCredential.isConditionalMediationAvailable =
                    function () { return Promise.resolve(false); };
            }
        }
        window.__yobroPasskeyShim = true;
    } catch (error) {
        // A page that froze `navigator.credentials` keeps the hanging behaviour;
        // nothing here may throw into the page's own scripts.
    }
})();
