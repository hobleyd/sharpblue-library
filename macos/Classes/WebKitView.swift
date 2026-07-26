import WebKit
import FlutterMacOS

// JS bridge injected before any page scripts.
// editor.html's _flutterNotify() falls back to window[name].postMessage(),
// which our injected objects forward via the webkit message handler.
private let kJsBridge = """
(function() {
  function makeChannel(name) {
    return { postMessage: function(v) {
      window.webkit.messageHandlers.HtmlView.postMessage(
        name + '\\x00' + (v !== undefined ? String(v) : '')
      );
    }};
  }
  window['onContentChanged']  = makeChannel('onContentChanged');
  window['onLinkRequest']     = makeChannel('onLinkRequest');
  window['onAttachRequest']   = makeChannel('onAttachRequest');
  window['onImageDoubleClicked'] = makeChannel('onImageDoubleClicked');
  window['onImagePasted']     = makeChannel('onImagePasted');
  document.addEventListener('dblclick', function(e) {
    var t = e.target;
    if (t && t.tagName === 'IMG') {
      var src = t.currentSrc || t.src;
      if (src) window['onImageDoubleClicked'].postMessage(src);
    }
  }, true);
  document.addEventListener('DOMContentLoaded', function() {
    window.webkit.messageHandlers.HtmlView.postMessage('pageLoaded\\x00');
  });
})();
"""

// WKWebView is added here as a plain sibling NSView (not a Flutter platform
// view), so nothing in Flutter's focus system ever calls makeFirstResponder
// on it, and WKWebView's own mouseDown handling doesn't reliably claim first
// responder either — clicking it left whatever Flutter text field (e.g. the
// To: field) was previously focused still as firstResponder (confirmed
// empirically: unchanged before/after super.mouseDown).
//
// Forcing makeFirstResponder directly from mouseDown raced with Flutter:
// unfocusing the Flutter field (needed so its cursor stops showing) also
// makes Flutter's text input plugin reassert itself as firstResponder,
// which can clobber our forced focus if that round trip lands after it.
// So mouseDown only *signals* the click via onClickFocus; the Dart side
// unfocuses its own field first, then calls back to focus() (same native
// makeFirstResponder + DOM focus path Tab already uses) — guaranteeing our
// focus grab is always the last word.
private class FocusableWebView: WKWebView {
  var onClickFocus: (() -> Void)?

  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

  override func mouseDown(with event: NSEvent) {
    onClickFocus?()
    super.mouseDown(with: event)
  }
}

class WebKitView: NSObject, WKScriptMessageHandler, FlutterStreamHandler, WKNavigationDelegate {

  private let id: Int64
  private let webView: FocusableWebView
  private weak var parentView: NSView?
  private let channel: FlutterMethodChannel
  private let eventChannel: FlutterEventChannel
  private var eventSink: FlutterEventSink?
  private var printKeyMonitor: Any?

  // Logical-pixel position/size from Dart (AppKit uses points = logical pixels).
  private var posX: CGFloat = 0
  private var posY: CGFloat = 0
  private var width: CGFloat = 0
  private var height: CGFloat = 0

