import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/draft_view.dart';

void main() {
  test('email: recipient with address, subject, and the whole body', () {
    final lines = draftLines({
      'kind': 'send_email', 'to': 'victor@example.com', 'to_name': 'Victor', //
      'subject': 'Meeting moved', 'body': 'Hi Victor,\nThe meeting moved to Thursday.\nCarlos',
    });
    expect(lines, [
      'To: Victor <victor@example.com>', 'Subject: Meeting moved', '', //
      'Hi Victor,', 'The meeting moved to Thursday.', 'Carlos',
    ]);
  });

  test('email still being resolved has no recipient yet', () {
    expect(draftLines({'kind': 'send_email', 'subject': 's', 'body': 'b'}).first, 'To: (who?)');
  });

  test('create event: title, time, guests, video call, description', () {
    final start = DateTime(2026, 10, 23, 15), end = DateTime(2026, 10, 23, 16);
    final lines = draftLines({
      'kind': 'create_event', 'title': 'Gym', 'start': start.toIso8601String(), 'end': end.toIso8601String(),
      'timezone': 'America/Chicago', 'guests': [{'name': 'Victor', 'email': 'v@example.com'}],
      'video_call': true, 'description': 'Leg day',
    });
    expect(lines, ['Gym', 'Fri Oct 23, 3:00 PM to 4:00 PM', 'Guests: Victor <v@example.com>', 'With a video call', '', 'Leg day']);
  });

  test('update and delete', () {
    final a = DateTime(2026, 10, 23, 9).toIso8601String(), b = DateTime(2026, 10, 23, 10).toIso8601String();
    expect(draftLines({'kind': 'update_event', 'before': {'title': 'Standup', 'start': a, 'end': b}, 'after': {'start': b}})[1],
        startsWith('After: Standup, Fri Oct 23, 10:00 AM'));
    expect(draftLines({'kind': 'delete_event', 'event': {'title': 'Standup', 'start': a, 'end': b}}).first, 'Delete: Standup');
  });

  test('null and unknown kinds never throw', () {
    expect(draftLines(null), isEmpty);
    expect(draftLines({'kind': 'new_tool', 'x': 1}).join('\n'), contains('"kind": "new_tool"'));
  });
}
