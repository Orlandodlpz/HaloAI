import 'dart:math';

/// What the phone drew on one Screen write, so a device event that echoes
/// the `sid` binds to exactly that revision, proposal hash and candidate list.
class ScreenContext {
  ScreenContext(this.sid, this.requestId, this.revision, {this.proposalHash, this.candidateIds = const []});
  final int sid;
  final String? requestId;
  final int? revision;
  final String? proposalHash;
  final List<String> candidateIds;
}

/// Maps the device's small local counters to the API's UUIDs, and screen IDs
/// to what was drawn.
///
/// The device's req (u16) and seg (u8) counters start over when it powers
/// up, so maps are scoped to the `boot` value from `hello`: a reconnect with
/// the same boot keeps them, a new boot clears them. No hashes cross BLE.
class IdMap {
  IdMap({Random? random}) : _random = random ?? Random.secure();

  /// Screen contexts kept for late events (a press on an older screen).
  static const keepScreens = 32;

  final Random _random;
  int? _boot;
  final _requests = <int, String>{};
  final _segments = <(int, int), String>{};
  final _screens = <int, ScreenContext>{};
  int _nextSid = 1;

  int? get boot => _boot;

  /// Call on every `hello`. Returns true if the device restarted since the
  /// last hello (or this is the first), meaning old mappings were dropped.
  bool onHello(int boot) {
    if (boot == _boot) return false;
    _boot = boot;
    _requests.clear();
    _segments.clear();
    _screens.clear();
    return true;
  }

  String requestId(int req) => _requests.putIfAbsent(req, _uuid);

  String segmentId(int req, int seg) => _segments.putIfAbsent((req, seg), _uuid);

  /// Registers a screen about to be written and returns its new sid (u16, wraps, never 0).
  ScreenContext newScreen({String? requestId, int? revision, String? proposalHash, List<String> candidateIds = const []}) {
    final sid = _nextSid;
    _nextSid = _nextSid == 0xFFFF ? 1 : _nextSid + 1;
    final s = ScreenContext(sid, requestId, revision, proposalHash: proposalHash, candidateIds: candidateIds);
    _screens[sid] = s;
    if (_screens.length > keepScreens) _screens.remove(_screens.keys.first);
    return s;
  }

  ScreenContext? screen(int sid) => _screens[sid];

  /// The candidate_id behind a `select` index, or null if the sid or index is unknown.
  String? candidateId(int sid, int index) {
    final ids = _screens[sid]?.candidateIds;
    return ids != null && index >= 0 && index < ids.length ? ids[index] : null;
  }

  /// The audio header carries only the low byte of the revision the device
  /// was displaying. Returns the largest revision <= [latest] with that low
  /// byte, or null if there is none.
  static int? resolveRevision(int lowByte, int latest) {
    var r = (latest & ~0xFF) | lowByte;
    if (r > latest) r -= 256;
    return r < 0 ? null : r;
  }

  String _uuid() {
    final b = List<int>.generate(16, (_) => _random.nextInt(256));
    b[6] = (b[6] & 0x0F) | 0x40; // version 4
    b[8] = (b[8] & 0x3F) | 0x80; // RFC 4122 variant
    final h = b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();
    return '${h.substring(0, 8)}-${h.substring(8, 12)}-${h.substring(12, 16)}-${h.substring(16, 20)}-${h.substring(20)}';
  }
}
