import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';

import 'html_view/html_view.dart';

/// A WYSIWYG HTML editor backed by a native WebView (bold/italic/underline,
/// colour, font, lists, indent, links). Shared across Sharpblue apps so each
/// one doesn't carry its own copy of the native plugin + editor JS.
///
/// Attachments and pasted-image handling are optional — apps that don't need
/// them (e.g. a recipe editor) simply omit those callbacks and the
/// corresponding toolbar affordances stay hidden.
class RichTextEditor extends StatefulWidget {
  const RichTextEditor({
    super.key,
    required this.initialHtml,
    required this.onContentChanged,
    this.onLinkRequested,
    this.onAttachRequested,
    this.onImagePasted,
    this.onClickFocus,
    this.autofocus = false,
  });

  final String initialHtml;
  final ValueChanged<String> onContentChanged;

  /// Called when the user taps the link button in the editor toolbar. The
  /// caller should prompt for a URL and call [RichTextEditorState.insertLink].
  /// When omitted, the link button is hidden.
  final VoidCallback? onLinkRequested;

  /// Called when the user taps the attach button in the editor toolbar. When
  /// omitted (the default), the attach button stays hidden.
  final VoidCallback? onAttachRequested;

  /// Called when the user pastes an image into the editor. The argument is a
  /// `data:` URL of the pasted image; the caller should register it as an
  /// inline attachment and insert it via [RichTextEditorState.insertImage].
  final ValueChanged<String>? onImagePasted;

  /// Called when a raw click forces native OS focus onto the editor. The
  /// caller should drop focus from whatever Flutter field currently has it
  /// (e.g. `FocusManager.instance.primaryFocus?.unfocus()`), since a native
  /// focus steal doesn't otherwise reach Flutter's own FocusNode tree.
  final VoidCallback? onClickFocus;

  /// Focuses the editor as soon as its content finishes loading. The webview
  /// loads asynchronously, so this can't be done with a synchronous
  /// `requestFocus()` call from the parent the way a plain text field works.
  final bool autofocus;

  @override
  State<RichTextEditor> createState() => RichTextEditorState();
}

class RichTextEditorState extends State<RichTextEditor> {
  late final HtmlViewController _controller;
  StreamSubscription<String>? _contentSub;
  StreamSubscription<void>? _linkSub;
  StreamSubscription<void>? _loadedSub;
  StreamSubscription<void>? _attachSub;
  StreamSubscription<String>? _imagePastedSub;
  StreamSubscription<void>? _clickFocusSub;

  String _pendingHtml = '';
  bool _disposed = false;
  bool _pageLoaded = false;

  @override
  void initState() {
    super.initState();
    _pendingHtml = widget.initialHtml;

    _controller = HtmlViewController();
    _controller.initialize().then((_) {
      if (_disposed) return;
      _contentSub = _controller.onContentChanged.listen((html) {
        if (mounted) widget.onContentChanged(html);
      });
      _linkSub = _controller.onLinkRequest.listen((_) {
        if (mounted) widget.onLinkRequested?.call();
      });
      _attachSub = _controller.onAttachRequested.listen((_) {
        if (mounted) widget.onAttachRequested?.call();
      });
      _imagePastedSub = _controller.onImagePasted.listen((dataUri) {
        if (mounted) widget.onImagePasted?.call(dataUri);
      });
      _clickFocusSub = _controller.onClickFocus.listen((_) {
        if (!mounted) return;
        // Unfocus the Flutter side first — calling focus() (native
        // makeFirstResponder) before this can otherwise be undone when
        // Flutter's text input plugin reasserts itself as firstResponder
        // while resigning the old field.
        widget.onClickFocus?.call();
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted) focus();
        });
      });
      _loadedSub = _controller.onPageLoaded.listen((_) async {
        if (_disposed) return;
        _pageLoaded = true;
        await _controller.eval(
            'setAttachButtonVisible(${widget.onAttachRequested != null})');
        await _controller.eval(
            'setLinkButtonVisible(${widget.onLinkRequested != null})');
        if (_pendingHtml.isNotEmpty) {
          await _controller.eval('setContent(${jsonEncode(_pendingHtml)})');
        }
        if (widget.autofocus && !_disposed) {
          await focus();
        }
      });
      _controller.loadAsset('packages/sharpblue_library/assets/editor/editor.html');
    });
  }

  @override
  void dispose() {
    _disposed = true;
    _contentSub?.cancel();
    _linkSub?.cancel();
    _loadedSub?.cancel();
    _attachSub?.cancel();
    _imagePastedSub?.cancel();
    _clickFocusSub?.cancel();
    _controller.dispose();
    super.dispose();
  }

  // -------------------------------------------------------------------------
  // Public API
  // -------------------------------------------------------------------------

  Future<void> hide() => _controller.setVisible(false);
  Future<void> show() => _controller.setVisible(true);

  /// Safe to call before the page has finished loading (e.g. while an
  /// existing document is still being fetched from disk) — the pending
  /// value is applied once [onPageLoaded] fires instead of being lost to a
  /// native call racing the webview's own asset load.
  Future<void> setContent(String html) async {
    _pendingHtml = html;
    if (!_pageLoaded) return;
    await _controller.eval('setContent(${jsonEncode(html)})');
  }

  Future<String> getContent() async {
    if (!_pageLoaded) return _pendingHtml;
    // Returns JSON-encoded string from JS; strip outer quotes.
    final raw = await _controller.eval('getContent()');
    if (raw == null || raw == 'null') return _pendingHtml;
    // JS result is JSON: "\"<html>\"" — decode it.
    try {
      return jsonDecode(raw) as String;
    } catch (_) {
      return raw;
    }
  }

  Future<void> insertImage(String dataUri, String contentId) async {
    await _controller.eval(
        'insertImage(${jsonEncode(dataUri)}, ${jsonEncode(contentId)})');
  }

  Future<void> insertLink(String url) async {
    await _controller.eval('insertLink(${jsonEncode(url)})');
  }

  Future<void> saveSelection() async {
    await _controller.eval('saveSelection()');
  }

  Future<void> insertAtCursor(String text) async {
    await _controller.eval('insertAtSaved(${jsonEncode(text)})');
  }

  Future<void> focus() async {
    // OS focus first (so the native webview control actually receives
    // keystrokes), then the DOM-level focus that places the caret in the
    // editor.
    await _controller.focus();
    await _controller.eval('focusEditor()');
  }

  // -------------------------------------------------------------------------
  // Build
  // -------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return HtmlViewWidget(controller: _controller);
  }
}
