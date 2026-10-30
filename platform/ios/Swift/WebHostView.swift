import SwiftUI
import WebKit

/// Hosts the existing Groove web UI (mobile-app/, bundled as "web") inside SwiftUI, unchanged.
/// The page talks to the native engine with `window.webkit.messageHandlers.groove.postMessage({...})`
/// and receives events through `window.grooveRemote(...)` — the same contract the Android shell uses.
struct WebHostView: UIViewRepresentable {
    let engine: GrooveEngine

    func makeCoordinator() -> Coordinator { Coordinator(engine: engine) }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.mediaTypesRequiringUserActionForPlayback = []
        // WKUserContentController retains its handlers strongly; the proxy below holds the coordinator weakly,
        // which breaks the WKWebView → controller → handler → WKWebView retain cycle.
        config.userContentController.add(WeakScriptMessageHandler(context.coordinator), name: "groove")
        let web = WKWebView(frame: .zero, configuration: config)
        web.isOpaque = false
        web.backgroundColor = UIColor(red: 0x3f / 255.0, green: 0x8c / 255.0, blue: 0x5f / 255.0, alpha: 1)  // splash green
        web.scrollView.contentInsetAdjustmentBehavior = .never
        context.coordinator.webView = web
        if let index = Bundle.main.url(forResource: "index", withExtension: "html", subdirectory: "web") {
            web.loadFileURL(index, allowingReadAccessTo: index.deletingLastPathComponent())
        }
        return web
    }

    func updateUIView(_ uiView: WKWebView, context: Context) {}

    static func dismantleUIView(_ uiView: WKWebView, coordinator: Coordinator) {
        uiView.configuration.userContentController.removeScriptMessageHandler(forName: "groove")
    }

    @MainActor
    final class Coordinator: NSObject, WKScriptMessageHandler {
        weak var webView: WKWebView?
        private let engine: GrooveEngine

        init(engine: GrooveEngine) {
            self.engine = engine
            super.init()
            engine.onTrackStarted = { [weak self] id in self?.send("track:\(id)") }
            engine.onQueueEnded = { [weak self] in self?.send("ended") }
        }

        func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
            guard let body = message.body as? [String: Any], let cmd = body["cmd"] as? String else { return }
            switch cmd {
            case "load":
                if let id = body["id"] as? Int, let path = body["path"] as? String {
                    engine.load(UInt64(id), url: URL(fileURLWithPath: path), autoplay: body["autoplay"] as? Bool ?? true)
                }
            case "next":
                if let id = body["id"] as? Int { engine.setNext(UInt64(id), url: (body["path"] as? String).map { URL(fileURLWithPath: $0) }) }
            case "play": engine.play()
            case "pause": engine.pause()
            case "seek": if let s = body["seconds"] as? Double { engine.seek(to: s) }
            default: break  // unknown commands are ignored, never trusted
            }
        }

        private func send(_ event: String) {
            let escaped = event.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "'", with: "\\'")
            webView?.evaluateJavaScript("window.grooveRemote && window.grooveRemote('\(escaped)')")
        }
    }
}

/// Forwards script messages to a weakly held handler.
final class WeakScriptMessageHandler: NSObject, WKScriptMessageHandler {
    private weak var target: WKScriptMessageHandler?
    init(_ target: WKScriptMessageHandler) { self.target = target }
    func userContentController(_ c: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.userContentController(c, didReceive: message)
    }
}
