import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:image_picker/image_picker.dart';
import 'package:url_launcher/url_launcher.dart';

import '../backend/api.dart';
import '../halo_runtime.dart';
import '../wallpaper/dither.dart';

/// Settings (P4): the badge's orientation, its wallpaper, and the Google
/// account the backend acts on.
class SettingsTab extends StatefulWidget {
  const SettingsTab({super.key, required this.rt, this.active = false});
  final HaloRuntime rt;

  /// True while this tab is on screen: it loads from the backend each time it
  /// is opened, by then the backend client exists (it is created after the
  /// app starts).
  final bool active;
  @override
  State<SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<SettingsTab> with WidgetsBindingObserver {
  HaloRuntime get _rt => widget.rt;

  // Wallpaper preview, rebuilt whenever the photo, the fit or the
  // orientation changes ([_previewFor] is what it was made from).
  Uint8List? _preview;
  Object? _previewFor;
  bool _preparing = false;
  String? _pickNote;

  // Google
  GoogleStatus? _google;
  String? _googleNote;
  Timer? _poll;
  DateTime? _pollUntil;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    if (widget.active) unawaited(_refreshGoogle());
  }

  @override
  void didUpdateWidget(covariant SettingsTab old) {
    super.didUpdateWidget(old);
    if (widget.active && !old.active) unawaited(_refreshGoogle());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _poll?.cancel();
    super.dispose();
  }

