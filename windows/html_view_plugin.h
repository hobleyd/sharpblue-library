#pragma once

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <unordered_map>

#include "webview2_view.h"

namespace html_view {

class HtmlViewPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  explicit HtmlViewPlugin(flutter::PluginRegistrarWindows* registrar);
  ~HtmlViewPlugin() override;

  HtmlViewPlugin(const HtmlViewPlugin&) = delete;
  HtmlViewPlugin& operator=(const HtmlViewPlugin&) = delete;

 private:
  void HandleMethod(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  flutter::PluginRegistrarWindows* registrar_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unordered_map<int64_t, std::unique_ptr<WebView2View>> views_;
  int64_t next_id_ = 1;
};

}  // namespace html_view
