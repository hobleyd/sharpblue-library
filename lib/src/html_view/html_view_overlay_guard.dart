import 'package:flutter/foundation.dart';

/// Signal for Flutter overlays (hover cards, dropdowns, etc.) that render on
/// top of an [HtmlViewWidget] but aren't backed by a [ModalRoute] — those
/// don't trip the ModalRoute-based native-hide check already in
/// [HtmlViewWidget], so they'd otherwise be obscured by the native WebView2
/// HWND, which always paints over Flutter's DirectX surface on Windows.
///
/// Call [acquire] before showing such an overlay and [release] once it's
/// dismissed. Uses a count rather than a bool so multiple overlays can be
/// open across different [HtmlViewWidget] instances at once.
class HtmlViewOverlayGuard {
  HtmlViewOverlayGuard._();

  static final ValueNotifier<int> activeCount = ValueNotifier(0);

  static void acquire() => activeCount.value++;

  static void release() {
    if (activeCount.value > 0) activeCount.value--;
  }
}
