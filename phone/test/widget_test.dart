import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/main.dart';

void main() {
  testWidgets('shows the Run button', (tester) async {
    await tester.pumpWidget(const MaterialApp(home: BenchPage()));
    expect(find.text('Run'), findsOneWidget);
  });
}
