import i18n from './locale'

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
import 'bootstrap/dist/js/bootstrap'

// Web UI logins (src/web_auth.h): the session cookie lasts two hours, the refresh cookie up to a
// week with "Remember me". An API call answered 401 trades the refresh cookie for new ones, then is
// retried once. Concurrent 401s share one refresh: each refresh spends the cookie it presents, so a
// second one in flight would fail and sign the browser out.
const nativeFetch = window.fetch.bind(window)
let refreshing = null

function refreshLogin() {
    if (!refreshing) {
        refreshing = nativeFetch('./api/auth/refresh', { method: 'POST', credentials: 'include' })
            .then(res => res.ok)
            .catch(() => false)
            .finally(() => setTimeout(() => { refreshing = null }, 0))
    }
    return refreshing
}

window.fetch = async (input, init) => {
    const res = await nativeFetch(input, init)
    const url = typeof input === 'string' ? input : input.url
    if (res.status !== 401 || /\/api\/(login|auth\/)/.test(url)) {
        return res
    }
    if (await refreshLogin()) {
        return nativeFetch(input, init)
    }
    // The login is over: back to the login page, then here
    if (!/\/(login|welcome)\/?$/.test(location.pathname)) {
        location.href = './login?redir=.' + location.pathname + location.hash
    }
    return res
}

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
