#include "include/sharpblue_library/html_view_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "html_view_plugin.h"

void HtmlViewPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  html_view::HtmlViewPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
