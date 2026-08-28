CC      = clang
CFLAGS  = -O2 -Wall -Wextra -Wno-multichar
LDFLAGS = -framework IOKit -framework CoreFoundation

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

.PHONY: install agent unagent dashboard undashboard clean
