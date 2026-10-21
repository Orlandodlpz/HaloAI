import 'dart:convert';
import 'dart:typed_data';

/// JSON messages on the Events, Screen and Settings characteristics.
/// Field names follow phone/docs/ble_fields_draft.md, which is a DRAFT until
/// Victor agrees; change them here and in the firmware together.

/// Service and characteristic UUIDs (draft).
class HaloUuids {
  static const service = '78450000-6bb6-46e1-be3c-499c2950b6ac';
  static const audio = '78450001-6bb6-46e1-be3c-499c2950b6ac';
  static const audioAck = '78450002-6bb6-46e1-be3c-499c2950b6ac';
  static const events = '78450003-6bb6-46e1-be3c-499c2950b6ac';
  static const screen = '78450004-6bb6-46e1-be3c-499c2950b6ac';
  static const wallpaper = '78450005-6bb6-46e1-be3c-499c2950b6ac';
  static const settings = '78450006-6bb6-46e1-be3c-499c2950b6ac';
}

/// One event from the device. [type] is one of `hello`, `wake`,
/// `record_start`, `record_end`, `approve`, `select`, `continue`, `cancel`,
/// `dismiss`, `ack`, `status_query`, `activity`, `battery` (added
/// 2026-10-13 with `reason` on `cancel`, BRIEFING section 2).
class DeviceEvent {
  DeviceEvent(this.type, this.fields);
  final String type;
  final Map<String, Object?> fields;

  static const types = {
    'hello', 'wake', 'record_start', 'record_end', 'approve', 'select', 'continue', //
    'cancel', 'dismiss', 'ack', 'status_query', 'activity', 'battery',
  };

  static DeviceEvent parse(String json) {
    final m = jsonDecode(json);
    if (m is! Map<String, Object?> || m['type'] is! String) throw FormatException('event without type: $json');
    final type = m['type'] as String;
    if (!types.contains(type)) throw FormatException('unknown event type: $type');
    return DeviceEvent(type, m);
  }

  int? get sid => fields['sid'] as int?;
  int? get req => fields['req'] as int?;
  int? get seg => fields['seg'] as int?;
  int? get index => fields['index'] as int?;
  int? get boot => fields['boot'] as int?;
  String? get pendingKey => fields['pending_key'] as String?;

  /// Battery percent, on `hello` and `battery` events.
  int? get battery => fields['battery'] as int?;

  /// Why the device cancelled: `user`, `expired`, `transport` or `battery`.
  String? get reason => fields['reason'] as String?;

  @override
  String toString() => jsonEncode(fields);
}

/// One Screen write.
class ScreenMessage {
  ScreenMessage({
    required this.sid,
    required String this.screen,
    this.rev,
    this.screenLines = const [],
    this.statusLine,
    this.candidates = const [],
    this.expiresInS,
    this.pendingKey,
  });

  /// Only `sid` and `status_line`: the device redraws its status bar with a
  /// partial refresh and leaves the body alone (BRIEFING section 2, Screen).
  ScreenMessage.statusOnly({required this.sid, required String this.statusLine})
      : screen = null,
        rev = null,
        screenLines = const [],
        candidates = const [],
        expiresInS = null,
        pendingKey = null;

  final int sid;
  final String? screen; // V1 state name, e.g. REVIEW, CONFLICT_AMBIGUOUS
  final int? rev;
  final List<String> screenLines;
  final String? statusLine;
  final List<String> candidates;
  final int? expiresInS;

  /// The execution key, on REVIEW (saved to flash as the device approves) and
  /// on EXECUTION_UNCERTAIN (BRIEFING section 2, added 2026-10-19).
  final String? pendingKey;

  Map<String, Object?> toJson() => screen == null
      ? {'sid': sid, 'status_line': statusLine}
      : {
          'sid': sid,
          'screen': screen,
          'rev': ?rev,
          'screen_lines': screenLines,
          'status_line': ?statusLine,
          'candidates': candidates,
          'expires_in_s': ?expiresInS,
          'pending_key': ?pendingKey,
        };
}

/// Settings write. Absent fields keep their previous value on the device.
Map<String, Object?> settingsJson({bool? phoneReady, String? orientation, DateTime? time, String? tz}) => {
      'phone_ready': ?phoneReady,
      'orientation': ?orientation,
      'time': ?time?.toIso8601String(),
      'tz': ?tz,
    };

/// Splits a JSON message into writes of at most [maxWrite] bytes, each
/// with the 2 byte prefix: message ID, then (bit 7 last, bits 0 to 6 index).
List<Uint8List> fragment(Map<String, Object?> message, int messageId, int maxWrite) {
  final body = utf8.encode(jsonEncode(message));
  final per = maxWrite - 2;
  final count = body.isEmpty ? 1 : (body.length + per - 1) ~/ per;
  if (count > 128) throw ArgumentError('message too large: ${body.length} bytes');
  return [
    for (var i = 0; i < count; i++)
      Uint8List.fromList([
        messageId & 0xFF,
        (i == count - 1 ? 0x80 : 0) | i,
        ...body.sublist(i * per, ((i + 1) * per).clamp(0, body.length)),
      ]),
  ];
}

/// Joins prefixed fragments back into JSON strings. A fragment out of order
/// or from a new message ID drops the partial message (it was lost).
class Defragmenter {
  int? _id;
  final _buf = BytesBuilder();
  int _next = 0;

  /// Returns the complete JSON string when the last fragment arrives.
  String? add(Uint8List write) {
    if (write.length < 2) throw FormatException('fragment shorter than its prefix');
    final id = write[0], index = write[1] & 0x7F, last = write[1] & 0x80 != 0;
    if (index == 0) {
      _id = id;
      _buf.clear();
      _next = 0;
    }
    if (id != _id || index != _next) {
      _id = null;
      _buf.clear();
      return null;
    }
    _buf.add(Uint8List.sublistView(write, 2));
    _next++;
    if (!last) return null;
    _id = null;
    return utf8.decode(_buf.takeBytes());
  }
}
