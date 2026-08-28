// razerpadbar — menu bar readout for the razerpad daemon.
// Shows CPU temp + pad RPM from /tmp/razerpad-status.json and switches the
// fan override (auto / quiet / max) by editing ~/.config/razerpad/config,
// which the daemon hot-reloads.
//
// Build: swiftc -O razerpadbar.swift -o razerpadbar

import AppKit

final class App: NSObject, NSApplicationDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let statusPath = "/tmp/razerpad-status.json"
    private let configPath = ("~/.config/razerpad/config" as NSString).expandingTildeInPath

    private let autoItem = NSMenuItem(title: "Auto (fan curve)", action: #selector(setAuto), keyEquivalent: "")
    private let quietItem = NSMenuItem(title: "Quiet (cap 25%)", action: #selector(setQuiet), keyEquivalent: "")
    private let maxItem = NSMenuItem(title: "Max cool", action: #selector(setMax), keyEquivalent: "")

    func applicationDidFinishLaunching(_ note: Notification) {
        let menu = NSMenu()
        for mi in [autoItem, quietItem, maxItem] {
            mi.target = self
            menu.addItem(mi)
        }
        menu.addItem(.separator())
        let dash = NSMenuItem(title: "Open dashboard", action: #selector(openDashboard), keyEquivalent: "")
        dash.target = self
        menu.addItem(dash)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit razerpadbar", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "")
        menu.addItem(quit)
        item.menu = menu

        update()
        Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.update()
        }
    }

    private func readStatus() -> [String: Any]? {
        guard let data = FileManager.default.contents(atPath: statusPath),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        // Treat a stale file (daemon stopped) the same as no file.
        if let ts = obj["ts"] as? Double, Date().timeIntervalSince1970 - ts > 20 {
            return nil
        }
        return obj
    }

    private func update() {
        let status = readStatus()
        let connected = status?["connected"] as? Bool ?? false
        if let s = status, connected,
           let temp = s["temp"] as? Double, let rpm = s["rpm"] as? Int {
            item.button?.title = String(format: "%.0f° %d", temp, rpm)
        } else if let s = status, let temp = s["temp"] as? Double {
            item.button?.title = String(format: "%.0f° ⏏", temp) // pad unplugged
        } else {
            item.button?.title = "razerpad ⏻" // daemon not running
        }
        item.button?.font = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium)

        let override = status?["override"] as? String ?? ""
        autoItem.state = (override != "quiet" && override != "max") ? .on : .off
        quietItem.state = override == "quiet" ? .on : .off
        maxItem.state = override == "max" ? .on : .off
    }

    private func writeOverride(_ value: String) {
        var lines: [String] = []
        if let text = try? String(contentsOfFile: configPath, encoding: .utf8) {
            lines = text.split(separator: "\n", omittingEmptySubsequences: true)
                .map(String.init)
                .filter { !$0.hasPrefix("override=") }
        }
        lines.append("override=\(value)")
        let dir = (configPath as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try? (lines.joined(separator: "\n") + "\n")
            .write(toFile: configPath, atomically: true, encoding: .utf8)
        update()
    }

    @objc private func setAuto() { writeOverride("off") }
    @objc private func setQuiet() { writeOverride("quiet") }
    @objc private func setMax() { writeOverride("max") }

    @objc private func openDashboard() {
        NSWorkspace.shared.open(URL(string: "http://127.0.0.1:8737/")!)
    }
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
