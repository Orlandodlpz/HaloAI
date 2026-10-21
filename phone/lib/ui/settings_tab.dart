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

  // Wallpaper
  Uint8List? _bits, _preview;
  int _w = landscape.w, _h = landscape.h;
  bool _preparing = false, _sending = false;
  String? _wallpaperNote;

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
    final file = await ImagePicker().pickImage(source: ImageSource.gallery);
    if (file == null) return;
    final size = _rt.orientation == 'portrait' ? portrait : landscape;
    setState(() {
      _preparing = true;
      _wallpaperNote = null;
    });
    try {
      final encoded = await file.readAsBytes();
      final sw = Stopwatch()..start();
      // Both run in another isolate: dithering 384,000 pixels would freeze the UI.
      final bits = await compute(prepareWallpaper, (encoded, size.w, size.h));
      final png = await compute(previewPng, (bits, size.w, size.h));
      _rt.record('P4LOG wallpaper prepared ${size.w}x${size.h} (${bits.length} bytes) in ${sw.elapsedMilliseconds} ms');
      setState(() {
        _bits = bits;
        _preview = png;
        _w = size.w;
        _h = size.h;
      });
    } catch (e) {
      setState(() => _wallpaperNote = 'Could not use that picture: $e');
    } finally {
      if (mounted) setState(() => _preparing = false);
    }
  }

  Future<void> _send() async {
    setState(() {
      _sending = true;
      _wallpaperNote = null;
    });
    try {
      await _rt.sendWallpaper(_bits!, _w, _h);
      setState(() => _wallpaperNote = 'Sent. The badge shows it when it goes idle.');
    } catch (e) {
      setState(() => _wallpaperNote = 'Not sent: $e');
    } finally {
      if (mounted) setState(() => _sending = false);
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
          onPressed: _preparing ? null : _pick,
          icon: const Icon(Icons.photo),
          label: Text(_preparing ? 'Preparing...' : 'Pick a photo'),
        ),
        const SizedBox(width: 8),
        FilledButton(
          onPressed: _bits != null && _rt.deviceTakesWallpaper && !_sending ? _send : null,
          child: Text(_sending ? 'Sending...' : 'Send to badge'),
        ),
      ]),
      if (_preview != null) ...[
        const SizedBox(height: 8),
        // Exactly the bits that would be sent, one pixel per panel pixel.
        AspectRatio(
          aspectRatio: _w / _h,
          child: Image.memory(_preview!, filterQuality: FilterQuality.none, fit: BoxFit.contain),
        ),
        Text(
            '$_w x $_h, black and white, as the badge will show it.'
            '${_w == landscape.w ? ' The red line marks the bottom of what the CrowPanel shows.' : ''}',
            style: small),
      ],
      if (!_rt.deviceTakesWallpaper)
        Text("The badge's firmware can't receive a wallpaper yet, so Send stays off.", style: small),
      if (_wallpaperNote != null) Text(_wallpaperNote!, style: small),
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
