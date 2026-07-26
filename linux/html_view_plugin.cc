#include "include/html_view/html_view_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <unordered_map>
#include <memory>

#include "webkit_view.h"

struct _HtmlViewPlugin {
  GObject parent_instance;

  GtkOverlay* overlay;   // resolved at registration time from FlView's parent
  FlBinaryMessenger* messenger;
  std::unordered_map<gint64, std::unique_ptr<WebkitView>>* views;
  gint64 next_id;
};

G_DEFINE_TYPE(HtmlViewPlugin, html_view_plugin, G_TYPE_OBJECT)

// ---------------------------------------------------------------------------
// Method call handler
// ---------------------------------------------------------------------------

static void method_call_handler(FlMethodChannel*,
                                 FlMethodCall* method_call,
                                 gpointer user_data) {
  HtmlViewPlugin* self = HTML_VIEW_PLUGIN(user_data);
  const char* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "createView") == 0) {
    if (!self->overlay) {
      fl_method_call_respond_error(method_call, "no_overlay",
                                   "GtkOverlay not found", nullptr, nullptr);
      return;
    }

    gint64 id = self->next_id++;
    auto view = std::make_unique<WebkitView>(id, self->overlay, self->messenger);
    (*self->views)[id] = std::move(view);
    g_autoptr(FlValue) resp = fl_value_new_int(id);
    fl_method_call_respond_success(method_call, resp, nullptr);

  } else if (strcmp(method, "destroyView") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    gint64 id = fl_value_get_int(args);
    self->views->erase(id);
    fl_method_call_respond_success(method_call, nullptr, nullptr);

  } else {
    fl_method_call_respond_not_implemented(method_call, nullptr);
  }
}

// ---------------------------------------------------------------------------
// GObject lifecycle
// ---------------------------------------------------------------------------

static void html_view_plugin_dispose(GObject* object) {
  HtmlViewPlugin* self = HTML_VIEW_PLUGIN(object);
  delete self->views;
  self->views = nullptr;
  G_OBJECT_CLASS(html_view_plugin_parent_class)->dispose(object);
}

static void html_view_plugin_class_init(HtmlViewPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = html_view_plugin_dispose;
}

static void html_view_plugin_init(HtmlViewPlugin* self) {
  self->overlay   = nullptr;
  self->next_id   = 1;
  self->views     = new std::unordered_map<gint64, std::unique_ptr<WebkitView>>();
  self->messenger = nullptr;
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

FLUTTER_PLUGIN_EXPORT
void html_view_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  HtmlViewPlugin* plugin = HTML_VIEW_PLUGIN(
      g_object_new(html_view_plugin_get_type(), nullptr));

  plugin->messenger = fl_plugin_registrar_get_messenger(registrar);

  // Find the GtkOverlay that wraps the FlView. my_application.cc inserts it
  // between the GtkWindow and the FlView before fl_register_plugins() is called.
  FlView* fl_view = fl_plugin_registrar_get_view(registrar);
  if (fl_view) {
    GtkWidget* parent = gtk_widget_get_parent(GTK_WIDGET(fl_view));
    if (GTK_IS_OVERLAY(parent)) {
      plugin->overlay = GTK_OVERLAY(parent);
    }
  }

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      plugin->messenger, "html_view", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      channel, method_call_handler, g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}
