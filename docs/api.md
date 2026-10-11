# API

Sunshine has a RESTful API which can be used to interact with the service.

Unless otherwise specified, authentication is required for all API calls. `POST /api/login` with the
admin username and password (and optionally `"remember_me": true`) sets two `HttpOnly` cookies:
`__Host-apollo_session`, presented by every call, and `__Host-apollo_refresh`, which
`POST /api/auth/refresh` trades for a new pair once the session cookie has expired (a call then returns
401). Each refresh spends the refresh cookie it presents.

State-changing calls (`POST`, `PUT`, `PATCH`, `DELETE`) are refused with 403 when their `Origin` (or
`Referer`) is neither the address the web UI is served from nor listed in `csrf_allowed_origins`, unless
they carry an `X-CSRF-Token` header from `GET /api/csrf-token`. Calls without either header (scripts,
curl) are not affected.

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## DELETE /api/apps/{uuid}
@copydoc confighttp::deleteAppByUuid()

## GET /api/apps/{uuid}/cover
@copydoc confighttp::getAppCover()

## DELETE /api/apps/{index}
@copydoc confighttp::deleteApp()

## GET /api/auth/status
@copydoc confighttp::getAuthStatus()

## POST /api/auth/logout
@copydoc confighttp::logout()

## POST /api/auth/refresh
@copydoc confighttp::refreshSession()

## GET /api/auth/sessions
@copydoc confighttp::getSessions()

## DELETE /api/auth/sessions/{id}
@copydoc confighttp::revokeSession()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## PATCH /api/config
@copydoc confighttp::patchConfig()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/csrf-token
@copydoc confighttp::getCSRFToken()

## GET /api/display-devices
@copydoc confighttp::getDisplayDevices()

## GET /api/health/vigem
@copydoc confighttp::getVigemHealth()

## GET /api/history/sessions
@copydoc confighttp::listSessionHistory()

## GET /api/history/sessions/active
@copydoc confighttp::getActiveSessionHistory()

## GET /api/history/sessions/{uuid}
@copydoc confighttp::getSessionHistoryDetail()

## DELETE /api/history/sessions/{uuid}
@copydoc confighttp::deleteSessionHistory()

## GET /api/host/info
@copydoc confighttp::getHostInfo()

## GET /api/host/stats
@copydoc confighttp::getHostStats()

## GET /api/logs
@copydoc confighttp::getLogs()

## GET /api/metadata
@copydoc confighttp::getMetadata()

## POST /api/password
@copydoc confighttp::savePassword()

## POST /api/pin
@copydoc confighttp::savePin()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/restart
@copydoc confighttp::restart()

## GET /api/rtsp/sessions
@copydoc confighttp::listRTSPSessions()

## GET /api/session/status
@copydoc confighttp::getSessionStatus()

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
