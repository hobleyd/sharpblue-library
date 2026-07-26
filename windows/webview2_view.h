#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <flutter/encodable_value.h>
#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/method_result.h>
#include <flutter/binary_messenger.h>

#include <functional>
#include <memory>
#include <queue>
#include <string>

namespace html_view {

class WebView2View {
 public:
  WebView2View(int64_t id, HWND parent_hwnd,
               flutter::BinaryMessenger* messenger);
  ~WebView2View();

  WebView2View(const WebView2View&) = delete;
  WebView2View& operator=(const WebView2View&) = delete;

  void HandleMethod(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  void InitializeEnvironment();
  void OnEnvironmentCreated(ICoreWebView2Environment* env);
  void OnControllerCreated(ICoreWebView2Controller* controller);
  void SetupWebView();
  void FlushPending();

  void DoLoadAsset(const std::string& asset_key,
                   std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void DoEval(const std::string& js,
              std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void SetPosition(double x, double y, double dpr);
  void SetSize(double w, double h, double dpr);
  void UpdateBounds();

  void EmitEvent(const std::string& type, const std::string& value);

  // Subclass proc installed on the parent HWND so we can call Close() on the
  // WebView2 controller before the host window is destroyed.
  static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp, UINT_PTR id, DWORD_PTR ref);

  static std::wstring GetExecutableDir();
  static std::wstring AssetPath(const std::string& key);
  static std::wstring PathToFileUri(const std::wstring& path);
  static std::string WideToUtf8(const wchar_t* wide);
  static std::wstring Utf8ToWide(const std::string& utf8);

  int64_t id_;
  HWND parent_hwnd_;

  // Physical pixel bounds (set by setPosition + setSize from Dart)
  double phys_x_ = 0.0, phys_y_ = 0.0;
  double phys_w_ = 1.0, phys_h_ = 1.0;

  // Alive guard: shared with all outstanding callbacks so they can bail
  // safely if this view is destroyed before the callback fires.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  bool ready_ = false;
  std::queue<std::function<void()>> pending_;

  Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
  EventRegistrationToken web_message_token_{};
  EventRegistrationToken nav_starting_token_{};
  EventRegistrationToken new_window_token_{};
  EventRegistrationToken context_menu_token_{};

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> event_channel_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
};

}  // namespace html_view
