# Web management login

The management UI uses `/login` instead of a browser HTTP authentication prompt.
Existing Sunshine usernames and passwords continue to work; first-run setup remains at `/welcome`.

`POST /api/login` accepts JSON containing `username` and `password`. Successful login issues an opaque 256-bit token in a `sunshine_session` cookie with `Path=/`, `Secure`, `HttpOnly`, and `SameSite=Strict`. Passwords are never persisted in browser storage. Sessions are held only in memory, expire after 12 hours, and are invalidated when credentials change or Sunshine restarts. Storage is limited to 256 sessions.

`POST /api/logout` revokes the presented session and clears the cookie. Both endpoints require JSON and reject cross-origin browser requests. Existing CSRF protection continues to protect management operations. Protected pages redirect to login; unauthenticated APIs return 401 without `WWW-Authenticate`. Browser requests use sessions; non-browser clients may continue sending explicit Basic authentication headers.

Only known management paths are accepted as post-login return destinations. Protected HTML and authentication responses are not cached, and cookies are redacted in request logs.

Validation includes `WebSessions.*` and `ConfigHttpTest.*` in `test_sunshine`, `node --test tests/web_auth.test.mjs`, and the Vite production build. The new executable and matching WebUI assets must be deployed together for the change to appear in an installed Sunshine instance.
