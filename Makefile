UNAME  := $(shell uname -s)
CFLAGS  = -O2 -Wall -Wextra -Wno-multichar
ifeq ($(UNAME),Darwin)
CC      = clang
LDFLAGS = -framework IOKit -framework CoreFoundation
else
CC      ?= cc
CFLAGS += -D_GNU_SOURCE
LDFLAGS =
endif

PREFIX  = /usr/local
PLIST   = com.razerpad.fancurve.plist
AGENT   = $(HOME)/Library/LaunchAgents/$(PLIST)

razerpad: razerpad.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: razerpad
	install -d $(PREFIX)/bin
	install -m 755 razerpad $(PREFIX)/bin/razerpad

# Install + start the login agent (runs the fan curve automatically)
agent: install
	sed "s|@BIN@|$(PREFIX)/bin/razerpad|" $(PLIST).in > $(AGENT)
	launchctl unload $(AGENT) 2>/dev/null || true
	launchctl load $(AGENT)
	@echo "Agent loaded. Logs: /tmp/razerpad.log"

unagent:
	launchctl unload $(AGENT) 2>/dev/null || true
	rm -f $(AGENT)

razerpadbar: razerpadbar.swift
	swiftc -O razerpadbar.swift -o razerpadbar

BAR_PLIST = com.razerpad.menubar.plist
BAR_AGENT = $(HOME)/Library/LaunchAgents/$(BAR_PLIST)

# Install + start the menu bar app (temp/RPM readout, fan mode switcher)
bar: razerpadbar
	install -m 755 razerpadbar $(PREFIX)/bin/razerpadbar
	sed "s|@BIN@|$(PREFIX)/bin/razerpadbar|" $(BAR_PLIST).in > $(BAR_AGENT)
	launchctl unload $(BAR_AGENT) 2>/dev/null || true
	launchctl load $(BAR_AGENT)

unbar:
	launchctl unload $(BAR_AGENT) 2>/dev/null || true
	rm -f $(BAR_AGENT)

DASH_PLIST = com.razerpad.dashboard.plist
DASH_AGENT = $(HOME)/Library/LaunchAgents/$(DASH_PLIST)

# Install + start the web dashboard (http://127.0.0.1:8737)
dashboard:
	sed "s|@DIR@|$(CURDIR)|" $(DASH_PLIST).in > $(DASH_AGENT)
	launchctl unload $(DASH_AGENT) 2>/dev/null || true
	launchctl load $(DASH_AGENT)
	@echo "Dashboard: http://127.0.0.1:8737 (logs: /tmp/razerpad-dashboard.log)"

undashboard:
	launchctl unload $(DASH_AGENT) 2>/dev/null || true
	rm -f $(DASH_AGENT)

clean:
	rm -f razerpad

# ── Linux (systemd + udev) ──────────────────────────────────────────────────
UNITDIR   = $(HOME)/.config/systemd/user
UDEVRULE  = /etc/udev/rules.d/70-razerpad.rules
SLEEPHOOK = /usr/lib/systemd/system-sleep/razerpad

# Root: udev rule (non-root access to the pad) + pre-suspend release hook.
udev: install
	install -m 644 linux/70-razerpad.rules $(UDEVRULE)
	sed "s|@BIN@|$(PREFIX)/bin/razerpad|" linux/razerpad-sleep > $(SLEEPHOOK)
	chmod 755 $(SLEEPHOOK)
	udevadm control --reload-rules
	udevadm trigger --subsystem-match=hidraw --action=add

unudev:
	rm -f $(UDEVRULE) $(SLEEPHOOK)
	udevadm control --reload-rules

# User: the fan-curve daemon as a systemd user service. For it to run without
# a login session (headless), also: sudo loginctl enable-linger $$USER
service:
	install -d $(UNITDIR)
	sed "s|@BIN@|$(PREFIX)/bin/razerpad|" linux/razerpad.service.in > $(UNITDIR)/razerpad.service
	systemctl --user daemon-reload
	systemctl --user enable --now razerpad.service
	@echo "Logs: journalctl --user -u razerpad -f"

unservice:
	systemctl --user disable --now razerpad.service 2>/dev/null || true
	rm -f $(UNITDIR)/razerpad.service
	systemctl --user daemon-reload

linux-dashboard:
	install -d $(UNITDIR)
	sed "s|@DIR@|$(CURDIR)|g" linux/razerpad-dashboard.service.in > $(UNITDIR)/razerpad-dashboard.service
	systemctl --user daemon-reload
	systemctl --user enable --now razerpad-dashboard.service
	@echo "Dashboard: http://127.0.0.1:8737"

linux-undashboard:
	systemctl --user disable --now razerpad-dashboard.service 2>/dev/null || true
	rm -f $(UNITDIR)/razerpad-dashboard.service
	systemctl --user daemon-reload

.PHONY: install agent unagent dashboard undashboard clean udev unudev service unservice linux-dashboard linux-undashboard
