// ---------------------------------------------------------------------------
//  NOLF2Launcher — the front door for the macOS port.
//
//  Picks the retail installation, lets the player add extra .rez archives
//  (mods / map packs), then spawns the engine with the right working directory
//  and -rez arguments.
//
//  ⚠️ HARD RULE: this never writes anything into the retail game folder.
//  The engine's own config-saving path is destructive against a real install,
//  so every setting here goes to ~/Library/Application Support/NOLF2Mac/.
//  Treat the game folder as strictly read-only.
//
//  LAYOUT: the window IS the 1024x768 promotional artwork. The art already
//  carries the logo (top-left) and Cate (right third), so this draws no title
//  of its own and keeps every control inside the empty left column —
//  x 48…556, below the logo and clear of the trademark strip along the bottom.
//
//  ⚠️ The artwork is NOT in the repository; see `Art` below.
// ---------------------------------------------------------------------------

import SwiftUI
import AppKit

// MARK: - Palette (sampled from the artwork)

enum Palette {
    static let ink      = Color(red: 0.03, green: 0.05, blue: 0.13)
    static let orange   = Color(red: 0.97, green: 0.60, blue: 0.09)
    static let teal     = Color(red: 0.35, green: 0.72, blue: 0.90)
    static let cream    = Color(red: 0.95, green: 0.93, blue: 0.88)
    static let creamDim = Color(red: 0.66, green: 0.72, blue: 0.82)
    static let danger   = Color(red: 0.95, green: 0.40, blue: 0.35)
}

// MARK: - Persisted settings
//
// ⚠️ Application Support, never the game folder. See the hard rule above.

struct Settings: Codable {
    var gameFolder: String = ""
    var extraRez: [String] = []
    var enginePath: String = ""
    var muteFaultyWaterfall: Bool = true      // known issue; see README
    var recentFolders: [String] = []

    // Display. ★ These exist because a double-clicked .app CANNOT be given
    // environment variables — every one of the engine's display and
    // troubleshooting switches was unreachable for anyone not launching it from
    // a terminal. The launcher is the only place a tester can get at them.
    var fullscreen: Bool = true
    var windowSize: String = "1280x800"       // used only when windowed
    var disable3DSound: Bool = false          // LT_NO_SOUND_3D
    var disableMusic: Bool = false            // LT_NO_MUSIC

    static var fileURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask)[0]
            .appendingPathComponent("NOLF2Mac", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("launcher.json")
    }

    static func load() -> Settings {
        guard let d = try? Data(contentsOf: fileURL),
              let s = try? JSONDecoder().decode(Settings.self, from: d) else { return Settings() }
        return s
    }

    func save() {
        guard let d = try? JSONEncoder().encode(self) else { return }
        try? d.write(to: Settings.fileURL, options: .atomic)
    }
}

// MARK: - Locating things

enum GameFolder {
    /// The archives the retail game ships, in load order. Later entries win, so
    /// the user's extras are appended after these.
    static let baseArchives = ["GAME.rez", "GAME2.rez", "Update_v1x3.rez"]

    /// Case-insensitive lookup — the retail tree is upper-case on disk while the
    /// engine and every doc refer to it in mixed case.
    static func file(_ name: String, in folder: URL) -> URL? {
        guard let items = try? FileManager.default.contentsOfDirectory(atPath: folder.path)
        else { return nil }
        if let hit = items.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) {
            return folder.appendingPathComponent(hit)
        }
        return nil
    }

    /// A folder is usable when the two core archives are present. This check is
    /// what turns a confusing engine-side failure into a clear message here.
    static func validate(_ path: String) -> String? {
        if path.isEmpty { return "No folder chosen yet." }
        let url = URL(fileURLWithPath: path)
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else {
            return "That folder no longer exists."
        }
        for a in ["GAME.rez", "GAME2.rez"] where file(a, in: url) == nil {
            return "Doesn't look like a NOLF2 installation — \(a) is missing."
        }
        return nil
    }

    /// Everything to pass as -rez, in order.
    static func archives(in folder: URL, extras: [String]) -> [String] {
        var out: [String] = []
        for a in baseArchives {
            if let u = file(a, in: folder) { out.append(u.lastPathComponent) }
        }
        // The loose `Game` directory overrides the archives, so it comes after
        // them — and before the user's extras, which override everything.
        if file("Game", in: folder) != nil { out.append("Game") }
        out.append(contentsOf: extras)
        return out
    }
}

