# Web configuration (companion LAN page)

The companion can serve its settings as a plain web page at `http://<companion-ip>/`, so
tokens and URLs can be pasted from a phone or PC instead of typed on the watch.

## Using it

1. Watch → Settings → Web configuration → **Open for 5 minutes**. The watch shows the URL
   (e.g. `http://192.168.1.109/`) and a countdown; **Close now** ends it early.
2. Open the URL in any browser on the same Wi-Fi network.
3. Edit Wi-Fi, helper, ntfy and Navidrome settings and press **Save**. Secrets (passwords,
   tokens) are never shown; leave them empty to keep the saved value.

The page lists the same whitelisted keys with the same validation as the watch's Settings
→ Server screen (e.g. URLs must be `https://`).

## Security

* Plain HTTP: anyone on the same network can read what you submit and, while the page is
  open, change settings. Use it on a trusted network only, never on public Wi-Fi.
* The HTTP server only runs while the watch has the page open (5 min, at most 10); it is
  stopped afterwards and on companion reboot.
* Cross-site form posts are refused (`Origin` must match the companion's address), so a web
  page in the same browser cannot change settings behind your back.
