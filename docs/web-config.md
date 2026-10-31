# Web Bluetooth configuration app

`web/` builds to static files (`build/web/`) for Cloudflare Pages: no backend, no secrets in
the bundle, strict CSP (`web/public/_headers`, `connect-src 'none'`).

## Deploy (Cloudflare Pages)

```bash
scripts/sw build web               # -> build/web
npx wrangler pages deploy build/web --project-name sw-setup   # or connect the repo in the dashboard:
#   build command: cd web && npm ci && npm run build   output: build/web
```

## Using it

1. Companion: power-cycle it (or press BOOT) to open the 5-minute pairing window. The
   passkey is fixed to `616616` for the prototype and also printed on its serial console (`BLE pairing passkey: 616616`).
   Watch: Settings → Web configuration (advertises for 5 minutes).
2. Open the page, tap **Connect device**, pick the device, accept the pairing prompt and
   enter/confirm the code.
3. Edit Wi-Fi, server/Navidrome settings (companion) or notification/vibration/display/time
   zone (watch). Secrets are write-only.

## Browser support

* **iPhone: Safari and every WebKit browser lack Web Bluetooth.** Use **Bluefy**, a
  third-party browser that bridges Web Bluetooth to CoreBluetooth. The page must be served
  over HTTPS (Cloudflare Pages is).
* Desktop/Android: Chrome, Edge, Opera.

Known/assumed Bluefy limitations and how the app copes (not yet verified on a device):

| Limitation | Mitigation |
|------------|------------|
| ATT MTU not exposed | RX writes are chunked to 20 bytes |
| `writeValueWithResponse` may be missing in older versions | falls back to `writeValue()` |
| Pairing is performed by iOS when an encrypted characteristic is first used; stale iOS bonds after reflashing a device cause immediate disconnects | "Forget This Device" in iOS Settings → Bluetooth, then reconnect |
| No `getDevices()`/background reconnection | the user taps Connect each visit (by design: user gesture required) |
| Reports of anecdotal disconnect-after-pairing issues | toggle iOS Bluetooth, unpair, retry |

Sources: Bluefy App Store listing; third-party compatibility notes. Re-test after Bluefy updates.
