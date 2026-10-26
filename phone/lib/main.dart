import 'dart:async';

import 'package:flutter/material.dart';

import 'halo_runtime.dart';
import 'ui/debug_tab.dart';
import 'ui/home_tab.dart';
import 'ui/memory_tab.dart';
import 'ui/settings_tab.dart';

/// HaloAI phone app. Speech to text runs fully on the phone; only text goes
/// to the backend, never audio.
///
/// Four tabs (P4), all views over [HaloRuntime], which owns the speech model,
/// the BLE session and the request controller for the whole process (P3):
/// - Home: the badge link, the current screen as the device gets it, the full
///   draft, typed commands and the phone mic hold (APIRESULT, MICRESULT lines).
/// - Memory: view, add, edit and delete what the assistant remembers.
/// - Settings: orientation, wallpaper, Google account.
/// - Debug: the P0 to P3 test bench (clips, loopback, test screen, logs).
///
/// Needs `--dart-define-from-file=config/backend.json` for the backend; every
/// log line also goes to files/logs/ (P3).
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  unawaited(HaloRuntime.instance.init());
  runApp(HaloApp(runtime: HaloRuntime.instance));
}

class HaloApp extends StatelessWidget {
  const HaloApp({super.key, required this.runtime});
  final HaloRuntime runtime;

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'HaloAI',
        theme: ThemeData(colorSchemeSeed: Colors.indigo),
        darkTheme: ThemeData(colorSchemeSeed: Colors.indigo, brightness: Brightness.dark),
        home: AppShell(runtime: runtime),
      );
}

class AppShell extends StatefulWidget {
  const AppShell({super.key, required this.runtime});
  final HaloRuntime runtime;
  @override
  State<AppShell> createState() => _AppShellState();
}

/// Closing or rebuilding this never stops the session; only Disconnect does.
class _AppShellState extends State<AppShell> {
  late final StreamSubscription<void> _sub;
  int _tab = 0;

  HaloRuntime get _rt => widget.runtime;

  @override
  void initState() {
    super.initState();
    _sub = _rt.changes.listen((_) {
      if (mounted) setState(() {});
    });
    unawaited(_rt.refreshServiceStatus());
  }

  @override
  void dispose() {
    _sub.cancel();
    super.dispose();
  }

  static const _titles = ['HaloAI', 'Memory', 'Settings', 'Debug'];

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(title: Text(_titles[_tab])),
        // IndexedStack keeps each tab's state (a half typed command, the
        // memory list) while switching.
        body: IndexedStack(index: _tab, children: [
          HomeTab(rt: _rt),
          MemoryTab(rt: _rt, active: _tab == 1),
          SettingsTab(rt: _rt, active: _tab == 2),
          DebugTab(rt: _rt),
        ]),
        bottomNavigationBar: NavigationBar(
          selectedIndex: _tab,
          onDestinationSelected: (i) => setState(() => _tab = i),
          destinations: const [
            NavigationDestination(icon: Icon(Icons.home_outlined), selectedIcon: Icon(Icons.home), label: 'Home'),
            NavigationDestination(
                icon: Icon(Icons.psychology_outlined), selectedIcon: Icon(Icons.psychology), label: 'Memory'),
            NavigationDestination(
                icon: Icon(Icons.settings_outlined), selectedIcon: Icon(Icons.settings), label: 'Settings'),
            NavigationDestination(icon: Icon(Icons.bug_report_outlined), selectedIcon: Icon(Icons.bug_report), label: 'Debug'),
          ],
        ),
      );
}
