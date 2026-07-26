#pragma once

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <string>

struct WebkitView {
  gint64 id;
  WebKitWebView* web_view;
  GtkOverlay* overlay;

  FlMethodChannel* channel;
  FlEventChannel* event_channel;

  FlMethodCall* pending_eval;

  gint pos_x;
  gint pos_y;
  gint width;
  gint height;

  gboolean alive;

  WebkitView(gint64 id, GtkOverlay* overlay, FlBinaryMessenger* messenger);
  ~WebkitView();

  void HandleMethod(FlMethodCall* method_call);
  void EmitEvent(const char* type, const char* value);
  void UpdatePosition();

 private:
  void DoLoadAsset(const std::string& key, FlMethodCall* call);
  void DoEval(const std::string& js, FlMethodCall* call);

  static std::string GetExecutableDir();
  static std::string AssetPath(const std::string& key);
};