  /// Back from the browser after Google sign in: check right away.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) unawaited(_refreshGoogle());
  }

  // ------------------------------------------------------------ wallpaper

  Future<void> _pick() async {
    // Size limits make Android decode the photo and re-encode it as JPEG, so
    // HEIC photos (the default on Samsung cameras), which the image package
    // cannot read, still work; 1600 px is double the panel's long side.
    final file = await ImagePicker().pickImage(
        source: ImageSource.gallery, maxWidth: 1600, maxHeight: 1600, imageQuality: 95);
    if (file == null) return;
    setState(() => _pickNote = null);
    await _rt.setWallpaperPhoto(await file.readAsBytes());
  }

  Object? get _previewKey => _rt.wallpaperPhoto == null
      ? null
      : (identityHashCode(_rt.wallpaperPhoto), _rt.wallpaperFit, _rt.orientation);

  Future<void> _refreshPreview() async {
    final key = _previewKey;
    if (key == null || _preparing) return;
    setState(() => _preparing = true);
    try {
      final size = _rt.panelSize;
      final sw = Stopwatch()..start();
      final bits = await _rt.wallpaperBits();
      final png = await compute(previewPng, (bits, size.w, size.h));
      _rt.record('P4LOG wallpaper preview ${size.w}x${size.h} ${_rt.wallpaperFit.name} in ${sw.elapsedMilliseconds} ms');
      if (!mounted) return;
      setState(() {
        _preview = png;
        _previewFor = key;
        _pickNote = null;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _preview = null;
        _previewFor = key; // do not retry the same picture in a loop
        _pickNote = 'Could not use that picture: $e';
      });
    } finally {
      if (mounted) setState(() => _preparing = false);
    }
  }

  Future<void> _send() async {
    try {
      await _rt.sendWallpaper();
    } catch (_) {
      // The runtime shows the reason in wallpaperNote.
    }
  }

  // --------------------------------------------------------------- Google

  Future<void> _refreshGoogle() async {
    final api = _rt.google;
    if (api == null) return;
    try {
      final s = await api.googleStatus();
      if (!mounted) return;
      setState(() => _google = s);
      if (s.connected) _stopPolling();
    } catch (e) {
      if (mounted) setState(() => _googleNote = _describe(e));
    }
  }

  static String _describe(Object e) {
    if (e is ApiError && e.httpStatus == 422) {
      return 'Google sign in needs a phone key; the sample data key cannot connect.';
    }
    if (e is ApiError) return e.message.isNotEmpty ? e.message : e.code;
    return "Can't reach HaloAI. Check your connection.";
  }

  Future<void> _connect() async {
    setState(() => _googleNote = null);
    try {
      final url = await _rt.google!.googleConnect();
      _rt.record('P4LOG google connect: opening the sign in page');
      if (!await launchUrl(url, mode: LaunchMode.externalApplication)) {
        throw StateError('no browser to open the sign in page');
      }
      // Nothing tells the phone when sign in finishes: poll until connected,
      // for as long as the sign in link is valid (10 minutes).
      _pollUntil = DateTime.now().add(const Duration(minutes: 10));
      _poll?.cancel();
      _poll = Timer.periodic(const Duration(seconds: 2), (_) {
        if (DateTime.now().isAfter(_pollUntil!)) return _stopPolling();
        unawaited(_refreshGoogle());
      });
      setState(() => _googleNote = 'Finish signing in in the browser, then come back.');
    } catch (e) {
      setState(() => _googleNote = _describe(e));
    }
  }

  void _stopPolling() {
    _poll?.cancel();
    _poll = null;
    if (mounted && _google?.connected == true) setState(() => _googleNote = null);
  }

  Future<void> _disconnect() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Disconnect Google?'),
        content: Text('HaloAI will stop using ${_google?.email ?? 'this account'} for your calendar and email.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Keep')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Disconnect')),
        ],
      ),
    );
    if (ok != true) return;
    try {
      await _rt.google!.googleDisconnect();
      _rt.record('P4LOG google disconnected');
      await _refreshGoogle();
    } catch (e) {
      setState(() => _googleNote = _describe(e));
    }
  }

  // ----------------------------------------------------------------- view

  @override
  Widget build(BuildContext context) {
    if (_previewKey != null && _previewKey != _previewFor && !_preparing) {
      WidgetsBinding.instance.addPostFrameCallback((_) => unawaited(_refreshPreview()));
    }
    final t = Theme.of(context).textTheme;
    final small = t.bodySmall;
    return ListView(padding: const EdgeInsets.all(16), children: [
      Text('Badge orientation', style: t.titleSmall),
      const SizedBox(height: 8),
      SegmentedButton<String>(
        segments: const [
          ButtonSegment(value: 'landscape', label: Text('Landscape'), icon: Icon(Icons.crop_landscape)),
          ButtonSegment(value: 'portrait', label: Text('Portrait'), icon: Icon(Icons.crop_portrait)),
        ],
        selected: {_rt.orientation},
        onSelectionChanged: (s) => unawaited(_rt.setOrientation(s.first)),
      ),
      Text('Sent to the badge and with every request.', style: small),
      const Divider(height: 32),
      Text('Wallpaper', style: t.titleSmall),
      const SizedBox(height: 8),
      Row(children: [
        OutlinedButton.icon(
          onPressed: _rt.wallpaperSending ? null : _pick,
          icon: const Icon(Icons.photo),
          label: Text(_rt.wallpaperPhoto == null ? 'Pick a photo' : 'Change photo'),
        ),
        const SizedBox(width: 8),
        FilledButton(
          onPressed: _rt.wallpaperPhoto != null && _rt.deviceTakesWallpaper && !_rt.wallpaperSending && !_preparing
              ? _send
              : null,
          child: Text(_rt.wallpaperSending ? 'Sending...' : 'Send to badge'),
        ),
      ]),
      if (_rt.wallpaperPhoto != null) ...[
        const SizedBox(height: 8),
        SegmentedButton<WallpaperFit>(
          segments: const [
            ButtonSegment(value: WallpaperFit.fill, label: Text('Fill (crop)'), icon: Icon(Icons.crop)),
            ButtonSegment(value: WallpaperFit.fit, label: Text('Fit (whole photo)'), icon: Icon(Icons.fit_screen)),
          ],
          selected: {_rt.wallpaperFit},
          onSelectionChanged: _rt.wallpaperSending ? null : (s) => unawaited(_rt.setWallpaperFit(s.first)),
        ),
        Text(
            _rt.wallpaperFit == WallpaperFit.fill
                ? 'Covers the whole screen; the edges of the photo that do not fit are cut off.'
                : 'Shows the whole photo, with white bars where its shape differs from the screen.',
            style: small),
        const SizedBox(height: 8),
        // Exactly the bits that would be sent, one pixel per panel pixel.
        AspectRatio(
          aspectRatio: _rt.panelSize.w / _rt.panelSize.h,
          child: DecoratedBox(
            decoration: BoxDecoration(border: Border.all(color: Theme.of(context).colorScheme.outline)),
            child: _preview == null || _previewFor != _previewKey
                ? const Center(child: CircularProgressIndicator())
                : Image.memory(_preview!, filterQuality: FilterQuality.none, fit: BoxFit.fill, gaplessPlayback: true),
          ),
        ),
        Text('${_rt.panelSize.w} x ${_rt.panelSize.h} (${_rt.orientation}), black and white, exactly as sent.',
            style: small),
      ],
      if (_pickNote != null) Text(_pickNote!, style: small),
      if (_rt.link.state.name == 'connected' && !_rt.deviceTakesWallpaper)
        Text("This badge's firmware can't receive a wallpaper, so Send stays off.", style: small)
      else if (!_rt.deviceTakesWallpaper && _rt.wallpaperPhoto != null)
        Text('Connect the badge to send it.', style: small),
      if (_rt.wallpaperStale)
        Text(
            'The badge has this wallpaper for ${_rt.wallpaperOnBadgeFor}; it is sent again for ${_rt.orientation} '
            'when the badge connects.',
            style: small),
      if (_rt.wallpaperNote != null) Text(_rt.wallpaperNote!, style: small),
      const Divider(height: 32),
      Text('Google account', style: t.titleSmall),
      const SizedBox(height: 8),
      if (_rt.google == null)
        Text('Backend off: build with --dart-define-from-file=config/backend.json', style: small)
      else ...[
        Text(_google == null
            ? 'Checking...'
            : _google!.connected
                ? 'Connected as ${_google!.email}'
                : 'Not connected'),
        const SizedBox(height: 8),
        Row(children: [
          if (_google?.connected == true)
            OutlinedButton(onPressed: _disconnect, child: const Text('Disconnect'))
          else
            FilledButton(onPressed: _connect, child: const Text('Connect Google')),
          const SizedBox(width: 8),
          TextButton(onPressed: _refreshGoogle, child: const Text('Refresh')),
        ]),
        if (_googleNote != null) Text(_googleNote!, style: small),
        if (_rt.google != _rt.api) Text('Using the separate Google test key.', style: small),
      ],
    ]);
  }
}
