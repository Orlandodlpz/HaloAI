import 'dart:typed_data';

/// The BLE link to the device, as the rest of the app sees it. One
/// implementation talks to the real radio (`fbp_transport.dart`); tests use a
/// fake. Keeping the BLE package behind this interface means swapping it
/// (for example over its license) touches one file.
abstract class DeviceTransport {
  /// Link changes: scanning, connected (with the negotiated MTU), lost, failed.
  Stream<LinkStatus> get status;

  /// Raw Audio notifications.
  Stream<Uint8List> get audio;

  /// Raw Events notifications (prefixed JSON fragments).
  Stream<Uint8List> get events;

  /// Largest single write: negotiated ATT MTU minus 3.
  int get maxWrite;

  Future<void> writeAudioAck(Uint8List json);
  Future<void> writeScreen(Uint8List fragment);
  Future<void> writeSettings(Uint8List fragment);

  /// Wallpaper write without response (P4, proposed format in
  /// docs/ble_fields_draft.md); the device answers on Events.
  Future<void> writeWallpaper(Uint8List fragment);

  /// Starts scanning and keeps the link up (reconnects) until [stop].
  Future<void> start();
  Future<void> stop();
}

enum LinkState { off, scanning, connecting, connected, lost, failed }

class LinkStatus {
  const LinkStatus(this.state, {this.mtu, this.phy, this.detail});
  final LinkState state;
  final int? mtu;
  final String? phy;
  final String? detail;

  @override
  String toString() => [
        state.name,
        if (mtu != null) 'MTU $mtu',
        ?phy,
        ?detail,
      ].join(', ');
}
