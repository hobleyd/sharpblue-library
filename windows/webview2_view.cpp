#include "webview2_view.h"

#include <commctrl.h>
#include <shlobj.h>
#include <algorithm>
#include <flutter/standard_method_codec.h>

namespace html_view {

namespace {

// Injected on every document load (before any page scripts).
// editor.html's _flutterNotify() checks window.flutter_inappwebview first;
// since that is undefined here it falls through to window[name].postMessage().
// Use \x01 (SOH) as the name/value separator, not \x00.
// TryGetWebMessageAsString returns an LPWSTR; std::wstring(LPWSTR) stops at
// the first null terminator, so any \x00 embedded in the message truncates it.
static const wchar_t* kJsBridge = LR"JS(
(function() {
  function makeChannel(name) {
    return {
      postMessage: function(v) {
        window.chrome.webview.postMessage(
          name + '\x01' + (v !== undefined ? String(v) : '')
        );
      }
    };
  }
  // Channels editor.html calls via window[name].postMessage(value)
  window['onContentChanged']  = makeChannel('onContentChanged');
  window['onLinkRequest']     = makeChannel('onLinkRequest');
  window['onAttachRequest']   = makeChannel('onAttachRequest');
  window['onImageDoubleClicked'] = makeChannel('onImageDoubleClicked');
  window['onImagePasted']     = makeChannel('onImagePasted');

  // Report a double-click on an image so the host can pop it out in a
  // resizable window. Capture phase so it fires regardless of page handlers.
  document.addEventListener('dblclick', function(e) {
    var t = e.target;
    if (t && t.tagName === 'IMG') {
      var src = t.currentSrc || t.src;
      if (src) window['onImageDoubleClicked'].postMessage(src);
    }
  }, true);

  // Tell Dart when the page DOM is ready so setContent() can be called.
  document.addEventListener('DOMContentLoaded', function() {
    window.chrome.webview.postMessage('pageLoaded\x01');
  });
})();
)JS";

}  // namespace

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string WebView2View::WideToUtf8(const wchar_t* wide) {
  if (!wide || !wide[0]) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring WebView2View::Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring out(n - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
  return out;
}

std::wstring WebView2View::GetExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring exe(path);
  auto pos = exe.rfind(L'\\');
  return pos != std::wstring::npos ? exe.substr(0, pos) : exe;
}

std::wstring WebView2View::AssetPath(const std::string& key) {
  return GetExecutableDir() + L"\\data\\flutter_assets\\" + Utf8ToWide(key);
}

std::wstring WebView2View::PathToFileUri(const std::wstring& path) {
  std::wstring uri = L"file:///" + path;
  std::replace(uri.begin(), uri.end(), L'\\', L'/');
  return uri;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WebView2View::WebView2View(int64_t id, HWND parent_hwnd,
                           flutter::BinaryMessenger* messenger)
    : id_(id), parent_hwnd_(parent_hwnd) {

  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      messenger, "html_view/" + std::to_string(id_),
      &flutter::StandardMethodCodec::GetInstance());
  // Capture alive_ so the handler bails safely if the view is destroyed before
  // the messenger unregisters it (avoids needing SetMethodCallHandler(nullptr)
  // in the destructor, which can race with secondary-window engine teardown).
  auto alive_ch = alive_;
  channel_->SetMethodCallHandler(
      [this, alive_ch](const auto& call, auto result) {
        if (!*alive_ch) { result->NotImplemented(); return; }
        HandleMethod(call, std::move(result));
      });

  event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          messenger, "html_view/" + std::to_string(id_) + "_events",
          &flutter::StandardMethodCodec::GetInstance());
  auto alive_ev = alive_;
  auto handler = std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [this, alive_ev](const flutter::EncodableValue*,
             std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& sink) {
        if (!*alive_ev) return nullptr;
        event_sink_ = std::move(sink);
        return nullptr;
      },
      [this, alive_ev](const flutter::EncodableValue*) {
        if (!*alive_ev) return nullptr;
        event_sink_ = nullptr;
        return nullptr;
      });
  event_channel_->SetStreamHandler(std::move(handler));

  InitializeEnvironment();
}

