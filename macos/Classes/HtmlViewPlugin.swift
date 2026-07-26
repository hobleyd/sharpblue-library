import FlutterMacOS
import AppKit

public class HtmlViewPlugin: NSObject, FlutterPlugin {

  private let messenger: FlutterBinaryMessenger
  private var views: [Int64: WebKitView] = [:]
  private var nextId: Int64 = 1
  // The Flutter content view for the engine this plugin is registered on.
  // Captured at register time so createView always attaches to the correct
  // window even when another window has become key in the meantime.
  private weak var flutterView: NSView?

  init(messenger: FlutterBinaryMessenger, flutterView: NSView?) {
    self.messenger = messenger
    self.flutterView = flutterView
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    let messenger = registrar.messenger
    let channel = FlutterMethodChannel(name: "html_view",
                                       binaryMessenger: messenger)
    let instance = HtmlViewPlugin(messenger: messenger, flutterView: registrar.view)
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "createView":
      // Prefer the Flutter content view captured at registration time so the
      // WKWebView is always attached to the engine's own window, regardless of
      // which window is currently key. Fall back to key/main window for
      // environments where the registrar has no associated view (e.g. headless).
      let parentView = flutterView
                    ?? NSApplication.shared.windows.first(where: { $0.isKeyWindow })?.contentView
                    ?? NSApplication.shared.mainWindow?.contentView
      guard let parent = parentView else {
        result(FlutterError(code: "no_window",
                            message: "No window available", details: nil))
        return
      }
      let id = nextId
      nextId += 1
      let view = WebKitView(id: id, parentView: parent, messenger: messenger)
      views[id] = view
      result(id)

    case "destroyView":
      if let id = call.arguments as? Int64 {
        views[id]?.dispose()
        views.removeValue(forKey: id)
      }
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
