#include "html_view_plugin.h"

#include <flutter/standard_method_codec.h>

namespace html_view {

// static
void HtmlViewPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<HtmlViewPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

HtmlViewPlugin::HtmlViewPlugin(flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar) {
  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "html_view",
      &flutter::StandardMethodCodec::GetInstance());
  // Capture alive_ in the handler so the destructor never needs to call
  // SetMethodCallHandler(nullptr) — that goes through a raw messenger pointer
  // which can be freed before plugin destructors run in secondary-window teardown.
  auto alive = alive_;
  channel_->SetMethodCallHandler([this, alive](const auto& call, auto result) {
    if (!*alive) { result->NotImplemented(); return; }
    HandleMethod(call, std::move(result));
  });
}

HtmlViewPlugin::~HtmlViewPlugin() {
  *alive_ = false;  // Blocks the channel handler without touching the messenger.
  views_.clear();   // ~WebView2View() is safe: its alive_ guards also fire.
}

void HtmlViewPlugin::HandleMethod(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

  if (call.method_name() == "createView") {
    HWND parent = registrar_->GetView()->GetNativeWindow();
    int64_t id = next_id_++;
    views_[id] = std::make_unique<WebView2View>(id, parent,
                                                 registrar_->messenger());
    result->Success(flutter::EncodableValue(id));

  } else if (call.method_name() == "destroyView") {
    // Dart encodes small integers as int32; accept both widths.
    int64_t id = -1;
    if (const auto* v = std::get_if<int64_t>(call.arguments())) {
      id = *v;
    } else if (const auto* v = std::get_if<int32_t>(call.arguments())) {
      id = *v;
    } else {
      result->Error("bad_args"); return;
    }
    views_.erase(id);
    result->Success();

  } else {
    result->NotImplemented();
  }
}

}  // namespace html_view
