import 'dart:async';
import 'dart:convert';

import 'package:http/http.dart' as http;

import '../ble/id_map.dart';
import 'api.dart';
import 'sse.dart';

/// Where the backend is and the key to use, from
/// `--dart-define-from-file=config/backend.json` (gitignored; names in
/// config/backend.example.json).
class BackendConfig {
  const BackendConfig(this.base, this.apiKey);
  final Uri base;
  final String apiKey;

  static BackendConfig? fromEnvironment() {
    const base = String.fromEnvironment('HALO_API_BASE');
    const key = String.fromEnvironment('HALO_API_KEY');
    if (base.isEmpty || key.isEmpty) return null;
    return BackendConfig(Uri.parse(base), key);
  }

  /// P4: an optional second key used **only** by the Google account screen,
  /// from `--dart-define-from-file=config/backend.google-test.json` (gitignored,
  /// a real scope key on its own test user). Approvals with it would act on
  /// that Google account, so requests and approvals always use [fromEnvironment].
  static BackendConfig? googleFromEnvironment() {
    const base = String.fromEnvironment('HALO_API_BASE');
    const key = String.fromEnvironment('HALO_GOOGLE_KEY');
    if (base.isEmpty || key.isEmpty) return null;
    return BackendConfig(Uri.parse(base), key);
  }
}

/// Phone to cloud API client (BRIEFING section 2).
///
/// Retries never create a second effect: every POST except `touch` carries a
/// `call_id` that is generated once per logical call and reused on every
/// retry, so the server replays a finished call or reattaches to a running
/// stream instead of doing the work again. Retried: network errors, a stream
/// silent for [idleTimeout] (the server pings every 10 s), a stream that ends
/// without a result, and 429 after its `Retry-After`. Anything else is an
/// answer and comes back as [ApiError].
class BackendClient {
  BackendClient(
    this.config, {
    http.Client? client,
    this.idleTimeout = const Duration(seconds: 25),
    this.approveTimeout = const Duration(seconds: 10),
    this.callTimeout = const Duration(seconds: 15),
    this.maxAttempts = 5,
    Future<void> Function(Duration)? sleep,
    String Function()? newId,
  })  : _http = client ?? http.Client(),
        _sleep = sleep ?? Future<void>.delayed,
        _newId = newId ?? uuidV4;

  final BackendConfig config;
  final http.Client _http;
  final Duration idleTimeout, approveTimeout, callTimeout;
  final int maxAttempts;
  final Future<void> Function(Duration) _sleep;
  final String Function() _newId;

  /// Starts a request. [sentAt] is RFC 3339 with its UTC offset
  /// (`isoWithOffset`), [timezone] an IANA name such as America/Chicago.
  Stream<StreamEvent> start({
    required String requestId,
    required String segmentId,
    required String text,
    required String sentAt,
    required String timezone,
    String language = 'en',
    String orientation = 'landscape',
  }) =>
      _stream('/v1/requests', {
        'call_id': _newId(),
        'request_id': requestId,
        'segment_id': segmentId,
        'text': text,
        'sent_at': sentAt,
        'timezone': timezone,
        'language': language,
        'orientation': orientation,
      });

  /// RECORD ADDITION text. [revision] is the base revision the phone saw.
  Stream<StreamEvent> addition(String requestId,
          {required int revision, required String segmentId, required String text, required String sentAt}) =>
      _stream('/v1/requests/$requestId/additions', {
        'call_id': _newId(),
        'revision': revision,
        'segment_id': segmentId,
        'text': text,
        'sent_at': sentAt,
      });

  Stream<StreamEvent> select(String requestId, {required int revision, required String candidateId}) =>
      _stream('/v1/requests/$requestId/select',
          {'call_id': _newId(), 'revision': revision, 'candidate_id': candidateId});

  /// Waits up to 5 s on the server, so its own timeout is longer
  /// ([approveTimeout], 10 s), which must stay below the device's EXECUTING
  /// timeout.
  Future<BackendResult> approve(String requestId, {required int revision, required String proposalHash}) =>
      _json('POST', '/v1/requests/$requestId/approve',
          {'call_id': _newId(), 'revision': revision, 'proposal_hash': proposalHash},
          timeout: approveTimeout);

  Future<BackendResult> continueAnyway(String requestId, {required int revision}) =>
      _json('POST', '/v1/requests/$requestId/continue', {'call_id': _newId(), 'revision': revision});

  Future<BackendResult> cancel(String requestId) =>
      _json('POST', '/v1/requests/$requestId/cancel', {'call_id': _newId()});

  /// Knob activity in a decision state; resets the 60 s expiry. No call_id.
  Future<BackendResult> touch(String requestId, {required int revision}) =>
      _json('POST', '/v1/requests/$requestId/touch', {'revision': revision});

  Future<BackendResult> executionStatus(String executionKey) =>
      _json('GET', '/v1/executions/$executionKey', null);

  Future<BackendResult> requestStatus(String requestId) => _json('GET', '/v1/requests/$requestId', null);

  // ---------------------------------------------- memory (P4, BRIEFING section 2)

  Future<List<MemoryItem>> memoryList() async {
    final m = await _call('GET', '/v1/memory', null);
    return [for (final i in (m['items'] as List? ?? const [])) MemoryItem((i as Map).cast<String, Object?>())];
  }

