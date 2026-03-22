EMACS ?= emacs
PYTHON ?= python3
SHELLCHECK ?= shellcheck

test: checkparens bytecompile checkpy shellcheck

check: test

checkparens:
	$(EMACS) --batch --eval '(find-file "embr.el")' --eval '(check-parens)' && echo "OK: parens balanced"

bytecompile:
	$(EMACS) --batch -L . -f batch-byte-compile embr.el && rm -f embr.elc && echo "OK: embr.el byte-compiles cleanly"

checkpy:
	$(PYTHON) -m py_compile embr.py && echo "OK: embr.py syntax valid"
	$(PYTHON) -m py_compile tools/screencast-gate.py && echo "OK: screencast-gate.py syntax valid"
	$(PYTHON) -m py_compile tools/embr-perf-report.py && echo "OK: embr-perf-report.py syntax valid"
	$(PYTHON) -m py_compile tools/acceptance-gate.py && echo "OK: acceptance-gate.py syntax valid"

shellcheck:
	$(SHELLCHECK) setup.sh tools/run-benchmarks.sh && echo "OK: shell scripts pass shellcheck"

# Native canvas module — requires canvas-patched Emacs + libjpeg-turbo.
module:
	$(MAKE) -C native

# M0 feasibility gate — requires CloakBrowser installed (not in default test).
screencast-gate:
	$(PYTHON) tools/screencast-gate.py

.PHONY: test check checkparens bytecompile checkpy shellcheck module screencast-gate
