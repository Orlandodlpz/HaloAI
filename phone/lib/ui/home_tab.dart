import 'dart:async';

import 'package:flutter/material.dart';

import '../backend/draft_view.dart';
import '../ble/badge_choice.dart';
import '../ble/device_transport.dart';
import '../halo_runtime.dart';
import 'mic_hold.dart';

/// Backend states where Cancel no longer applies: the action was approved or
/// the request is over.
const _afterApprove = {'executing', 'success', 'failure', 'execution_uncertain', 'cancelled', 'read_only_result'};

/// Home: the badge link, the current screen exactly as the device gets it
/// (with phone side buttons that take the same paths as device presses), the
/// full draft the device's text budget cuts (P4), typed commands and the phone
/// mic hold.
class HomeTab extends StatefulWidget {
  const HomeTab({super.key, required this.rt});
  final HaloRuntime rt;
  @override
  State<HomeTab> createState() => _HomeTabState();
}

class _HomeTabState extends State<HomeTab> {
  final _text = TextEditingController();
  HaloRuntime get _rt => widget.rt;

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final t = Theme.of(context).textTheme;
    return ListView(padding: const EdgeInsets.all(16), children: [
      _badgeCard(context),
      if (_rt.badgeChoices != null) ...[
        const SizedBox(height: 12),
        _whichBadgeCard(context, _rt.badgeChoices!),
      ],
      const SizedBox(height: 12),
      MicHoldButton(rt: _rt),
      const SizedBox(height: 4),
      Text(_rt.status, style: t.bodySmall),
      const SizedBox(height: 12),
      _requestCard(context),
      if (draftLines(_rt.ctl?.current?.draft).isNotEmpty) ...[
        const SizedBox(height: 12),
        _draftCard(context),
      ],
    ]);
  }

  Widget _badgeCard(BuildContext context) {
    final t = Theme.of(context).textTheme;
    final connected = _rt.link.state == LinkState.connected;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(children: [
          Icon(connected ? Icons.bluetooth_connected : Icons.bluetooth,
              color: connected ? Theme.of(context).colorScheme.primary : null),
          const SizedBox(width: 12),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(connected ? 'Badge connected' : 'Badge: ${_rt.link.state.name}', style: t.titleMedium),
              Text('${_rt.serviceStatus}', style: t.bodySmall),
              Text(_rt.badgeId == null ? 'No badge remembered yet' : 'Your badge: ${_rt.badgeId}', style: t.bodySmall),
            ]),
          ),
          FilledButton.tonal(
            onPressed: () => unawaited(_rt.device == null ? _rt.connect() : _rt.disconnect()),
            child: Text(_rt.device == null ? 'Connect' : 'Disconnect'),
          ),
        ]),
      ),
    );
  }

  /// Several badges in range and none remembered: the strongest signal is
  /// listed first, since the badge next to the phone is usually it.
  Widget _whichBadgeCard(BuildContext context, List<FoundBadge> badges) {
    final t = Theme.of(context).textTheme;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Text('More than one HaloAI badge is nearby', style: t.titleMedium),
          Text('Which one is yours? The one next to your phone usually has the strongest signal. '
              'Only that badge will connect from now on.', style: t.bodySmall),
          for (final b in badges)
            ListTile(
              contentPadding: EdgeInsets.zero,
              leading: const Icon(Icons.badge_outlined),
              title: Text(b.id),
              subtitle: Text('Signal ${b.rssi} dBm${b == badges.first ? ' (strongest)' : ''}'),
              trailing: FilledButton(onPressed: () => _rt.chooseBadge(b.id), child: const Text('Mine')),
            ),
          Align(
            alignment: Alignment.centerRight,
            child: TextButton(onPressed: () => _rt.chooseBadge(null), child: const Text('None of these')),
          ),
        ]),
      ),
    );
  }

  Widget _requestCard(BuildContext context) {
    final ctl = _rt.ctl;
    final small = Theme.of(context).textTheme.bodySmall;
    final v = _rt.view;
    final state = ctl?.current?.state;
    void run(Future<void> Function() f) => unawaited(f());
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Row(children: [
            Expanded(
              child: TextField(
                controller: _text,
                enabled: ctl != null,
                decoration: const InputDecoration(isDense: true, hintText: 'Type a command'),
              ),
            ),
            TextButton(
                onPressed: ctl == null ? null : () => run(() => ctl.say(_text.text)), child: const Text('Send')),
            TextButton(
                onPressed: ctl?.currentRequestId == null ? null : () => run(() => ctl!.add(_text.text)),
                child: const Text('Add')),
          ]),
          if (v != null) ...[
            const SizedBox(height: 8),
            Text('${v.screen}  rev ${v.revision}${v.expiresInS == null ? '' : '  ${v.expiresInS} s'}',
                style: Theme.of(context).textTheme.labelLarge),
            if (v.statusLine != null) Text(v.statusLine!, style: small),
            // Exactly the lines the device draws, in its fixed width font.
            for (final l in v.lines) Text(l, style: const TextStyle(fontFamily: 'monospace')),
          ],
          Wrap(spacing: 4, children: [
            for (var i = 0; i < (v?.candidates.length ?? 0); i++)
              OutlinedButton(
                  onPressed: state == 'conflict_ambiguous' ? () => run(() => ctl!.select(i)) : null,
                  child: Text(v!.candidates[i], style: small)),
            FilledButton(onPressed: state == 'review' ? () => run(ctl!.approve) : null, child: const Text('Approve')),
            OutlinedButton(
                onPressed: state == 'conflict_schedule' ? () => run(ctl!.continueAnyway) : null,
                child: const Text('Continue')),
            OutlinedButton(
                // Like a double press: cancels only before execution. After SUCCESS
                // the device just acknowledges, so the button is off.
                onPressed: ctl?.currentRequestId == null || _afterApprove.contains(state)
                    ? null
                    : () => run(ctl!.cancel),
                child: const Text('Cancel')),
          ]),
          for (final l in _rt.apiLog.take(2)) Text(l, maxLines: 1, overflow: TextOverflow.ellipsis, style: small),
        ]),
      ),
    );
  }

  /// The whole draft: the device shows at most 48 x 12 characters, so a long
  /// email body or a guest list is read here.
  Widget _draftCard(BuildContext context) => Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            Text('Full draft', style: Theme.of(context).textTheme.titleSmall),
            const SizedBox(height: 4),
            SelectableText(draftLines(_rt.ctl?.current?.draft).join('\n')),
          ]),
        ),
      );
}
