import AppKit
import CDFN
import CoreAudio
import ServiceManagement
import Sparkle

private let visibleUID = "Stillmic:visible:48k2ch"
private let engineUID = "Stillmic:engine:48k2ch"
private let installedDriver = "/Library/Audio/Plug-Ins/HAL/Stillmic.driver"

// `--no-audio` shows the menu without touching audio devices or login items, for UI and memory checks.
private let noAudio = CommandLine.arguments.contains("--no-audio")

// MARK: - Core Audio helpers

private func address(_ selector: AudioObjectPropertySelector,
                     _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}

private func stringProperty(_ id: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
    var a = address(selector)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, &value) == noErr, let value else { return nil }
    return value.takeRetainedValue() as String
}

private func device(uid: String) -> AudioObjectID {
    var a = address(kAudioHardwarePropertyTranslateUIDToDevice)
    var cfUID = uid as CFString
    var id = AudioObjectID(0)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let st = withUnsafePointer(to: &cfUID) {
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a,
                                   UInt32(MemoryLayout<CFString>.size), $0, &size, &id)
    }
    return st == noErr ? id : 0
}

private struct Mic {
    let id: AudioObjectID
    let uid: String
    let name: String
    let builtIn: Bool
}

private func inputDevices() -> [Mic] {
    let system = AudioObjectID(kAudioObjectSystemObject)
    var a = address(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(system, &a, 0, nil, &size) == noErr else { return [] }
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(system, &a, 0, nil, &size, &ids) == noErr else { return [] }

    return ids.compactMap { id in
        var streams = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)
        var streamBytes: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &streams, 0, nil, &streamBytes) == noErr, streamBytes > 0,
              let uid = stringProperty(id, kAudioDevicePropertyDeviceUID), uid != visibleUID,
              let name = stringProperty(id, kAudioObjectPropertyName) else { return nil }
        var t = address(kAudioDevicePropertyTransportType)
        var transport: UInt32 = 0
        var n = UInt32(MemoryLayout<UInt32>.size)
        AudioObjectGetPropertyData(id, &t, 0, nil, &n, &transport)
        // Aggregates can contain Stillmic, which would feed the output back into the input.
        if transport == kAudioDeviceTransportTypeAggregate || transport == kAudioDeviceTransportTypeVirtual { return nil }
        return Mic(id: id, uid: uid, name: name, builtIn: transport == kAudioDeviceTransportTypeBuiltIn)
    }
}

private func isRunningSomewhere(_ id: AudioObjectID) -> Bool {
    var a = address(kAudioDevicePropertyDeviceIsRunningSomewhere)
    var value: UInt32 = 0
    var n = UInt32(MemoryLayout<UInt32>.size)
    return AudioObjectGetPropertyData(id, &a, 0, nil, &n, &value) == noErr && value != 0
}

// MARK: - App

