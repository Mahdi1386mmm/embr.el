# PLAN: Proxy Support (SOCKS5 / HTTP)

## Context

Add proxy support to embr. Two defcustoms: `embr-proxy-type` (symbol, `'socks` or `'http`) and `embr-proxy-address` (string, host:port). Header line shows a red "PROXY" badge when the session is proxied. Proxy is a launch-time setting (Playwright constraint), so it applies per-session, not toggled mid-session.

Covers Tor (`socks`, `127.0.0.1:9050`), I2P (`http`, `127.0.0.1:4444`), or any other SOCKS5/HTTP proxy.

## Design

- **`embr-proxy-type` defcustom**: symbol, default `nil`. `'socks` or `'http`. When nil, no proxy.
- **`embr-proxy-address` defcustom**: string, default `"127.0.0.1:9050"`. The host:port of the proxy. Only used when `embr-proxy-type` is non-nil.
- **`embr--proxy-active` buffer-local var**: the full proxy URL the current session was launched with (e.g. `"socks5://127.0.0.1:9050"`), or nil. Used for header line badge and buffer naming.
- **Header line badge**: red "PROXY" badge when `embr--proxy-active` is non-nil.
- **No separate entry point**: user sets `embr-proxy-type` and `embr-proxy-address` before launching. To switch, quit, change, relaunch.

## Changes

### 1. embr.el

**A. New defcustoms (after other defcustoms):**
```elisp
(defcustom embr-proxy-type nil
  "Proxy type for browser traffic.
When non-nil, embr routes all traffic through the proxy at
`embr-proxy-address'."
  :type '(choice (const :tag "No proxy" nil)
                 (const :tag "SOCKS5" socks)
                 (const :tag "HTTP" http)))

(defcustom embr-proxy-address "127.0.0.1:9050"
  "Proxy host:port.  Only used when `embr-proxy-type' is non-nil."
  :type 'string)
```

**B. New buffer-local var (near `embr--incognito-flag`):**
```elisp
(defvar embr--proxy-active nil "Non-nil when this session uses a proxy.")
```
Make buffer-local in `embr-mode` (init to nil).

**C. Build proxy URL and pass in init params:**
In `embr--build-init-params`, when `embr-proxy-type` is non-nil, build the full URL and include it:
```elisp
(when embr-proxy-type
  (let ((scheme (pcase embr-proxy-type
                  ('socks "socks5")
                  ('http "http"))))
    (push (cons 'proxy (format "%s://%s" scheme embr-proxy-address)) params)))
```

**D. Set `embr--proxy-active` after successful init:**
In both `embr-browse` and `embr-browse-incognito`, after the init handshake succeeds:
```elisp
(when embr-proxy-type
  (setq embr--proxy-active t))
```

**E. Header line badge (after INCOGNITO badge, ~line 2247):**
```elisp
(when embr--proxy-active
  (propertize " PROXY " 'face '(:background "red" :foreground "white")))
```

**F. Buffer rename logic:**
In `embr--update-metadata`, extend the buffer name prefix to show proxy when active (alongside incognito check).

**G. URL history:**
Proxy sessions should not record URL history (same as incognito). Add `embr--proxy-active` checks alongside the existing `embr--incognito-flag` checks in `embr-navigate`.

### 2. embr.py

**A. Init handler -- accept proxy param:**
In the `cmd == "init"` block, after `color_scheme` handling (~line 611):
```python
proxy = params.get("proxy")
if proxy:
    context_opts["proxy"] = {"server": proxy}
    print(f"embr: proxy={proxy}", file=sys.stderr)
```

That's it on the Python side. No env vars, no new module-level state.

### 3. README.md

- Add `embr-proxy-type` and `embr-proxy-address` to the configuration table.
- Brief usage note about proxy support (Tor/I2P examples).

## Verification

1. `make test` (checkparens, bytecompile, checkpy, shellcheck)
2. Manual: set `embr-proxy-type` to `'socks`, launch, verify traffic routes through proxy
3. Verify header line shows red "PROXY" badge
4. Verify nil type launches without proxy (no badge)
5. Verify proxy works with incognito sessions
