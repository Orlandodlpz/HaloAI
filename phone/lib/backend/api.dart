/// Shapes of the phone to cloud API (BRIEFING section 2, "Phone to cloud API").
library;

/// One candidate on a CONFLICT-AMBIGUOUS result. Only [label] goes to the
/// device; [candidateId] stays on the phone and goes back in `select`.
class Candidate {
  const Candidate(this.candidateId, this.label);
  final String candidateId, label;
}

/// The one result shape every call returns (the `result` event, and the JSON
/// of every other call, including the status endpoints).
class BackendResult {
  BackendResult(this.json)
      : state = json['state'] as String,
        requestId = json['request_id'] as String,
        revision = json['revision'] as int;

  final Map<String, Object?> json;
  final String state, requestId;
  final int revision;

  List<String>? get screenLines => (json['screen_lines'] as List?)?.cast<String>();
  List<Candidate>? get candidates => (json['candidates'] as List?)
      ?.cast<Map>()
      .map((c) => Candidate(c['candidate_id'] as String, c['label'] as String))
      .toList();
  String? get proposalHash => json['proposal_hash'] as String?;

  /// Present on REVIEW (the key approve will use) and after approve.
  String? get executionKey => json['execution_key'] as String?;

  /// On `failure`: `processing` (never approved) or `execution` (approved and
  /// confirmed not done). Used by the phone only, never sent over BLE.
  String? get failureStage => json['failure_stage'] as String?;
  int? get expiresInS => json['expires_in_s'] as int?;
  Map<String, Object?>? get draft => (json['draft'] as Map?)?.cast<String, Object?>();
  Map<String, Object?> get timings => (json['timings'] as Map?)?.cast<String, Object?>() ?? const {};

  bool get isFinalExecution => const {'success', 'failure', 'execution_uncertain'}.contains(state);

  @override
  String toString() => 'BackendResult($state, $requestId, rev $revision)';
}

/// An error answer: `{"error": {"code", "message", "current"}}`. [message] is
/// for logs only, never for the screen.
class ApiError implements Exception {
  ApiError(this.httpStatus, this.code, this.message, {this.current, this.retryAfter});

  factory ApiError.fromJson(int httpStatus, Map<String, Object?> body, {Duration? retryAfter}) {
    final e = (body['error'] as Map?)?.cast<String, Object?>() ?? const {};
    final current = e['current'];
    return ApiError(httpStatus, e['code'] as String? ?? 'http_$httpStatus', e['message'] as String? ?? '',
        current: current is Map ? BackendResult(current.cast<String, Object?>()) : null, retryAfter: retryAfter);
  }

  final int httpStatus;
  final String code, message;

  /// The current result, so the phone can resync without another call.
  final BackendResult? current;
  final Duration? retryAfter;

  @override
  String toString() => 'ApiError($httpStatus $code: $message)';
}

/// One Server-Sent Event from `start`, `additions` or `select`.
sealed class StreamEvent {
  const StreamEvent(this.requestId, this.revision);
  final String requestId;
  final int revision;

  static StreamEvent? fromSse(String event, Map<String, Object?> data) => switch (event) {
        'accepted' => Accepted(data['request_id'] as String, data['revision'] as int, data['call_id'] as String?),
        'status' => Status(data['request_id'] as String, data['revision'] as int, data['status_line'] as String),
        'slow' => Slow(data['request_id'] as String, data['revision'] as int),
        'result' => ResultEvent(BackendResult(data)),
        _ => null,
      };
}

class Accepted extends StreamEvent {
  const Accepted(super.requestId, super.revision, this.callId);
  final String? callId;
}

class Status extends StreamEvent {
  const Status(super.requestId, super.revision, this.statusLine);
  final String statusLine;
}

class Slow extends StreamEvent {
  const Slow(super.requestId, super.revision);
}

class ResultEvent extends StreamEvent {
  ResultEvent(this.result) : super(result.requestId, result.revision);
  final BackendResult result;
}
