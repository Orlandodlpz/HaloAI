import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/halo_runtime.dart';
import 'package:haloai_phone/main.dart';

void main() {
  testWidgets('four tabs, and the bench lives under Debug', (tester) async {
    await tester.pumpWidget(HaloApp(runtime: HaloRuntime()));
    for (final tab in ['Home', 'Memory', 'Settings', 'Debug']) {
      expect(find.text(tab), findsOneWidget);
    }
    expect(find.text('Hold to talk (phone mic)'), findsOneWidget);
    expect(find.text('Connect'), findsOneWidget);
    await tester.tap(find.text('Debug'));
    await tester.pumpAndSettle();
    expect(find.text('Run clips'), findsOneWidget);
    await tester.tap(find.text('Settings'));
    await tester.pumpAndSettle();
    expect(find.text('Landscape'), findsOneWidget);
    expect(find.text('Pick a photo'), findsOneWidget);
    expect(find.text('Send to badge'), findsOneWidget);
  });
}