final class Controller: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let defaults = UserDefaults.standard
    private let pipeline: OpaquePointer
    private var statusItem: NSStatusItem!
    private var updater: SPUStandardUpdaterController?

    private var visible = AudioObjectID(0)
    private var activeMic: Mic?
    private var activeEngine = AudioObjectID(0)
    private var failed = false
    private var retryScheduled = false

    private var visibleListener: AudioObjectPropertyListenerBlock?
    private var micListener: AudioObjectPropertyListenerBlock?
    private var devicesListener: AudioObjectPropertyListenerBlock?
    private var restartListener: AudioObjectPropertyListenerBlock?

    private var enabled: Bool {
        get { defaults.object(forKey: "enabled") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "enabled") }
    }

    private var aggressive: Bool {
        get { defaults.bool(forKey: "aggressive") }
        set { defaults.set(newValue, forKey: "aggressive") }
    }

    init(pipeline: OpaquePointer) {
        self.pipeline = pipeline
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
        if !CommandLine.arguments.contains("--no-updater") {
            updater = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        }
        if noAudio { return updateIcon() }

        // Stillmic is silent while this app isn't running, and idling costs nothing, so start at
        // login by default. Done once, so turning it off in the menu sticks.
        if defaults.object(forKey: "loginConfigured") == nil {
            defaults.set(true, forKey: "loginConfigured")
            try? SMAppService.mainApp.register()
        }

        nn_pipeline_set_enabled(pipeline, enabled)
        nn_pipeline_set_aggressive(pipeline, aggressive)
        let listener: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.refreshDevices() }
        devicesListener = listener
        var a = address(kAudioHardwarePropertyDevices)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &a, .main, listener)

        // After coreaudiod restarts, device IDs may be reused while old registrations are dead.
        let restart: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.handleServiceRestart() }
        restartListener = restart
        var r = address(kAudioHardwarePropertyServiceRestarted)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &r, .main, restart)
        refreshDevices()

        // The menu bar icon is easy to miss on a first run, so offer the install once.
        if visible == 0, !CommandLine.arguments.contains("--no-audio"),
           defaults.object(forKey: "offeredDriverInstall") == nil {
            defaults.set(true, forKey: "offeredDriverInstall")
            DispatchQueue.main.async { [weak self] in self?.installDriver() }
        }

        // An app update can't replace the system-folder driver by itself, so say so once per
        // driver version instead of leaving a new app running on an old driver unnoticed.
        if !CommandLine.arguments.contains("--no-audio"),
           let installed = driverVersion(at: URL(fileURLWithPath: installedDriver)),
           let bundled = Bundle.main.url(forResource: "Stillmic", withExtension: "driver").flatMap(driverVersion(at:)),
           installed != bundled,
           defaults.string(forKey: "offeredDriverUpdate") != bundled {
            defaults.set(bundled, forKey: "offeredDriverUpdate")
            DispatchQueue.main.async { [weak self] in
                self?.runDriverInstall(
                    asking: "Update the Stillmic microphone?",
                    detail: "This update includes a newer Stillmic microphone. Until you update it, the app keeps using the older one. You can also do it later from the menu bar.",
                    action: "Update", cancel: "Later")
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        nn_pipeline_stop(pipeline)
    }

    private func handleServiceRestart() {
        stopAudio()
        visible = 0
        visibleListener = nil
        refreshDevices()
    }

    /// Picks up driver installs and mics coming and going.
    private func refreshDevices() {
        let id = device(uid: visibleUID)
        if id != visible {
            var a = address(kAudioDevicePropertyDeviceIsRunningSomewhere)
            if visible != 0, let old = visibleListener {
                AudioObjectRemovePropertyListenerBlock(visible, &a, .main, old)
            }
            visible = id
            visibleListener = nil
            if id != 0 {
                let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.sync() }
                visibleListener = block
                AudioObjectAddPropertyListenerBlock(id, &a, .main, block)
            }
        }
        sync()
    }

    private func preferredMic() -> Mic? {
        let mics = inputDevices()
        if let saved = defaults.string(forKey: "micUID"), let mic = mics.first(where: { $0.uid == saved }) {
            return mic
        }
        return mics.first(where: \.builtIn) ?? mics.first
    }

    /// Audio runs only while an app is reading Stillmic.
    private func sync() {
        let wanted = visible != 0 && isRunningSomewhere(visible) ? preferredMic() : nil
        let engine = wanted == nil ? 0 : device(uid: engineUID)
        if wanted?.id == activeMic?.id && wanted?.uid == activeMic?.uid && engine == activeEngine && !failed {
            updateIcon()
            return
        }
        stopAudio()
        if let wanted { startAudio(wanted, engine: engine) }
        updateIcon()
    }

    private func startAudio(_ mic: Mic, engine: AudioObjectID) {
        failed = engine == 0 || nn_pipeline_start(pipeline, mic.id, engine) != noErr
        guard !failed else {
            scheduleRetry()
            return
        }
        activeMic = mic
        activeEngine = engine
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.restartAudio() }
        micListener = block
        for selector in [kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyDeviceIsAlive] {
            var a = address(selector)
            AudioObjectAddPropertyListenerBlock(mic.id, &a, .main, block)
        }
    }

    private func stopAudio() {
        if let mic = activeMic, let block = micListener {
            for selector in [kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyDeviceIsAlive] {
                var a = address(selector)
                AudioObjectRemovePropertyListenerBlock(mic.id, &a, .main, block)
            }
        }
        micListener = nil
        activeMic = nil
        activeEngine = 0
        failed = false
        nn_pipeline_stop(pipeline)
    }

    /// Another app may hold the mic exclusively, and its release raises no event to wake us.
    private func scheduleRetry() {
        guard !retryScheduled else { return }
        retryScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self else { return }
            self.retryScheduled = false
            if self.failed { self.sync() }
        }
    }

    private func restartAudio() {
        stopAudio()
        sync()
    }

    private func updateIcon() {
        guard let button = statusItem.button else { return }
        let name = enabled ? "waveform" : "waveform.slash"
        button.image = NSImage(systemSymbolName: name, accessibilityDescription: "Stillmic")
        button.appearsDisabled = activeMic == nil
    }

    // MARK: Menu, built only when opened

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        let status = NSMenuItem(title: statusText(), action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)
        menu.addItem(.separator())

        let toggle = NSMenuItem(title: "Noise Removal", action: #selector(toggleEnabled), keyEquivalent: "")
        toggle.target = self
        toggle.state = enabled ? .on : .off
        menu.addItem(toggle)

        let modes = NSMenu()
        for (title, value) in [("Standard", false), ("Aggressive", true)] {
            let item = NSMenuItem(title: title, action: #selector(selectMode(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = value
            item.state = value == aggressive ? .on : .off
            modes.addItem(item)
        }
        let modeItem = NSMenuItem(title: "Mode", action: nil, keyEquivalent: "")
        modeItem.submenu = modes
        menu.addItem(modeItem)

        let mics = NSMenu()
        let current = preferredMic()?.uid
        for mic in inputDevices() {
            let item = NSMenuItem(title: mic.name, action: #selector(selectMic(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = mic.uid
            item.state = mic.uid == current ? .on : .off
            mics.addItem(item)
        }
        let micItem = NSMenuItem(title: "Microphone", action: nil, keyEquivalent: "")
        micItem.submenu = mics
        menu.addItem(micItem)

        let login = NSMenuItem(title: "Start at Login", action: #selector(toggleLogin), keyEquivalent: "")
        login.target = self
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(login)

        menu.addItem(.separator())
        addDriverItems(to: menu)
        if let updater {
            let check = NSMenuItem(title: "Check for Updates…",
                                   action: #selector(SPUStandardUpdaterController.checkForUpdates(_:)), keyEquivalent: "")
            check.target = updater
            menu.addItem(check)
        }

        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Stillmic", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    private func statusText() -> String {
        if visible == 0 { return "Stillmic Mic is not installed" }
        if failed { return "Could not start audio" }
        if let activeMic { return (enabled ? "Cleaning " : "Passing through ") + activeMic.name }
        return "Idle: no app is using Stillmic"
    }

    // MARK: Virtual mic

    private func driverVersion(at url: URL) -> String? {
        let plist = url.appendingPathComponent("Contents/Info.plist")
        return NSDictionary(contentsOf: plist)?["CFBundleVersion"] as? String
    }

    private func addDriverItems(to menu: NSMenu) {
        let installed = driverVersion(at: URL(fileURLWithPath: installedDriver))
        let bundled = Bundle.main.url(forResource: "Stillmic", withExtension: "driver").flatMap(driverVersion(at:))
        if bundled != nil, installed == nil || installed != bundled {
            let title = installed == nil ? "Install Stillmic Mic…" : "Update Stillmic Mic…"
            let item = NSMenuItem(title: title, action: #selector(installDriver), keyEquivalent: "")
            item.target = self
            menu.addItem(item)
        }
        if installed != nil {
            let item = NSMenuItem(title: "Uninstall Stillmic Mic…", action: #selector(uninstallDriver), keyEquivalent: "")
            item.target = self
            menu.addItem(item)
        }
    }

    @objc private func installDriver() {
        runDriverInstall(asking: "Install Stillmic Mic?", detail: nil, action: "Install", cancel: "Cancel")
    }

    private func runDriverInstall(asking title: String, detail: String?, action: String, cancel: String) {
        guard let source = Bundle.main.url(forResource: "Stillmic", withExtension: "driver")?.path else { return }
        guard confirm(title, detail: detail, action: action, cancel: cancel) else { return }
        let hal = (installedDriver as NSString).deletingLastPathComponent
        let dest = shellQuote(installedDriver)
        // Staged and backup names don't end in .driver, so coreaudiod ignores them. The working
        // driver is only moved once the new copy is complete, and comes back if the swap fails.
        let staged = shellQuote(installedDriver + ".new")
        let backup = shellQuote(installedDriver + ".old")
        runPrivileged("/bin/mkdir -p \(shellQuote(hal)) && /bin/rm -rf \(staged) \(backup)"
            + " && /usr/bin/ditto \(shellQuote(source)) \(staged)"
            + " && /usr/sbin/chown -R root:wheel \(staged) && (/usr/bin/xattr -dr com.apple.quarantine \(staged) || true)"
            + " && (if [ -e \(dest) ]; then /bin/mv \(dest) \(backup); fi)"
            + " && { /bin/mv \(staged) \(dest) || { if [ -e \(backup) ]; then /bin/mv \(backup) \(dest); fi; exit 1; }; }"
            + " && /bin/rm -rf \(backup) && (/usr/bin/killall coreaudiod || true)")
    }

    @objc private func uninstallDriver() {
        guard confirm("Uninstall Stillmic Mic?", detail: nil, action: "Uninstall", cancel: "Cancel") else { return }
        runPrivileged("/bin/rm -rf \(shellQuote(installedDriver)) && (/usr/bin/killall coreaudiod || true)")
    }

    private func confirm(_ title: String, detail: String?, action: String, cancel: String) -> Bool {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = (detail.map { $0 + "\n\n" } ?? "")
            + "macOS asks for your password, and all audio on this Mac stops for a moment, "
            + "including any call in progress. Apps that were playing sound may need restarting, "
            + "and browser tabs may need reloading."
        alert.addButton(withTitle: action)
        alert.addButton(withTitle: cancel)
        return alert.runModal() == .alertFirstButtonReturn
    }

    private func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private func runPrivileged(_ command: String) {
        let escaped = command.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        var error: NSDictionary?
        NSAppleScript(source: "do shell script \"\(escaped)\" with administrator privileges")?.executeAndReturnError(&error)
        // -128 means the person cancelled the password prompt.
        if let error, (error[NSAppleScript.errorNumber] as? Int) != -128 {
            let alert = NSAlert()
            alert.messageText = "That didn't work"
            alert.informativeText = error[NSAppleScript.errorMessage] as? String ?? "Unknown error"
            alert.runModal()
        }
    }

    @objc private func toggleEnabled() {
        enabled.toggle()
        nn_pipeline_set_enabled(pipeline, enabled)
        updateIcon()
    }

    @objc private func selectMode(_ sender: NSMenuItem) {
        aggressive = sender.representedObject as? Bool ?? false
        nn_pipeline_set_aggressive(pipeline, aggressive)
    }

    @objc private func selectMic(_ sender: NSMenuItem) {
        defaults.set(sender.representedObject as? String, forKey: "micUID")
        sync()
    }

    @objc private func toggleLogin() {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled { try service.unregister() } else { try service.register() }
        } catch {
            NSLog("Stillmic: login item change failed: %@", error.localizedDescription)
        }
    }
}

// MARK: - Entry point

private func weightsPath() -> String? {
    Bundle.main.path(forResource: "dfn3", ofType: "weights")
        ?? ProcessInfo.processInfo.environment["STILLMIC_WEIGHTS"]
}

guard let path = weightsPath(), let pipeline = nn_pipeline_create(path) else {
    fputs("Stillmic: could not load dfn3.weights\n", stderr)
    exit(1)
}

// `--capture-only <seconds>`: denoise the default-choice mic with no output, then report usage.
if let i = CommandLine.arguments.firstIndex(of: "--capture-only") {
    let seconds = CommandLine.arguments.count > i + 1 ? Double(CommandLine.arguments[i + 1]) ?? 10 : 10
    let mics = inputDevices()
    guard let mic = mics.first(where: \.builtIn) ?? mics.first,
          nn_pipeline_start(pipeline, mic.id, 0) == noErr else {
        fputs("Stillmic: could not start capture\n", stderr)
        exit(1)
    }
    func cpuSeconds() -> Double {
        var r = rusage()
        getrusage(RUSAGE_SELF, &r)
        return Double(r.ru_utime.tv_sec) + Double(r.ru_utime.tv_usec) / 1e6
            + Double(r.ru_stime.tv_sec) + Double(r.ru_stime.tv_usec) / 1e6
    }
    Thread.sleep(forTimeInterval: 2)
    let start = cpuSeconds()
    Thread.sleep(forTimeInterval: seconds)
    let used = cpuSeconds() - start
    var info = rusage_info_current()
    withUnsafeMutablePointer(to: &info) {
        $0.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) {
            _ = proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT, $0)
        }
    }
    print(String(format: "%@: %.2f%% of one core, footprint %.1f MB", mic.name, used / seconds * 100,
                 Double(info.ri_phys_footprint) / 1_048_576))
    nn_pipeline_stop(pipeline)
    exit(0)
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let controller = Controller(pipeline: pipeline)
app.delegate = controller
app.run()
