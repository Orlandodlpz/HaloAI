// Against the live backend. Skipped unless the key file is passed in:
//   flutter test test/backend/live_test.dart --dart-define-from-file=config/backend.json
// Use a sample data (mock scope) key: approve here would act for real on a
// real key. These tests never approve.
import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/api.dart';
import 'package:haloai_phone/backend/backend_client.dart';
import 'package:haloai_phone/backend/request_controller.dart';
import 'package:haloai_phone/ble/device_session.dart';
import 'package:haloai_phone/ble/id_map.dart';

void main() {
  final config = BackendConfig.fromEnvironment();
  final skip = config == null ? 'no --dart-define-from-file=config/backend.json' : null;

  test('start streams accepted, then a result, and cancel ends it', () async {
    final api = BackendClient(config!);
    final rid = uuidV4();
    final events = await api
        .start(
          requestId: rid,
          segmentId: uuidV4(),
          text: 'block 3 to 4 tomorrow for the gym',
          sentAt: isoWithOffset(DateTime.now()),
          timezone: 'America/Chicago',
        )
        .toList();
    for (final e in events) {
      // ignore: avoid_print
      print('LIVE ${e.runtimeType} rev ${e.revision}${e is Status ? ' "${e.statusLine}"' : ''}'
          '${e is ResultEvent ? ' ${e.result.state} ${e.result.screenLines} key=${e.result.executionKey}' : ''}');
    }
    expect(events.first, isA<Accepted>());
    final result = (events.last as ResultEvent).result;
    expect(result.requestId, rid);
    expect(result.state, anyOf('review', 'conflict_schedule'));
    if (result.state == 'review') expect(result.executionKey, hasLength(26));

    final c = await api.cancel(rid);
    expect(c.state, 'cancelled');
    final late = api.approve(rid, revision: result.revision, proposalHash: result.proposalHash ?? 'x');
    await expectLater(late, throwsA(isA<ApiError>().having((e) => e.code, 'code', 'expired')));
    api.close();
  }, skip: skip, timeout: const Timeout(Duration(seconds: 90)));

  // The P2 gate flows through the controller, no device attached. Approves,
  // so only ever with a sample data key.
  group('controller, sample data', () {
    late BackendClient api;
    late RequestController c;
    final views = <ScreenView>[];

    setUp(() {
      api = BackendClient(config!);
      c = RequestController(api: api, timezone: 'America/Chicago');
      views.clear();
      c.views.listen((v) {
        views.add(v);
        // ignore: avoid_print
        print('VIEW ${v.screen} rev ${v.revision} ${v.lines} ${v.candidates}'
            '${v.statusLine == null ? '' : ' status="${v.statusLine}"'}${v.pendingKey == null ? '' : ' key'}');
      });
    });
    tearDown(() async {
      await c.dispose();
      api.close();
    });

    test('calendar: review with key, approve, polled to success', () async {
      await c.say('block 3 to 4 tomorrow for the gym');
      if (c.current!.state == 'conflict_schedule') await c.continueAnyway();
      expect(c.current!.state, 'review');
      expect(views.last.pendingKey, hasLength(26));
      await c.approve();
      expect(c.current!.state, 'success');
      expect(views.last.screen, 'SUCCESS');
      expect(views.last.pendingKey, isNull);
    }, skip: skip, timeout: const Timeout(Duration(seconds: 90)));

    test('email: choose a candidate, add to it, then cancel', () async {
      await c.say('email Alex about lunch on friday');
      expect(c.current!.state, 'conflict_ambiguous');
      if (c.current!.candidates!.isNotEmpty) {
        await c.select(0);
        expect(c.current!.state, 'review');
        final rev = c.current!.revision;
        await c.add('actually make it saturday');
        expect(c.current!.revision, greaterThan(rev));
      }
      await c.cancel();
      expect(c.current!.state, 'cancelled');
      expect(views.last.screen, 'CANCELLED');
    }, skip: skip, timeout: const Timeout(Duration(seconds: 120)));
  });
}
