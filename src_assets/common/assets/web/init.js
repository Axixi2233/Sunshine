import i18n from './locale'
import { handleAuthentication } from './web_auth'

// All pages, including ones using fetch directly, handle expired sessions consistently.
const originalFetch = globalThis.fetch.bind(globalThis);
globalThis.fetch = async (...args) => {
    const response = await originalFetch(...args);
    if (new URL(response.url, location.href).origin === location.origin) {
        handleAuthentication(response, location);
    }
    return response;
};

import 'bootstrap/dist/css/bootstrap.min.css'
// Load Sunshine.css after bootstrap to override some of the styles.
// Makes themes load and style correctly.
import './sunshine.css'

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
import 'bootstrap/dist/js/bootstrap'

export function initApp(app, config) {
    //Wait for locale initialization, then render
    i18n().then(i18n => {
        app.use(i18n);
        app.provide('i18n', i18n.global)
        app.mount('#app');
        if (config) {
            config(app)
        }
    });
}