WebView2View::~WebView2View() {
  // Signal all captured alive_ guards: channel handlers and WebView2 callbacks
  // will bail immediately without touching 'this'.
  *alive_ = false;
  event_sink_ = nullptr;

  // Only Win32 operations need a valid HWND.  SubclassProc nulls parent_hwnd_
  // on WM_DESTROY; IsWindow() guards the case where the Flutter child HWND was
  // destroyed (by FlutterViewController teardown) before engine shutdown.
  if (parent_hwnd_ && IsWindow(parent_hwnd_)) {
    RemoveWindowSubclass(parent_hwnd_, SubclassProc,
                         reinterpret_cast<UINT_PTR>(this));
    if (controller_) controller_->put_IsVisible(FALSE);
  }

  // WebView2 COM operations do not need a valid parent HWND.
  if (webview_) {
    webview_->remove_NavigationStarting(nav_starting_token_);
    webview_->remove_NewWindowRequested(new_window_token_);
    webview_->remove_WebMessageReceived(web_message_token_);
    Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
    if (SUCCEEDED(webview_.As(&webview11)) && webview11) {
      webview11->remove_ContextMenuRequested(context_menu_token_);
    }
    webview_ = nullptr;
  }
  if (controller_) {
    // Close() must always be called — even if the parent HWND is gone —
    // so WebView2 releases its browser process references.
    controller_->Close();
    controller_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// HWND subclass — close controller before host window is destroyed
// ---------------------------------------------------------------------------

LRESULT CALLBACK WebView2View::SubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                             LPARAM lp, UINT_PTR id,
                                             DWORD_PTR ref) {
  if (msg == WM_DESTROY) {
    auto* self = reinterpret_cast<WebView2View*>(ref);
    // Remove first so we don't re-enter on recursive DestroyWindow calls.
    RemoveWindowSubclass(hwnd, SubclassProc, id);
    // Null this out so the destructor knows the HWND is already gone.
    self->parent_hwnd_ = nullptr;
    // Close the WebView2 controller while the host HWND is still valid.
    if (self->webview_) {
      self->webview_->remove_NavigationStarting(self->nav_starting_token_);
      self->webview_->remove_NewWindowRequested(self->new_window_token_);
      self->webview_->remove_WebMessageReceived(self->web_message_token_);
      Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
      if (SUCCEEDED(self->webview_.As(&webview11)) && webview11) {
        webview11->remove_ContextMenuRequested(self->context_menu_token_);
      }
      self->webview_ = nullptr;
    }
    if (self->controller_) {
      self->controller_->put_IsVisible(FALSE);
      self->controller_->Close();
      self->controller_ = nullptr;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// WebView2 async initialization
// ---------------------------------------------------------------------------

void WebView2View::InitializeEnvironment() {
  wchar_t local_app_data[MAX_PATH] = {};
  SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app_data);
  std::wstring user_data_dir = std::wstring(local_app_data) + L"\\html_view_webview2";

  auto alive = alive_;
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_dir.c_str(), nullptr,
      Microsoft::WRL::Callback<
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, alive](HRESULT hr,
                        ICoreWebView2Environment* env) -> HRESULT {
            if (!*alive) return S_OK;
            if (SUCCEEDED(hr) && env) {
              OnEnvironmentCreated(env);
            }
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    // WebView2 runtime not installed — pending calls will never flush.
  }
}

void WebView2View::OnEnvironmentCreated(ICoreWebView2Environment* env) {
  env_.Attach(env);
  env->AddRef();

  auto alive = alive_;
  env_->CreateCoreWebView2Controller(
      parent_hwnd_,
      Microsoft::WRL::Callback<
          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [this, alive](HRESULT hr,
                        ICoreWebView2Controller* controller) -> HRESULT {
            if (!*alive) return S_OK;
            if (SUCCEEDED(hr) && controller) {
              OnControllerCreated(controller);
            }
            return S_OK;
          })
          .Get());
}

void WebView2View::OnControllerCreated(ICoreWebView2Controller* controller) {
  controller_.Attach(controller);
  controller->AddRef();

  controller_->get_CoreWebView2(webview_.GetAddressOf());
  SetupWebView();

  // Show at current bounds (may be 0,0 until setSize/setPosition arrive).
  UpdateBounds();
  controller_->put_IsVisible(TRUE);

  // Subclass the parent HWND so we learn about WM_DESTROY before it fires.
  // WebView2 must be closed BEFORE the host window is destroyed to avoid an AV.
  SetWindowSubclass(parent_hwnd_, SubclassProc,
                    reinterpret_cast<UINT_PTR>(this),
                    reinterpret_cast<DWORD_PTR>(this));

  ready_ = true;
  FlushPending();
}

void WebView2View::SetupWebView() {
  // Basic settings
  Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(settings.GetAddressOf()))) {
    settings->put_IsScriptEnabled(TRUE);
    // Context menus are enabled so the spell-checker's suggestion menu can
    // appear on misspelled words. The ContextMenuRequested handler below limits
    // the menu to editable content (the compose editor) and suppresses it
    // everywhere else, so read-only email bodies keep their menu-free behaviour.
    settings->put_AreDefaultContextMenusEnabled(TRUE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
  }

  // Inject channel bridge on every document start
  webview_->AddScriptToExecuteOnDocumentCreated(kJsBridge, nullptr);

  // Intercept http/https/mailto navigations (user-clicked links in email HTML).
  // Cancel and emit onLinkOpened so Dart can open the URL externally.
  // data:/file:/about: and similar are allowed through for normal page loads.
  auto alive_nav = alive_;
  webview_->add_NavigationStarting(
      Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this, alive_nav](ICoreWebView2*,
                            ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            if (!*alive_nav) return S_OK;
            LPWSTR uri_raw = nullptr;
            args->get_Uri(&uri_raw);
            if (uri_raw) {
              std::wstring uri(uri_raw);
              CoTaskMemFree(uri_raw);
              auto colon = uri.find(L':');
              if (colon != std::wstring::npos) {
                auto scheme = uri.substr(0, colon);
                if (scheme == L"http" || scheme == L"https" || scheme == L"mailto") {
                  args->put_Cancel(TRUE);
                  EmitEvent("onLinkOpened", WideToUtf8(uri.c_str()));
                }
              }
            }
            return S_OK;
          }).Get(),
      &nav_starting_token_);

  // Intercept target="_blank" / window.open() links (very common in HTML
  // newsletters/marketing email). Unhandled, WebView2's default action is to
  // spawn its own unmanaged popup window to host the URL — which looks like
  // a stray "new browser instance" and bypasses url_launcher entirely. Route
  // it through the same onLinkOpened path as NavigationStarting instead.
  auto alive_new_win = alive_;
  webview_->add_NewWindowRequested(
      Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this, alive_new_win](ICoreWebView2*,
                                ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (!*alive_new_win) return S_OK;
            args->put_Handled(TRUE);
            LPWSTR uri_raw = nullptr;
            args->get_Uri(&uri_raw);
            if (uri_raw) {
              std::wstring uri(uri_raw);
              CoTaskMemFree(uri_raw);
              EmitEvent("onLinkOpened", WideToUtf8(uri.c_str()));
            }
            return S_OK;
          }).Get(),
      &new_window_token_);

  // Receive messages from JS (channel\x01payload)
  auto alive = alive_;
  webview_->add_WebMessageReceived(
      Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*,
                        ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!*alive) return S_OK;
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
              std::wstring msg(raw);
              CoTaskMemFree(raw);
              auto sep = msg.find(L'\x01');
              if (sep != std::wstring::npos) {
                EmitEvent(WideToUtf8(msg.substr(0, sep).c_str()),
                          WideToUtf8(msg.substr(sep + 1).c_str()));
              }
            }
            return S_OK;
          })
          .Get(),
      &web_message_token_);

  // Context menu: only show it on editable content (the compose editor), where
  // its main purpose is the spell-checker's "did you mean" suggestions — the
  // browser inserts those into the menu automatically and applies the chosen
  // word for us. On non-editable content (rendered email bodies) suppress the
  // menu entirely, matching the plugin's prior menu-free behaviour.
  Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
  if (SUCCEEDED(webview_.As(&webview11)) && webview11) {
    auto alive_ctx = alive_;
    webview11->add_ContextMenuRequested(
        Microsoft::WRL::Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
            [this, alive_ctx](ICoreWebView2*,
                              ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
              if (!*alive_ctx) return S_OK;
              Microsoft::WRL::ComPtr<ICoreWebView2ContextMenuTarget> target;
              args->get_ContextMenuTarget(&target);
              BOOL editable = FALSE;
              if (target) target->get_IsEditable(&editable);
              if (!editable) {
                // Handled with no SelectedCommandId set = no menu is shown.
                args->put_Handled(TRUE);
              }
              // Editable: leave Handled FALSE so WebView2 renders its own menu
              // (including spelling suggestions) and applies the user's choice.
              return S_OK;
            })
            .Get(),
        &context_menu_token_);
  }
}

