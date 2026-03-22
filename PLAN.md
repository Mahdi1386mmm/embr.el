# PLAN: In-Buffer Tab Bar

## Context

Add a clickable tab bar below the header line and above the rendered page. Users click a tab label to switch, or click "x" to close. Gated by `embr-tab-bar` defcustom (default nil).

## Architecture Decision: In-Buffer Text

Emacs does not support multiple header lines. `tab-line-format` renders ABOVE the header line, not below. A separate window adds complexity. The simplest approach: insert a propertized text line at the top of the buffer, before the image.

The default backend already does `erase-buffer` + `insert-image` on every frame, so one extra line of text per frame is negligible. The canvas backend updates image data in-place via `image-flush`, so the tab bar line persists without re-insertion.

## Design

### Tab State

- `embr--tab-list` buffer-local var: cached list of tabs from the daemon. Each entry is an alist: `index`, `title`, `url`, `active`.
- Refreshed after tab-affecting commands: `new-tab`, `close-tab`, `switch-tab`.
- Refreshed on startup after init.
- Active tab title updates live via `embr--current-title` (existing metadata push). Other tabs update on next tab operation.

### Rendering

`embr--render-tab-bar` builds a single propertized string from `embr--tab-list`:

```
 Tab Title 1 x | *Active Tab x | Tab Title 3 x
```

- Active tab: bold, distinct background
- Inactive tabs: subdued
- Each label: `embr-tab-index` text property + keymap with `[mouse-1]` -> switch
- Each "x": `embr-tab-index` text property + keymap with `[mouse-1]` -> close
- Titles truncated to ~25 chars
- Pipe separator between tabs
- When only one tab exists, the tab bar still renders (single tab, no ambiguity)

### Click Handling

- `embr--tab-bar-click`: read index from `embr-tab-index` text property at event position, send `switch-tab`, refresh tab list in callback
- `embr--tab-bar-close`: read index from text property, send `close-tab`, refresh tab list in callback

### Frame Integration

**Default backend** (`embr--default-display-frame`):
```elisp
(erase-buffer)
(when (and embr-tab-bar embr--tab-list)
  (insert (embr--render-tab-bar) "\n"))
(insert-image ...)
```

Tab bar is rebuilt from cached `embr--tab-list` on every frame. No extra daemon calls.

**Canvas backend** (`embr--backend-init-canvas`):
```elisp
(erase-buffer)
(when (and embr-tab-bar embr--tab-list)
  (insert (embr--render-tab-bar) "\n"))
(insert (propertize " " 'display embr--canvas-image))
```

Canvas tab bar updates go through `embr--refresh-tab-bar`, which replaces only line 1 when tabs change. Frame blits do not touch the tab bar.

### Tab List Refresh Flow

`embr--refresh-tab-list`:
1. Send `list-tabs` async
2. Callback stores response in `embr--tab-list`
3. Calls `embr--refresh-tab-bar`

`embr--refresh-tab-bar`:
- Default backend: no-op (next frame render rebuilds it from cache)
- Canvas backend: delete line 1, insert new tab bar line (image is on line 2, untouched)

## Changes

### embr.el

**A. New defcustom (after other defcustoms):**
```elisp
(defcustom embr-tab-bar nil
  "Non-nil means show a clickable tab bar above the page."
  :type 'boolean)
```

**B. New faces:**
```elisp
(defface embr-tab-bar
  '((t :background "gray20" :foreground "white"))
  "Face for the tab bar background.")

(defface embr-tab-active
  '((t :inherit embr-tab-bar :weight bold :background "gray40"))
  "Face for the active tab label.")

(defface embr-tab-inactive
  '((t :inherit embr-tab-bar))
  "Face for inactive tab labels.")

(defface embr-tab-close
  '((t :inherit embr-tab-bar :foreground "gray60"))
  "Face for the tab close button.")
```

**C. New buffer-local var:**
```elisp
(defvar embr--tab-list nil "Cached tab list from the daemon.")
```
Make buffer-local in `embr-mode`.

**D. Keymaps for click targets:**
```elisp
(defvar embr--tab-label-map
  (let ((map (make-sparse-keymap)))
    (define-key map [mouse-1] #'embr--tab-bar-click)
    map)
  "Keymap for clickable tab labels.")

(defvar embr--tab-close-map
  (let ((map (make-sparse-keymap)))
    (define-key map [mouse-1] #'embr--tab-bar-close)
    map)
  "Keymap for tab close buttons.")
```

**E. `embr--render-tab-bar`:**
Iterate `embr--tab-list`. For each tab:
- Build label string: truncated title (or URL if title empty)
- Apply `embr-tab-active` or `embr-tab-inactive` face
- Add `embr-tab-index` text property
- Add `keymap` text property (`embr--tab-label-map`)
- Add `mouse-face` for hover
- Append close button "x" with `embr--tab-close-map` keymap and `embr-tab-close` face
- Join with pipe separator

Override active tab title with `embr--current-title` for live updates.

**F. Click handlers:**
```elisp
(defun embr--tab-bar-click (event)
  "Switch to clicked tab."
  (interactive "e")
  ...)

(defun embr--tab-bar-close (event)
  "Close clicked tab."
  (interactive "e")
  ...)
```
Both extract `embr-tab-index` from text property at event position.

**G. `embr--refresh-tab-list`:**
Async `list-tabs` -> store in `embr--tab-list` -> call `embr--refresh-tab-bar`.

**H. `embr--refresh-tab-bar`:**
Canvas: replace line 1 in-place. Default: no-op.

**I. Modify `embr--default-display-frame`:**
Insert tab bar line after `erase-buffer`, before `insert-image`.

**J. Modify `embr--backend-init-canvas`:**
Insert tab bar line before canvas space.

**K. Hook tab commands:**
After `embr-new-tab`, `embr-close-tab`, `embr-next-tab`, `embr-prev-tab` callbacks, call `embr--refresh-tab-list`.

**L. Initial fetch:**
After init in `embr-browse`, if `embr-tab-bar` is non-nil, call `embr--refresh-tab-list`.

### embr.py

No changes. Existing `list-tabs` returns everything needed.

### README.md

- Add `embr-tab-bar` to config table.

## Verification

1. `make test`
2. Set `embr-tab-bar` to t, open embr, verify bar appears
3. Open multiple tabs, verify labels update
4. Click tab labels to switch
5. Click "x" to close tabs
6. Verify both default and canvas backends
7. Verify bar hidden when `embr-tab-bar` is nil
8. Verify single-tab display (no crash, no empty bar)
