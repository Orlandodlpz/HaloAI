import 'dart:convert';

/// Server-Sent Events parser for the backend's streams.
///
/// The server writes `event: <name>`, `data: <json>` and a blank line per
/// event, with `\n` line endings, and `: ping` comments every 10 s
/// (pipeline/app/streams.py). Lines arrive in arbitrary byte chunks, so
/// [add] buffers until a full line is there. A `\r\n` ending is tolerated.
class SseParser {
  final _buffer = StringBuffer();
  String _event = 'message';
  final _data = <String>[];

  /// Feeds decoded text; returns every event it completed as (name, data).
  List<(String, String)> add(String chunk) {
    _buffer.write(chunk);
    final text = _buffer.toString();
    final lines = text.split('\n');
    _buffer
      ..clear()
      ..write(lines.removeLast()); // incomplete tail
    final out = <(String, String)>[];
    for (var line in lines) {
      if (line.endsWith('\r')) line = line.substring(0, line.length - 1);
      if (line.isEmpty) {
        if (_data.isNotEmpty) out.add((_event, _data.join('\n')));
        _event = 'message';
        _data.clear();
      } else if (line.startsWith(':')) {
        // keepalive comment
      } else if (line.startsWith('event:')) {
        _event = line.substring(6).trim();
      } else if (line.startsWith('data:')) {
        final v = line.substring(5);
        _data.add(v.startsWith(' ') ? v.substring(1) : v);
      }
    }
    return out;
  }

  /// Parses an event's data as a JSON object.
  static Map<String, Object?> json(String data) => (jsonDecode(data) as Map).cast<String, Object?>();
}
