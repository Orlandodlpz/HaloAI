import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/api.dart';
import 'package:haloai_phone/backend/backend_client.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

void main() {
  late List<(String, String, Map<String, Object?>)> calls;

  BackendClient client(http.Response Function(int n, http.Request r) handler) {
    var n = 0;
    return BackendClient(BackendConfig(Uri.parse('https://api.example'), 'k'),
        sleep: (_) async {},
        client: MockClient((req) async {
          calls.add((req.method, req.url.path, req.body.isEmpty ? {} : (jsonDecode(req.body) as Map).cast()));
          return handler(n++, req);
        }));
  }

  http.Response ok(Object body) => http.Response(jsonEncode(body), 200);
  const item = {
    'memory_id': 'm1', 'kind': 'contact', 'label': 'Victor', 'value': 'victor@example.com', //
    'source': 'user', 'uses': 2, 'updated_at': '2026-10-21T10:00:00-05:00',
  };

  setUp(() => calls = []);

  test('list parses items, including recent actions that cannot be edited', () async {
    final c = client((_, _) => ok({
          'items': [
            item,
            {'memory_id': 'act_x', 'kind': 'action', 'label': 'Emailed Victor', 'value': 'v: hi', 'source': 'action'},
          ]
        }));
    final items = await c.memoryList();
    expect(items.map((i) => (i.kind, i.editable)), [('contact', true), ('action', false)]);
    expect(calls.single.$1, 'GET');
  });

  test('add, edit and delete send the contract fields, each with a call_id', () async {
    final c = client((_, r) => r.method == 'DELETE' ? ok({'deleted': true}) : ok(item));
    await c.memoryAdd('contact', 'Victor', 'victor@example.com');
    await c.memoryEdit('m1', value: 'v@example.com');
    await c.memoryDelete('m1');
    expect(calls.map((c) => '${c.$1} ${c.$2}'), ['POST /v1/memory', 'PATCH /v1/memory/m1', 'DELETE /v1/memory/m1']);
    expect(calls[0].$3.keys.toSet(), {'call_id', 'kind', 'label', 'value'});
    expect(calls[1].$3.keys.toSet(), {'call_id', 'value'}, reason: 'an unchanged label is not sent');
    expect(calls[2].$3.keys.toSet(), {'call_id'}, reason: 'DELETE carries a body so a retry is safe');
  });

  test('a retried write reuses its call_id', () async {
    final c = client((n, _) => n == 0
        ? http.Response(jsonEncode({'error': {'code': 'rate_limited', 'message': ''}}), 429, headers: {'retry-after': '1'})
        : ok(item));
    await c.memoryAdd('fact', 'Desk', 'Room 2.104');
    expect(calls, hasLength(2));
    expect(calls[1].$3['call_id'], calls[0].$3['call_id']);
  });

  test('a refused memory surfaces the backend message', () async {
    final c = client((_, _) => http.Response(
        jsonEncode({'error': {'code': 'invalid_request', 'message': 'that memory already exists'}}), 422));
    await expectLater(c.memoryAdd('contact', 'Victor', 'victor@example.com'),
        throwsA(isA<ApiError>().having((e) => e.message, 'message', 'that memory already exists')));
  });

  test('Google status, connect url, and the sample key refusal', () async {
    final c = client((n, r) => switch (r.url.path) {
          '/v1/google/status' => ok({'connected': true, 'email': 'carlos@example.com'}),
          '/v1/google/connect' => ok({'url': 'https://accounts.google.com/o/oauth2/v2/auth?state=s'}),
          _ => ok({'connected': false}),
        });
    final s = await c.googleStatus();
    expect((s.connected, s.email), (true, 'carlos@example.com'));
    expect((await c.googleConnect()).host, 'accounts.google.com');
    await c.googleDisconnect();
    expect(calls[1].$3.keys, ['call_id']);
    expect(calls[2].$2, '/v1/google/disconnect');

    final demo = client((_, _) => http.Response(
        jsonEncode({'error': {'code': 'invalid_request', 'message': 'the demo site cannot connect a Google account'}}),
        422));
    await expectLater(demo.googleConnect(), throwsA(isA<ApiError>().having((e) => e.httpStatus, 'status', 422)));
  });
}
