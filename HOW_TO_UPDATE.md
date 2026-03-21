# Normally an Update is just M-x elpaca-update && M-x elpaca-rebuild , 0.40 changes a lot under the hood so the following is recommended

### Migrating from 0.30 to 0.40

Version 0.40 replaces the browser engine (Camoufox/Firefox → CloakBrowser/Chromium). A clean install is recommended:

1. On 0.30, run `M-x embr-uninstall` to remove the old venv, browser cache, and profile.
2. Remove the package from Elpaca/straight (delete from your config, restart Emacs, let it re-clone).
3. Install 0.40 fresh and run `M-x embr-setup-or-update-all`.

If you skipped step 1, you can manually remove leftover 0.30 state:

```sh
rm -rf ~/.local/share/embr ~/.cache/camoufox
```
