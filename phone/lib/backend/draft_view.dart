import 'dart:convert';

/// The full text of a result's `draft` (P4), for the phone: the device only
/// gets `screen_lines`, cut to its text budget (48 x 12 landscape), so a long
/// email body or a guest list is read here instead. Kinds from
/// pipeline/app/tools.py; an unknown kind falls back to its JSON, so a new
/// backend tool never breaks the view.
List<String> draftLines(Map<String, Object?>? draft) {
  if (draft == null) return const [];
  String s(Object? v) => v?.toString() ?? '';
  switch (draft['kind']) {
    case 'send_email':
      final name = s(draft['to_name']), to = s(draft['to']);
      return [
        'To: ${name.isNotEmpty && to.isNotEmpty ? '$name <$to>' : name.isNotEmpty ? name : to.isNotEmpty ? to : '(who?)'}',
        'Subject: ${s(draft['subject'])}',
        '',
        ...s(draft['body']).split('\n'),
      ];
    case 'create_event':
      final guests = [
        for (final g in (draft['guests'] as List? ?? const []))
          if (g is Map) [s(g['name']), if (s(g['email']).isNotEmpty) '<${s(g['email'])}>'].join(' ').trim(),
      ];
      return [
        s(draft['title']),
        when(draft['start'], draft['end']),
        if (guests.isNotEmpty) 'Guests: ${guests.join(', ')}',
        if (draft['video_call'] == true) 'With a video call',
        if (s(draft['description']).isNotEmpty) ...['', ...s(draft['description']).split('\n')],
      ];
    case 'update_event':
      final before = (draft['before'] as Map?) ?? const {};
      final after = (draft['after'] as Map?) ?? const {};
      return [
        'Before: ${s(before['title'])}, ${when(before['start'], before['end'])}',
        'After: ${s(after['title'] ?? before['title'])}, ${when(after['start'], after['end'])}',
      ];
    case 'delete_event':
      final e = (draft['event'] as Map?) ?? const {};
      return ['Delete: ${s(e['title'])}', when(e['start'], e['end'])];
    default:
      return const JsonEncoder.withIndent('  ').convert(draft).split('\n');
  }
}

const _days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
const _months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/// "Thu Oct 23, 3:00 PM to 4:00 PM" in the phone's local time, which is the
/// timezone every request was sent with. Unparseable values are shown as is.
String when(Object? start, Object? end) {
  final a = DateTime.tryParse(start?.toString() ?? '')?.toLocal();
  final b = DateTime.tryParse(end?.toString() ?? '')?.toLocal();
  if (a == null) return [start, end].whereType<Object>().join(' to ');
  String day(DateTime t) => '${_days[t.weekday - 1]} ${_months[t.month - 1]} ${t.day}';
  String clock(DateTime t) {
    final h = t.hour % 12 == 0 ? 12 : t.hour % 12;
    return '$h:${t.minute.toString().padLeft(2, '0')} ${t.hour < 12 ? 'AM' : 'PM'}';
  }

  if (b == null) return '${day(a)}, ${clock(a)}';
  final sameDay = a.year == b.year && a.month == b.month && a.day == b.day;
  return sameDay ? '${day(a)}, ${clock(a)} to ${clock(b)}' : '${day(a)}, ${clock(a)} to ${day(b)}, ${clock(b)}';
}