void WebView2View::FlushPending() {
  while (!pending_.empty()) {
    pending_.front()();
    pending_.pop();
  }
}

// ---------------------------------------------------------------------------
// Method channel dispatch
// ---------------------------------------------------------------------------

void WebView2View::HandleMethod(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

  const auto& name = call.method_name();

  if (name == "loadHtml") {
    const auto* html = std::get_if<std::string>(call.arguments());
    if (!html) { result->Error("bad_args", "expected string"); return; }
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    auto h = *html;
    auto do_load = [this, h, shared]() {
      std::wstring whtml = Utf8ToWide(h);
      HRESULT hr = webview_->NavigateToString(whtml.c_str());
      if (SUCCEEDED(hr)) shared->Success();
      else shared->Error("load_failed", "NavigateToString failed");
    };
    if (ready_) do_load(); else pending_.push(std::move(do_load));

  } else if (name == "loadUrl") {
    const auto* url = std::get_if<std::string>(call.arguments());
    if (!url) { result->Error("bad_args", "expected string"); return; }
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    auto u = *url;
    auto do_load = [this, u, shared]() {
      std::wstring wurl = Utf8ToWide(u);
      HRESULT hr = webview_->Navigate(wurl.c_str());
      if (SUCCEEDED(hr)) shared->Success();
      else shared->Error("navigate_failed", "Navigate failed");
    };
    if (ready_) do_load(); else pending_.push(std::move(do_load));

  } else if (name == "loadAsset") {
    const auto* key = std::get_if<std::string>(call.arguments());
    if (!key) { result->Error("bad_args", "expected string asset key"); return; }
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    if (ready_) {
      DoLoadAsset(*key, shared);
    } else {
      auto k = *key;
      pending_.push([this, k, shared]() { DoLoadAsset(k, shared); });
    }

  } else if (name == "eval") {
    const auto* js = std::get_if<std::string>(call.arguments());
    if (!js) { result->Error("bad_args", "expected string js"); return; }
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    if (ready_) {
      DoEval(*js, shared);
    } else {
      auto j = *js;
      pending_.push([this, j, shared]() { DoEval(j, shared); });
    }

  } else if (name == "setPosition") {
    const auto* list =
        std::get_if<flutter::EncodableList>(call.arguments());
    if (!list || list->size() != 3) { result->Error("bad_args"); return; }
    SetPosition(std::get<double>((*list)[0]),
                std::get<double>((*list)[1]),
                std::get<double>((*list)[2]));
    result->Success();

  } else if (name == "setSize") {
    const auto* list =
        std::get_if<flutter::EncodableList>(call.arguments());
    if (!list || list->size() != 3) { result->Error("bad_args"); return; }
    SetSize(std::get<double>((*list)[0]),
            std::get<double>((*list)[1]),
            std::get<double>((*list)[2]));
    result->Success();

  } else if (name == "focus") {
    // JS-side element.focus() only moves focus within the web content; the
    // WebView2 HWND itself also needs OS-level keyboard focus, or neither a
    // visible caret nor keystrokes reach it while the host Flutter window
    // still holds focus. MoveFocus grants that at the OS level.
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    auto do_focus = [this, shared]() {
      if (controller_) {
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
      shared->Success();
    };
    if (ready_) do_focus(); else pending_.push(std::move(do_focus));

  } else if (name == "printCurrent") {
    auto shared = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
        std::move(result));
    auto do_print = [this, shared]() {
      if (webview_) {
        webview_->ExecuteScript(L"window.print()", nullptr);
      }
      shared->Success();
    };
    if (ready_) do_print(); else pending_.push(std::move(do_print));

  } else if (name == "setVisible") {
    const auto* v = std::get_if<bool>(call.arguments());
    if (!v) { result->Error("bad_args"); return; }
    if (controller_) controller_->put_IsVisible(*v ? TRUE : FALSE);
    result->Success();

  } else if (name == "dispose") {
    result->Success();

  } else {
    result->NotImplemented();
  }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

void WebView2View::DoLoadAsset(
    const std::string& asset_key,
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  std::wstring uri = PathToFileUri(AssetPath(asset_key));
  HRESULT hr = webview_->Navigate(uri.c_str());
  if (SUCCEEDED(hr)) {
    result->Success();
  } else {
    result->Error("navigate_failed", "WebView2 Navigate returned error");
  }
}

void WebView2View::DoEval(
    const std::string& js,
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  std::wstring wjs = Utf8ToWide(js);
  auto alive = alive_;
  HRESULT hr = webview_->ExecuteScript(
      wjs.c_str(),
      Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [alive, result](HRESULT hr, LPCWSTR result_json) -> HRESULT {
            if (!*alive) return S_OK;
            if (SUCCEEDED(hr)) {
              result->Success(flutter::EncodableValue(
                  WebView2View::WideToUtf8(result_json)));
            } else {
              result->Error("eval_failed");
            }
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    result->Error("eval_failed", "ExecuteScript call failed");
  }
}

void WebView2View::SetPosition(double x, double y, double dpr) {
  phys_x_ = x * dpr;
  phys_y_ = y * dpr;
  UpdateBounds();
}

void WebView2View::SetSize(double w, double h, double dpr) {
  phys_w_ = w * dpr;
  phys_h_ = h * dpr;
  UpdateBounds();
}

void WebView2View::UpdateBounds() {
  if (!controller_) return;
  RECT bounds;
  bounds.left   = static_cast<LONG>(phys_x_);
  bounds.top    = static_cast<LONG>(phys_y_);
  bounds.right  = static_cast<LONG>(phys_x_ + phys_w_);
  bounds.bottom = static_cast<LONG>(phys_y_ + phys_h_);
  controller_->put_Bounds(bounds);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void WebView2View::EmitEvent(const std::string& type,
                              const std::string& value) {
  if (!event_sink_) return;
  event_sink_->Success(flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("type"),  flutter::EncodableValue(type)},
      {flutter::EncodableValue("value"), flutter::EncodableValue(value)},
  }));
}

}  // namespace html_view
