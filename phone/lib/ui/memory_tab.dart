import 'dart:async';

import 'package:flutter/material.dart';

import '../backend/api.dart';
import '../halo_runtime.dart';

/// Memory (P4): everything the assistant remembers, which the user can view,
/// edit and delete (BRIEFING Locked Decisions). Recent actions can only be
/// deleted ("forget it"); the backend refuses edits to them.
class MemoryTab extends StatefulWidget {
  const MemoryTab({super.key, required this.rt, this.active = false});
  final HaloRuntime rt;

  /// True while this tab is on screen: it loads from the backend each time it
  /// is opened, by then the backend client exists (it is created after the
  /// app starts).
  final bool active;
  @override
  State<MemoryTab> createState() => _MemoryTabState();
}

const _sections = {
  'contact': 'Contacts',
  'preference': 'Preferences',
  'fact': 'Facts',
  'routine': 'Routines',
  'action': 'Recent actions',
};

const _hints = {
  'contact': ('Name', 'Email address'),
  'preference': ('What', 'Preference'),
  'fact': ('What', 'Fact'),
  'routine': ('Name (say "do my ... routine")', 'Command'),
};

class _MemoryTabState extends State<MemoryTab> {
  List<MemoryItem>? _items;
  String? _error;
  bool _loading = false;

  HaloRuntime get _rt => widget.rt;

  @override
  void initState() {
    super.initState();
    if (widget.active) unawaited(_load());
  }

  @override
  void didUpdateWidget(covariant MemoryTab old) {
    super.didUpdateWidget(old);
    if (widget.active && !old.active) unawaited(_load());
  }

  Future<void> _load() async {
    final api = _rt.api;
    if (api == null) {
      setState(() => _error = 'Backend off: build with --dart-define-from-file=config/backend.json');
      return;
    }
    setState(() => _loading = true);
    try {
      final items = await api.memoryList();
      _rt.record('P4LOG memory list: ${items.length} items');
      setState(() {
        _items = items;
        _error = null;
      });
    } catch (e) {
      setState(() => _error = _describe(e));
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  /// The backend's own message for a refused edit ("that memory already
  /// exists", "a contact needs an email address"); never raw exceptions.
  static String _describe(Object e) =>
      e is ApiError ? (e.message.isNotEmpty ? e.message : e.code) : "Can't reach HaloAI. Check your connection.";

  Future<void> _run(String what, Future<void> Function() f) async {
    try {
      await f();
      _rt.record('P4LOG memory $what');
      await _load();
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(_describe(e))));
    }
  }

  Future<void> _edit({MemoryItem? item}) async {
    final kind = ValueNotifier(item?.kind ?? 'contact');
    final label = TextEditingController(text: item?.label);
    final value = TextEditingController(text: item?.value);
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(item == null ? 'Remember something' : 'Edit'),
        content: ValueListenableBuilder(
          valueListenable: kind,
          builder: (context, k, _) => Column(mainAxisSize: MainAxisSize.min, children: [
            if (item == null)
              DropdownButtonFormField<String>(
                initialValue: k,
                items: [
                  for (final e in _hints.keys) DropdownMenuItem(value: e, child: Text(_sections[e]!)),
                ],
                onChanged: (v) => kind.value = v!,
              ),
            TextField(controller: label, maxLength: 100, decoration: InputDecoration(labelText: _hints[k]?.$1)),
            TextField(
              controller: value,
              maxLength: k == 'routine' ? 200 : 300,
              maxLines: null,
              keyboardType: k == 'contact' ? TextInputType.emailAddress : TextInputType.text,
              decoration: InputDecoration(labelText: _hints[k]?.$2),
            ),
          ]),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Save')),
        ],
      ),
    );
    if (ok != true) return;
    final api = _rt.api!;
    final l = label.text.trim(), v = value.text.trim();
    if (l.isEmpty || v.isEmpty) return _snack('Both fields are needed.');
    if (kind.value == 'contact' && !RegExp(r'^[^@\s]+@[^@\s]+\.[^@\s]+$').hasMatch(v)) {
      return _snack('A contact needs an email address.');
    }
    if (item != null && l == item.label && v == item.value) return; // nothing changed
    await _run(item == null ? 'add ${kind.value}' : 'edit ${item.kind}', () async {
      if (item == null) {
        await api.memoryAdd(kind.value, l, v);
      } else {
        await api.memoryEdit(item.id, label: l == item.label ? null : l, value: v == item.value ? null : v);
      }
    });
  }

  Future<void> _delete(MemoryItem item) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(item.kind == 'action' ? 'Forget this action?' : 'Delete this memory?'),
        content: Text('${item.label}\n${item.value}'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Keep')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Delete')),
        ],
      ),
    );
    if (ok == true) await _run('delete ${item.kind}', () => _rt.api!.memoryDelete(item.id));
  }

  void _snack(String m) => ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(m)));

  @override
  Widget build(BuildContext context) {
    final items = _items ?? const <MemoryItem>[];
    final t = Theme.of(context).textTheme;
    return Scaffold(
      floatingActionButton: _rt.api == null
          ? null
          : FloatingActionButton(onPressed: () => _edit(), tooltip: 'Add', child: const Icon(Icons.add)),
      body: RefreshIndicator(
        onRefresh: _load,
        child: ListView(padding: const EdgeInsets.only(bottom: 88), children: [
          if (_loading && _items == null) const LinearProgressIndicator(),
          if (_error != null) Padding(padding: const EdgeInsets.all(16), child: Text(_error!)),
          if (_items != null && items.isEmpty)
            const Padding(padding: EdgeInsets.all(16), child: Text('Nothing remembered yet.')),
          for (final section in _sections.entries)
            if (items.any((i) => i.kind == section.key)) ...[
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
                child: Text(section.value, style: t.titleSmall),
              ),
              for (final i in items.where((i) => i.kind == section.key))
                ListTile(
                  title: Text(i.label),
                  subtitle: Text(i.value, maxLines: 3, overflow: TextOverflow.ellipsis),
                  onTap: i.editable ? () => _edit(item: i) : null,
                  trailing: IconButton(
                      icon: const Icon(Icons.delete_outline), tooltip: 'Delete', onPressed: () => _delete(i)),
                ),
            ],
        ]),
      ),
    );
  }
}