/// Windowed sizes offered, filtered to what the display can actually show.
/// Mirrors the ladder the renderer reports for the in-game Display Options
/// (nullrender.cpp), so the two agree.
enum Resolutions {
    static let ladder = ["800x600", "1024x768", "1280x720", "1280x800",
                         "1440x900", "1600x900", "1680x1050", "1920x1080", "2560x1440"]
    static var available: [String] {
        let f = NSScreen.main?.frame.size ?? CGSize(width: 1920, height: 1080)
        let out = ladder.filter { r in
            let p = r.split(separator: "x").compactMap { Int($0) }
            return p.count == 2 && CGFloat(p[0]) <= f.width && CGFloat(p[1]) <= f.height
        }
        return out.isEmpty ? ["800x600"] : out
    }
}

enum EngineLocator {
    /// In a shipped .app the engine sits beside the launcher. In development it
    /// is wherever CMake put it, so a saved override wins.
    static func find(saved: String) -> String? {
        if !saved.isEmpty, FileManager.default.isExecutableFile(atPath: saved) { return saved }
        let sibling = Bundle.main.bundleURL
            .appendingPathComponent("Contents/MacOS/Lithtech").path
        if FileManager.default.isExecutableFile(atPath: sibling) { return sibling }
        return nil
    }
}

// MARK: - The three dlopen'd game modules
//
// ★★ THE ENGINE IS ONLY HALF THE GAME. All the game logic lives in three
// modules the engine dlopen()s at runtime:
//
//     libCShell.dylib   the client shell — menus, HUD, the whole front end
//     libObject.lto     the server-side game logic
//     libClientFx.dylib the effects library
//
// ⚠️ Each defaults to a path relative to the CURRENT DIRECTORY ("./libCShell.dylib"),
// which only resolves on a development machine where someone has symlinked them
// into the game folder. A tester has no such symlinks — so the launcher finds
// them itself and passes ABSOLUTE paths through the engine's own overrides.
// Without this the engine starts, finds no client shell, falls back to the
// built-in null shell, and renders a BLACK SCREEN with no error.

enum GameModules {
    static let names = ["libCShell.dylib", "libObject.lto", "libClientFx.dylib"]
    static let envVars = ["LT_CSHELL_MODULE", "LT_OBJECT_MODULE", "LT_CLIENTFX_MODULE"]

    /// Search, in order: beside the engine (shipping bundle), the bundle's
    /// Frameworks directory, and the CMake build layout, where each module
    /// lands in its own subdirectory next to `client/Lithtech`.
    static func locate(enginePath: String) -> [String: String] {
        let fm = FileManager.default
        let engineDir = URL(fileURLWithPath: enginePath).deletingLastPathComponent()
        var roots = [engineDir,
                     Bundle.main.bundleURL.appendingPathComponent("Contents/Frameworks"),
                     engineDir.deletingLastPathComponent()]
        // …plus one level of subdirectories of the build root (ObjectDLL/,
        // ClientShellDLL/, ClientFxDLL/).
        if let subs = try? fm.contentsOfDirectory(at: engineDir.deletingLastPathComponent(),
                                                 includingPropertiesForKeys: nil) {
            roots.append(contentsOf: subs.filter {
                (try? $0.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
            })
        }

        var found: [String: String] = [:]
        for (name, env) in zip(names, envVars) {
            for r in roots {
                let c = r.appendingPathComponent(name)
                if fm.fileExists(atPath: c.path) {
                    found[env] = c.path
                    break
                }
            }
        }
        return found
    }

    /// Which modules could not be found — reported before launching, rather
    /// than letting the player stare at a black screen.
    static func missing(enginePath: String) -> [String] {
        let found = locate(enginePath: enginePath)
        return zip(names, envVars).filter { found[$0.1] == nil }.map { $0.0 }
    }
}

enum EngineLog {
    /// A standard macOS location, so a bug report can just say "attach this".
    static var url: URL {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/NOLF2Mac", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("engine.log")
    }
}

// MARK: - Background artwork
//
// ⚠️ THE ARTWORK IS DELIBERATELY NOT IN THE REPOSITORY. It is copyrighted
// promotional material — the image carries its own trademark notice — and this
// project never redistributes assets it has no right to. The build script
// copies it in from launcher/Resources/ (git-ignored) when present. When it is
// absent the launcher still builds, runs and looks deliberate, so a clean clone
// works for anyone.

enum Art {
    static let image: NSImage? = {
        guard let u = Bundle.main.url(forResource: "NOLF2_Background", withExtension: "jpg")
        else { return nil }
        return NSImage(contentsOf: u)
    }()
}

struct FallbackBackground: View {
    var body: some View {
        ZStack(alignment: .topLeading) {
            LinearGradient(colors: [Palette.ink, Color(red: 0.05, green: 0.11, blue: 0.30)],
                           startPoint: .bottomLeading, endPoint: .topTrailing)
            VStack(alignment: .leading, spacing: 4) {
                Text("NO ONE LIVES FOREVER 2")
                    .font(.system(size: 36, weight: .black, design: .rounded))
                    .foregroundColor(Palette.orange)
                Text("A SPY IN H.A.R.M.'S WAY")
                    .font(.system(size: 13, weight: .semibold, design: .rounded))
                    .tracking(3)
                    .foregroundColor(Palette.cream)
            }
            .padding(.leading, 60).padding(.top, 60)
        }
    }
}

// MARK: - Styled pieces

struct SectionLabel: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.system(size: 10, weight: .heavy, design: .rounded))
            .tracking(2.2)
            .foregroundColor(Palette.orange)
    }
}

