(function () {
    'use strict';

    const STORAGE_KEY = 'osmw-accent-scheme';
    const SCHEMES = new Set(['amber', 'blue', 'green']);

    function normalize(value) {
        return SCHEMES.has(value) ? value : 'amber';
    }

    function apply(value, persist) {
        const scheme = normalize(value);
        document.documentElement.dataset.accent = scheme;
        if (persist) {
            try { localStorage.setItem(STORAGE_KEY, scheme); } catch (_) { }
        }
        return scheme;
    }

    const queryScheme = new URLSearchParams(location.search).get('accent');
    let storedScheme = '';
    try { storedScheme = localStorage.getItem(STORAGE_KEY) || ''; } catch (_) { }
    apply(queryScheme || storedScheme || 'amber', !!queryScheme);

    window.AccentTheme = {
        apply: (value) => apply(value, true),
        preview: (value) => apply(value, false),
        normalize,
    };

    window.addEventListener('storage', (event) => {
        if (event.key === STORAGE_KEY && event.newValue) apply(event.newValue, false);
    });
}());
