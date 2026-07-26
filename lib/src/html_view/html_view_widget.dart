import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';
import 'package:window_manager/window_manager.dart';

import 'html_view_controller.dart';
import 'html_view_overlay_guard.dart';

class HtmlViewWidget extends StatefulWidget {
  const HtmlViewWidget({
    required this.controller,
    this.scaleFactor,
    super.key,
  });

  final HtmlViewController controller;

  /// Override the device pixel ratio. Defaults to [View.devicePixelRatio].
  final double? scaleFactor;

  @override
  State<HtmlViewWidget> createState() => _HtmlViewWidgetState();
}

class _HtmlViewWidgetState extends State<HtmlViewWidget>
    with WindowListener {
  final _key = GlobalKey();

  // WebView2 (a native Win32 child window) always paints over Flutter's
  // DirectX surface, so we hide it whenever something Flutter-rendered
  // should appear on top of it: either a ModalRoute (dialog, bottom sheet)
  // pushed above this widget's route, or a non-route overlay (hover card,
  // dropdown) that has claimed HtmlViewOverlayGuard.
  bool _isModalCurrent = true;

  bool _disposed = false;
  Offset? _lastPos;
  Size? _lastSize;

  // The native view isn't clipped by Flutter's own clip regions — an
  // ancestor Scrollable's viewport is invisible to it. Without this, once
  // the widget scrolls fully out of that viewport it keeps rendering at its
  // (correctly tracked) position anyway, as a ghost overlapping whatever
  // Flutter content is now underneath it. Force it hidden in that case;
  // repositioning alone (see _scheduleSync) isn't enough.
  bool _scrolledOutOfView = false;

  @override
  void initState() {
    super.initState();
    windowManager.addListener(this);
    HtmlViewOverlayGuard.activeCount.addListener(_onGuardChanged);
    _scheduleSync();
  }

  // The native view is positioned by absolute screen coordinates outside
  // Flutter's own compositor, so it has to be told explicitly whenever this
  // widget's on-screen position changes. Layout/window-event hooks alone
  // miss the common case of an ancestor Scrollable: scrolling moves content
  // via a paint-phase offset, not a relayout, so nothing else here would
  // ever see it move. Re-checking every frame (piggy-backing on whatever
  // frames Flutter is already producing — a scroll, an animation, a resize —
  // rather than forcing our own) catches that without polling while idle.
  void _scheduleSync() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_disposed || !mounted) return;
      _reportPosition();
      _reportSize();
      _updateScrollVisibility();
      _scheduleSync();
    });
  }

  /// Global rect of the nearest ancestor Scrollable's viewport, or null if
  /// this widget isn't inside one.
  Rect? _scrollableViewportGlobalRect() {
    final scrollable = Scrollable.maybeOf(context);
    final viewportBox =
        scrollable?.context.findRenderObject() as RenderBox?;
    if (viewportBox == null || !viewportBox.attached) return null;
    return viewportBox.localToGlobal(Offset.zero) & viewportBox.size;
  }

  void _updateScrollVisibility() {
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box == null || !box.attached) return;
    final viewportRect = _scrollableViewportGlobalRect();
    if (viewportRect == null) return;
    final selfRect = box.localToGlobal(Offset.zero) & box.size;
    final outOfView = !selfRect.overlaps(viewportRect);
    if (outOfView != _scrolledOutOfView) {
      _scrolledOutOfView = outOfView;
      _applyVisibility();
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    // ModalRoute.of() registers a dependency on _ModalScopeStatus so this
    // method is called whenever a route is pushed/popped above this widget.
    final isCurrent = ModalRoute.of(context)?.isCurrent ?? true;
    if (isCurrent != _isModalCurrent) {
      _isModalCurrent = isCurrent;
      _applyVisibility();
    }
  }

  void _onGuardChanged() => _applyVisibility();

  void _applyVisibility() {
    final visible = _isModalCurrent &&
        HtmlViewOverlayGuard.activeCount.value == 0 &&
        !_scrolledOutOfView;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) unawaited(widget.controller.setVisible(visible));
    });
  }

  @override
  void onWindowMove() {
    _reportPosition();
  }

  @override
  void onWindowResize() {
    _reportPosition();
    _reportSize();
  }

  double get _dpr =>
      widget.scaleFactor ??
      ui.PlatformDispatcher.instance.views.first.devicePixelRatio;

  void _reportPosition() {
    if (!mounted) return;
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box == null || !box.attached) return;
    final pos = box.localToGlobal(Offset.zero);
    if (pos == _lastPos) return;
    _lastPos = pos;
    unawaited(widget.controller.setPosition(pos.dx, pos.dy, _dpr));
  }

  void _reportSize() {
    if (!mounted) return;
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box == null || !box.attached) return;
    if (box.size == _lastSize) return;
    _lastSize = box.size;
    unawaited(
        widget.controller.setSize(box.size.width, box.size.height, _dpr));
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox.expand(key: _key);
  }

  @override
  void dispose() {
    _disposed = true;
    windowManager.removeListener(this);
    HtmlViewOverlayGuard.activeCount.removeListener(_onGuardChanged);
    super.dispose();
  }
}