struct MenuButtonStyle: ButtonStyle {
    var tint: Color = Palette.orange
    var filled: Bool = false
    var big: Bool = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: big ? 15 : 11, weight: .bold, design: .rounded))
            .foregroundColor(filled ? Palette.ink : tint)
            .padding(.horizontal, big ? 26 : 12)
            .padding(.vertical, big ? 9 : 5)
            .background(
                RoundedRectangle(cornerRadius: 5)
                    .fill(filled ? tint : Color.black.opacity(0.35))
                    .overlay(RoundedRectangle(cornerRadius: 5)
                        .strokeBorder(tint.opacity(filled ? 1 : 0.7), lineWidth: 1.2))
            )
            .opacity(configuration.isPressed ? 0.6 : 1)
    }
}

// MARK: - Main view

struct LauncherView: View {
    @State private var settings = Settings.load()
    @State private var launchError: String?
    @State private var selectedExtra: String?

    private var folderProblem: String? { GameFolder.validate(settings.gameFolder) }
    private var canLaunch: Bool {
        folderProblem == nil && EngineLocator.find(saved: settings.enginePath) != nil
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            if let img = Art.image {
                Image(nsImage: img).resizable().frame(width: 1024, height: 768)
            } else {
                FallbackBackground().frame(width: 1024, height: 768)
            }
            controlPanel
                .frame(width: 500, height: 520)
                .padding(.leading, 48)
                .padding(.top, 190)
        }
        .frame(width: 1024, height: 768)
        .preferredColorScheme(.dark)
    }

    /// The control column, sitting in the empty left third of the artwork.
    private var controlPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("macOS source port  ·  beta")
                .font(.system(size: 11, weight: .semibold, design: .rounded))
                .tracking(1.6)
                .foregroundColor(Palette.teal)

            gameFolderSection
            extraRezSection
            optionsSection
            Spacer(minLength: 0)
            footer
        }
        .padding(18)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(Color.black.opacity(0.45))
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(Palette.teal.opacity(0.30), lineWidth: 1))
        )
    }

    private var gameFolderSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionLabel(text: "Retail game folder")
            HStack(spacing: 8) {
                Text(settings.gameFolder.isEmpty ? "Choose your NOLF2 installation…"
                                                 : settings.gameFolder)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(settings.gameFolder.isEmpty ? Palette.creamDim : Palette.cream)
                    .lineLimit(1).truncationMode(.head)
                    .padding(.horizontal, 9).padding(.vertical, 7)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 5).fill(Color.black.opacity(0.5)))
                    .overlay(RoundedRectangle(cornerRadius: 5)
                        .strokeBorder(folderProblem == nil ? Palette.teal.opacity(0.55)
                                                           : Palette.danger.opacity(0.8),
                                      lineWidth: 1))
                Button("CHOOSE…") { chooseGameFolder() }
                    .buttonStyle(MenuButtonStyle(tint: Palette.teal))
            }
            HStack(spacing: 5) {
                Image(systemName: folderProblem == nil ? "checkmark.seal.fill"
                                                       : "exclamationmark.triangle.fill")
                    .font(.system(size: 9))
                Text(folderProblem ?? "Installation looks good.")
                    .font(.system(size: 10))
            }
            .foregroundColor(folderProblem == nil ? Palette.teal : Palette.danger)

            if !settings.recentFolders.isEmpty {
                HStack(spacing: 8) {
                    Text("Recent:").font(.system(size: 9, weight: .semibold))
                        .foregroundColor(Palette.creamDim)
                    ForEach(settings.recentFolders.prefix(3), id: \.self) { r in
                        Button((r as NSString).lastPathComponent) {
                            settings.gameFolder = r; settings.save()
                        }
                        .buttonStyle(.plain)
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(Palette.orange)
                    }
                }
            }
        }
    }

    private var extraRezSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                SectionLabel(text: "Additional .rez archives")
                Spacer()
                Text("later entries override earlier")
                    .font(.system(size: 9)).foregroundColor(Palette.creamDim)
            }
            ZStack {
                RoundedRectangle(cornerRadius: 5).fill(Color.black.opacity(0.5))
                if settings.extraRez.isEmpty {
                    Text("No extra archives. Mods and map packs go here.")
                        .font(.system(size: 10)).foregroundColor(Palette.creamDim)
                } else {
                    List(selection: $selectedExtra) {
                        ForEach(settings.extraRez, id: \.self) { p in
                            Text((p as NSString).lastPathComponent)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(Palette.cream)
                                .tag(p)
                        }
                    }
                    .listStyle(.plain)
                    .padding(3)
                }
            }
            .frame(height: 82)
            .overlay(RoundedRectangle(cornerRadius: 5)
                .strokeBorder(Palette.creamDim.opacity(0.25), lineWidth: 1))

            HStack(spacing: 6) {
                Button("ADD…") { addRez() }.buttonStyle(MenuButtonStyle(tint: Palette.teal))
                Button("REMOVE") { removeSelected() }
                    .buttonStyle(MenuButtonStyle(tint: Palette.creamDim))
                    .disabled(selectedExtra == nil)
                Spacer()
                Button("MOVE UP") { move(-1) }
                    .buttonStyle(MenuButtonStyle(tint: Palette.creamDim))
                    .disabled(selectedExtra == nil)
                Button("MOVE DOWN") { move(1) }
                    .buttonStyle(MenuButtonStyle(tint: Palette.creamDim))
                    .disabled(selectedExtra == nil)
            }
        }
    }

    private var optionsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel(text: "Display")
            HStack(spacing: 10) {
                Picker("", selection: $settings.fullscreen) {
                    Text("Fullscreen").tag(true)
                    Text("Windowed").tag(false)
                }
                .pickerStyle(.segmented)
                .frame(width: 190)
                .labelsHidden()

                Picker("", selection: $settings.windowSize) {
                    ForEach(Resolutions.available, id: \.self) { Text($0).tag($0) }
                }
                .frame(width: 130)
                .labelsHidden()
                .disabled(settings.fullscreen)
                .opacity(settings.fullscreen ? 0.4 : 1)

                Text(settings.fullscreen ? "uses the whole display" : "window size")
                    .font(.system(size: 9)).foregroundColor(Palette.creamDim)
            }

            DisclosureGroup {
                VStack(alignment: .leading, spacing: 3) {
                    Toggle(isOn: $settings.muteFaultyWaterfall) {
                        Text("Mute the faulty distant-waterfall ambience")
                            .font(.system(size: 10)).foregroundColor(Palette.cream)
                    }
                    Toggle(isOn: $settings.disable3DSound) {
                        Text("Disable 3D positional sound")
                            .font(.system(size: 10)).foregroundColor(Palette.cream)
                    }
                    Toggle(isOn: $settings.disableMusic) {
                        Text("Disable music")
                            .font(.system(size: 10)).foregroundColor(Palette.cream)
                    }
                }
                .toggleStyle(.checkbox)
                .padding(.top, 3)
            } label: {
                Text("Troubleshooting")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundColor(Palette.teal)
            }
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let e = launchError {
                Text(e).font(.system(size: 10)).foregroundColor(Palette.danger)
            }
            if EngineLocator.find(saved: settings.enginePath) == nil {
                HStack(spacing: 8) {
                    Text("Engine binary not found.")
                        .font(.system(size: 10)).foregroundColor(Palette.danger)
                    Button("LOCATE…") { chooseEngine() }
                        .buttonStyle(MenuButtonStyle(tint: Palette.danger))
                }
            }
            HStack {
                Button("QUIT") { NSApp.terminate(nil) }
                    .buttonStyle(MenuButtonStyle(tint: Palette.creamDim))
                Spacer()
                Button("LAUNCH") { launch() }
                    .buttonStyle(MenuButtonStyle(tint: Palette.orange, filled: true, big: true))
                    .disabled(!canLaunch)
                    .opacity(canLaunch ? 1 : 0.4)
            }
        }
    }

    // MARK: actions

    private func chooseGameFolder() {
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = false
        p.allowsMultipleSelection = false
        p.prompt = "Choose"
        p.message = "Select your No One Lives Forever 2 installation folder"
        guard p.runModal() == .OK, let url = p.url else { return }
        settings.gameFolder = url.path
        settings.recentFolders.removeAll { $0 == url.path }
        settings.recentFolders.insert(url.path, at: 0)
        settings.recentFolders = Array(settings.recentFolders.prefix(5))
        settings.save()
    }

    private func chooseEngine() {
        let p = NSOpenPanel()
        p.canChooseFiles = true
        p.canChooseDirectories = false
        p.prompt = "Use"
        p.message = "Select the Lithtech engine executable"
        guard p.runModal() == .OK, let url = p.url else { return }
        settings.enginePath = url.path
        settings.save()
    }

    private func addRez() {
        let p = NSOpenPanel()
        p.canChooseFiles = true
        p.allowsMultipleSelection = true
        p.allowedFileTypes = ["rez"]
        p.message = "Select additional .rez archives"
        guard p.runModal() == .OK else { return }
        for u in p.urls where !settings.extraRez.contains(u.path) {
            settings.extraRez.append(u.path)
        }
        settings.save()
    }

    private func removeSelected() {
        guard let s = selectedExtra else { return }
        settings.extraRez.removeAll { $0 == s }
        selectedExtra = nil
        settings.save()
    }

    private func move(_ delta: Int) {
        guard let s = selectedExtra, let i = settings.extraRez.firstIndex(of: s) else { return }
        let j = i + delta
        guard j >= 0, j < settings.extraRez.count else { return }
        settings.extraRez.swapAt(i, j)
        settings.save()
    }

    private func launch() {
        launchError = nil
        guard let enginePath = EngineLocator.find(saved: settings.enginePath) else {
            launchError = "Could not find the engine executable."; return
        }
        let folder = URL(fileURLWithPath: settings.gameFolder)

        var args: [String] = []
        for a in GameFolder.archives(in: folder, extras: settings.extraRez) {
            args.append("-rez"); args.append(a)
        }

        // Fail loudly HERE rather than launching into a black screen.
        let absent = GameModules.missing(enginePath: enginePath)
        if !absent.isEmpty {
            launchError = "Missing game module(s): \(absent.joined(separator: ", ")). "
                        + "The engine cannot run without them."
            return
        }

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: enginePath)
        // ⚠️ The engine resolves its archives relative to the CURRENT DIRECTORY,
        // so this must be the game folder — not the app bundle.
        proc.currentDirectoryURL = folder
        proc.arguments = args

        var env = ProcessInfo.processInfo.environment
        // ★★ Without this the engine loads its built-in NULL client shell and
        // renders a black screen — no menu, no game, no error message.
        env["LT_LOAD_CSHELL"] = "1"
        // Absolute paths to the three dlopen'd modules, so this works on a
        // machine that has no dev symlinks in the game folder.
        for (k, v) in GameModules.locate(enginePath: enginePath) { env[k] = v }
        if settings.muteFaultyWaterfall { env["LT_MUTE_SOUNDS"] = "waterfall_lg_dist" }
        // ⚠️ Set these only when the user asked for them. The engine's
        // disable-switches now respect their VALUE, but an unset variable is
        // still the cleanest way to say "default".
        if settings.disable3DSound { env["LT_NO_SOUND_3D"] = "1" }
        if settings.disableMusic   { env["LT_NO_MUSIC"]    = "1" }
        if !settings.fullscreen {
            env["LT_WINDOWED"]    = "1"
            env["LT_WINDOW_SIZE"] = settings.windowSize
        }
        proc.environment = env

        // Capture the engine's output. A black screen is otherwise silent, and
        // this is the file a bug report should carry.
        FileManager.default.createFile(atPath: EngineLog.url.path, contents: nil)
        if let h = try? FileHandle(forWritingTo: EngineLog.url) {
            proc.standardOutput = h
            proc.standardError = h
        }

        do {
            try proc.run()
            settings.save()
            // Give it a moment: if the engine dies immediately, the player
            // should see why instead of the launcher vanishing.
            DispatchQueue.global().asyncAfter(deadline: .now() + 2.0) {
                let died = !proc.isRunning
                let status = proc.isRunning ? 0 : proc.terminationStatus
                DispatchQueue.main.async {
                    if died && status != 0 {
                        launchError = "The engine exited immediately (status \(status)). "
                                    + "See \(EngineLog.url.path)"
                    } else {
                        NSApp.terminate(nil)
                    }
                }
            }
        } catch {
            launchError = "Could not start the engine: \(error.localizedDescription)"
        }
    }
}

// MARK: - App

@main
struct NOLF2LauncherApp: App {
    var body: some Scene {
        // ⚠️ WindowGroup, not Window: `Window` and `.windowResizability` are
        // macOS 13+, and this targets 12.0 so the beta reaches more machines.
        WindowGroup("No One Lives Forever 2") {
            LauncherView()
        }
    }
}