  /// [kind] is `contact`, `preference`, `fact` or `routine`. Adding an item
  /// that already exists only refreshes it.
  Future<MemoryItem> memoryAdd(String kind, String label, String value) async => MemoryItem(
      await _call('POST', '/v1/memory', {'call_id': _newId(), 'kind': kind, 'label': label, 'value': value}));

  /// Not for `action` items, which can only be deleted (the backend answers 422).
  Future<MemoryItem> memoryEdit(String memoryId, {String? label, String? value}) async => MemoryItem(await _call(
      'PATCH', '/v1/memory/${Uri.encodeComponent(memoryId)}', {'call_id': _newId(), 'label': ?label, 'value': ?value}));

  /// DELETE with a JSON body: the call_id makes a retried delete safe.
  Future<void> memoryDelete(String memoryId) =>
      _call('DELETE', '/v1/memory/${Uri.encodeComponent(memoryId)}', {'call_id': _newId()});

  // --------------------------------------------- Google account (P4, section 2)

  Future<GoogleStatus> googleStatus() async => GoogleStatus(await _call('GET', '/v1/google/status', null));

  /// The Google sign in page to open in a browser. Needs a `real` scope key:
  /// the sample data key gets 422 `invalid_request`. Nothing tells the phone
  /// when sign in finishes, so the caller polls [googleStatus].
  Future<Uri> googleConnect() async =>
      Uri.parse((await _call('POST', '/v1/google/connect', {'call_id': _newId()}))['url'] as String);

  Future<void> googleDisconnect() => _call('POST', '/v1/google/disconnect', {'call_id': _newId()});

  void close() => _http.close();

  // ------------------------------------------------------------------ calls

  Map<String, String> _headers(bool json) => {
        'Authorization': 'Bearer ${config.apiKey}',
        'Accept': 'text/event-stream, application/json',
        if (json) 'Content-Type': 'application/json',
      };

  Uri _url(String path) => config.base.resolve(path);

  Future<BackendResult> _json(String method, String path, Map<String, Object?>? body, {Duration? timeout}) async =>
      BackendResult(await _call(method, path, body, timeout: timeout));

  /// One plain JSON call with retries; the same [body] (and its call_id) on
  /// every attempt.
  Future<Map<String, Object?>> _call(String method, String path, Map<String, Object?>? body,
      {Duration? timeout}) async {
    Object? lastError;
    for (var attempt = 1; attempt <= maxAttempts; attempt++) {
      final abort = Completer<void>();
      final req = http.AbortableRequest(method, _url(path), abortTrigger: abort.future)
        ..headers.addAll(_headers(body != null));
      if (body != null) req.body = jsonEncode(body);
      try {
        // One deadline for headers and body together.
        final res = await Future(() async => http.Response.fromStream(await _http.send(req)))
            .timeout(timeout ?? callTimeout);
        if (res.statusCode == 429) {
          lastError = _error(res.statusCode, res.body, res.headers);
          await _sleep(_retryAfter(res.headers));
          continue;
        }
        if (res.statusCode < 200 || res.statusCode >= 300) throw _error(res.statusCode, res.body, res.headers);
        return (jsonDecode(res.body) as Map).cast<String, Object?>();
      } on TimeoutException catch (e) {
        if (!abort.isCompleted) abort.complete();
        lastError = e;
      } on http.ClientException catch (e) {
        lastError = e;
      }
      await _sleep(_backoff(attempt));
    }
    throw lastError ?? StateError('no attempt made');
  }

  Stream<StreamEvent> _stream(String path, Map<String, Object?> body) async* {
    Object? lastError;
    for (var attempt = 1; attempt <= maxAttempts; attempt++) {
      final abort = Completer<void>();
      final req = http.AbortableRequest('POST', _url(path), abortTrigger: abort.future)
        ..headers.addAll(_headers(true))
        ..body = jsonEncode(body);
      try {
        final res = await _http.send(req).timeout(idleTimeout);
        if (res.statusCode != 200) {
          final text = await res.stream.bytesToString();
          if (res.statusCode == 429) {
            lastError = _error(res.statusCode, text, res.headers);
            await _sleep(_retryAfter(res.headers));
            continue;
          }
          throw _error(res.statusCode, text, res.headers);
        }
        final parser = SseParser();
        await for (final chunk in res.stream.timeout(idleTimeout).transform(utf8.decoder)) {
          for (final (name, data) in parser.add(chunk)) {
            final event = StreamEvent.fromSse(name, SseParser.json(data));
            if (event == null) continue;
            yield event;
            if (event is ResultEvent) {
              if (!abort.isCompleted) abort.complete();
              return;
            }
          }
        }
        lastError = StateError('stream ended without a result');
      } on TimeoutException catch (e) {
        if (!abort.isCompleted) abort.complete();
        lastError = e;
      } on http.ClientException catch (e) {
        lastError = e;
      }
      await _sleep(_backoff(attempt));
    }
    throw lastError ?? StateError('no attempt made');
  }

  ApiError _error(int status, String body, Map<String, String> headers) {
    Map<String, Object?> json;
    try {
      json = (jsonDecode(body) as Map).cast<String, Object?>();
    } catch (_) {
      json = const {};
    }
    return ApiError.fromJson(status, json, retryAfter: status == 429 ? _retryAfter(headers) : null);
  }

  static Duration _retryAfter(Map<String, String> headers) {
    final s = int.tryParse(headers['retry-after'] ?? '');
    return Duration(seconds: s == null || s < 1 ? 1 : s);
  }

  static Duration _backoff(int attempt) => Duration(seconds: attempt >= 4 ? 8 : 1 << (attempt - 1));
}
