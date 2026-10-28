/// Which HaloAI badge this phone connects to (2026-10-28).
///
/// Every badge advertises the same HaloAI service, so taking the first one a
/// scan hears can connect the phone to someone else's badge, whose holds then
/// run on this phone's key and account (seen in the P1 run: the phone took a
/// teammate's badge after a Disconnect and Connect). So the phone remembers
/// the badge it first connected to and ignores every other one; with nothing
/// remembered and more than one badge in range, the user picks.
library;

/// A HaloAI badge heard in a scan.
class FoundBadge {
  const FoundBadge(this.id, this.rssi);

  /// Bluetooth address on Android (`remoteId`), stable for a given badge.
  final String id;

  /// Signal strength in dBm: the badge closest to the phone is usually the
  /// strongest.
  final int rssi;

  @override
  String toString() => '$id ($rssi dBm)';
}

/// What a scan should do next.
sealed class BadgeDecision {
  const BadgeDecision();
}

/// Keep scanning: nothing acceptable heard yet.
class KeepScanning extends BadgeDecision {
  const KeepScanning();
}

/// Connect to this badge.
class ConnectTo extends BadgeDecision {
  const ConnectTo(this.id);
  final String id;
}

/// Several badges and none remembered: ask which one is this phone's,
/// strongest signal first.
class AskUser extends BadgeDecision {
  const AskUser(this.badges);
  final List<FoundBadge> badges;
}

/// How long a scan keeps listening after the first badge, so a second one in
/// range is heard before the phone settles on the first.
const badgeListenWindow = Duration(seconds: 3);

/// Decides from what a scan has heard so far. [mine] is the remembered
/// badge, if any; [windowOver] is true once [badgeListenWindow] has passed
/// since the first badge was heard (or the scan has ended).
BadgeDecision decideBadge(String? mine, List<FoundBadge> seen, {required bool windowOver}) {
  if (mine != null) {
    // Only ever this phone's own badge, however close another one is.
    return seen.any((b) => b.id == mine) ? ConnectTo(mine) : const KeepScanning();
  }
  if (seen.isEmpty || !windowOver) return const KeepScanning();
  if (seen.length == 1) return ConnectTo(seen.single.id);
  return AskUser([...seen]..sort((a, b) => b.rssi.compareTo(a.rssi)));
}