  init(id: Int64, parentView: NSView, messenger: FlutterBinaryMessenger) {
    self.id = id
    self.parentView = parentView

    let config = WKWebViewConfiguration()
    let contentController = WKUserContentController()

    let script = WKUserScript(source: kJsBridge,
                               injectionTime: .atDocumentStart,
                               forMainFrameOnly: true)
    contentController.addUserScript(script)
    config.userContentController = contentController

    webView = FocusableWebView(frame: .zero, configuration: config)
    webView.isHidden = false

    channel = FlutterMethodChannel(name: "html_view/\(id)",
                                   binaryMessenger: messenger)
    eventChannel = FlutterEventChannel(name: "html_view/\(id)_events",
                                       binaryMessenger: messenger)

    super.init()

    webView.navigationDelegate = self
    contentController.add(self, name: "HtmlView")
    parentView.addSubview(webView)

    webView.onClickFocus = { [weak self] in
      self?.emitEvent(type: "onClickFocus", value: "")
    }

    channel.setMethodCallHandler { [weak self] call, result in
      self?.handleMethod(call, result: result)
    }
    eventChannel.setStreamHandler(self)

    // Intercept Cmd-P when this webview is visible in the key window,
    // even when the WKWebView itself has native keyboard focus.
    printKeyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
      guard let self = self,
            !self.webView.isHidden,
            let win = self.webView.window,
            win == NSApplication.shared.keyWindow,
            event.modifierFlags.contains(.command),
            event.charactersIgnoringModifiers == "p" else { return event }
      if #available(macOS 11.0, *) {
        let op = self.webView.printOperation(with: NSPrintInfo.shared)
        op.showsPrintPanel = true
        op.showsProgressPanel = true
        op.runModal(for: win, delegate: nil, didRun: nil, contextInfo: nil)
      }
      return nil // consume — prevents double-firing with Flutter shortcuts
    }
  }

  // MARK: - WKNavigationDelegate

  func webView(_ webView: WKWebView,
               decidePolicyFor navigationAction: WKNavigationAction,
               decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
    if let url = navigationAction.request.url {
      let scheme = url.scheme?.lowercased() ?? ""
      if scheme == "http" || scheme == "https" || scheme == "mailto" {
        emitEvent(type: "onLinkOpened", value: url.absoluteString)
        decisionHandler(.cancel)
        return
      }
    }
    decisionHandler(.allow)
  }

  // MARK: - WKScriptMessageHandler

  func userContentController(_ userContentController: WKUserContentController,
                             didReceive message: WKScriptMessage) {
    guard let body = message.body as? String else { return }
    guard let sep = body.firstIndex(of: "\0") else { return }
    let type  = String(body[body.startIndex..<sep])
    let value = String(body[body.index(after: sep)...])
    emitEvent(type: type, value: value)
  }

  // MARK: - FlutterStreamHandler

  func onListen(withArguments arguments: Any?,
                eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    eventSink = events
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    return nil
  }

  // MARK: - Method handling

  private func handleMethod(_ call: FlutterMethodCall,
                             result: @escaping FlutterResult) {
    switch call.method {
    case "loadHtml":
      guard let html = call.arguments as? String else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      webView.loadHTMLString(html, baseURL: nil)
      result(nil)

    case "loadUrl":
      guard let urlStr = call.arguments as? String,
            let url = URL(string: urlStr) else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      if url.isFileURL {
        webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
      } else {
        webView.load(URLRequest(url: url))
      }
      result(nil)

    case "loadAsset":
      guard let key = call.arguments as? String else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      loadAsset(key: key, result: result)

    case "eval":
      guard let js = call.arguments as? String else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      webView.evaluateJavaScript(js) { value, error in
        if let error = error {
          result(FlutterError(code: "eval_failed", message: error.localizedDescription,
                              details: nil))
        } else {
          result(value.map { "\($0)" } ?? "null")
        }
      }

    case "setPosition":
      guard let list = call.arguments as? [Double], list.count == 3 else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      // macOS AppKit uses logical points (no DPR multiply needed).
      posX = CGFloat(list[0])
      posY = CGFloat(list[1])
      updateFrame()
      result(nil)

    case "setSize":
      guard let list = call.arguments as? [Double], list.count == 3 else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      width  = CGFloat(list[0])
      height = CGFloat(list[1])
      updateFrame()
      result(nil)

    case "setVisible":
      guard let visible = call.arguments as? Bool else {
        result(FlutterError(code: "bad_args", message: nil, details: nil)); return
      }
      webView.isHidden = !visible
      result(nil)

    case "printCurrent":
      if #available(macOS 11.0, *), let win = webView.window {
        let op = webView.printOperation(with: NSPrintInfo.shared)
        op.showsPrintPanel = true
        op.showsProgressPanel = true
        op.runModal(for: win, delegate: nil, didRun: nil, contextInfo: nil)
      }
      result(nil)

    case "focus":
      webView.window?.makeFirstResponder(webView)
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func loadAsset(key: String, result: @escaping FlutterResult) {
    // Flutter assets are bundled inside App.framework/Resources/flutter_assets/,
    // not in the main bundle's Resources. Search all loaded frameworks first,
    // then fall back to the main bundle for other build configurations.
    let searchBundles: [Bundle] = Bundle.allFrameworks.filter {
      $0.bundlePath.hasSuffix("App.framework")
    } + [Bundle.main]

    for bundle in searchBundles {
      guard let resPath = bundle.resourcePath else { continue }
      let fullPath = "\(resPath)/flutter_assets/\(key)"
      if FileManager.default.fileExists(atPath: fullPath) {
        let fileURL = URL(fileURLWithPath: fullPath)
        let accessURL = URL(fileURLWithPath: "\(resPath)/flutter_assets")
        webView.loadFileURL(fileURL, allowingReadAccessTo: accessURL)
        result(nil)
        return
      }
    }
    result(FlutterError(code: "not_found", message: "Asset not found: \(key)", details: nil))
  }

  // MARK: - Layout

  private func updateFrame() {
    guard let parent = parentView else { return }
    let h = parent.bounds.height
    // AppKit default: origin at bottom-left; convert Flutter top-left Y.
    let y: CGFloat = parent.isFlipped ? posY : (h - posY - height)
    webView.frame = NSRect(x: posX, y: y, width: max(1, width), height: max(1, height))
  }

  // MARK: - Events

  private func emitEvent(type: String, value: String) {
    eventSink?(["type": type, "value": value])
  }

  func dispose() {
    if let monitor = printKeyMonitor {
      NSEvent.removeMonitor(monitor)
      printKeyMonitor = nil
    }
    channel.setMethodCallHandler(nil)
    eventChannel.setStreamHandler(nil)
    webView.configuration.userContentController.removeScriptMessageHandler(forName: "HtmlView")
    webView.removeFromSuperview()
  }
}
