# BLE field names: DRAFT for Victor

**Status:** DRAFT 2026-10-13 (Carlos). Not a contract yet. After Victor agrees (or changes it), the agreed version is appended to BRIEFING section 2 and this file is marked as moved.

**Update 2026-10-14:** agreed by Carlos and Victor on 2026-10-13 and adopted in full in BRIEFING section 2, with two additions from Victor (see the note under the Events table). BRIEFING section 2 names this file as the detailed reference, so it stays here.

Everything here fills in what section 2 leaves open: UUIDs, the exact JSON for every message, and a few rules the byte layout needs. It does not change anything section 2 already decided.

## Service and characteristics

128 bit UUIDs, random base, generated once. Only the 4th and 5th hex digits change.

| Characteristic | UUID | Properties |
| --- | --- | --- |
| Service | `78450000-6bb6-46e1-be3c-499c2950b6ac` | |
| Audio | `78450001-6bb6-46e1-be3c-499c2950b6ac` | notify |
| Audio Ack | `78450002-6bb6-46e1-be3c-499c2950b6ac` | write without response |
| Events | `78450003-6bb6-46e1-be3c-499c2950b6ac` | notify |
| Screen | `78450004-6bb6-46e1-be3c-499c2950b6ac` | write (with response) |
| Wallpaper | `78450005-6bb6-46e1-be3c-499c2950b6ac` | write without response (fields in P4) |
| Settings | `78450006-6bb6-46e1-be3c-499c2950b6ac` | write (with response) |

The device advertises the service UUID, so the phone scans for it and never connects to anything else.

## Audio rules the byte layout needs

- **Codec ID** (header byte 0): `1` = IMA ADPCM, 16 kHz, mono. Anything else is rejected.
- **Nibble order:** low nibble first, then high nibble (standard IMA). Each payload byte is 2 samples.
- **Sequence numbers** start at 0 for every segment. The final marker's `seq` equals the number of audio chunks.
- **Last chunk** may be shorter than 160 bytes (payload length says how long). It is padded to an even sample count; total samples in the final marker says where real audio ends.
- **Final marker resend:** if the final ack (`"final": true`) has not arrived 300 ms after sending the final marker, the device sends the final marker again, until the delivery deadline (then FAILURE). The phone acknowledges a repeated final marker again; it never reports the segment twice.
- **rev byte:** the low 8 bits of the revision the device was displaying (the `rev` of the last Screen). The phone maps it back to the full revision.

## Audio Ack (phone to device)

```
{"req": 12, "seg": 0, "upto": 57}
{"req": 12, "seg": 0, "upto": 57, "resend_from": 58}
{"req": 12, "seg": 0, "upto": 140, "final": true}
```

- `upto`: highest sequence number stored with nothing missing before it; `-1` if chunk 0 has not arrived.
- `resend_from`: resend every chunk from this sequence number (go back N). Duplicates are harmless.
- `final`: every chunk and the final marker are stored. The device may free the segment and leave FINALIZE AUDIO.
- Sent every 10 chunks or every 200 ms, whichever comes first; a new gap is reported at once.

## Events (device to phone)

Every event has `"type"`. IDs in events are the same device local counters as in the audio header.

| type | Fields | When |
| --- | --- | --- |
| `hello` | `fw` (string), `boot` (u32, random per power up), `state` (V1 state name), `req` (current req ID), `sid` (last screen drawn, or null), `pending_key` (26 char execution key, or null), `battery` (percent) | Every connect, as soon as the phone turns on Events notifications (sent earlier, it is lost: nobody is subscribed yet). The phone subscribes to Audio first and Events last, so it is ready to answer |
| `wake` | none | WALLPAPER woken (input consumed) |
| `record_start` | `req`, `seg`, `rev`, `addition` (bool) | Hold begins (RECORDING or RECORD ADDITION) |
| `record_end` | `req`, `seg`, `chunks`, `ms` (hold length) | Release; informational, the final marker decides |
| `approve` | `sid` | Single press in REVIEW after the 1 s guard |
| `select` | `sid`, `index` (0 based, position on that screen) | Single press in CONFLICT-AMBIGUOUS |
| `continue` | `sid` | Single press in CONFLICT-SCHEDULE |
| `cancel` | `req`, `sid` (or null) | Double press in a pre execution state |
| `dismiss` | `sid`, `pending_key` | Double press in EXECUTION UNCERTAIN (stays saved) |
| `ack` | `sid` | Any input on PHONE DISCONNECTED, SUCCESS, FAILURE; single press or timeout on READ-ONLY RESULT |
| `status_query` | `pending_key` | Single press in EXECUTION UNCERTAIN, or after BOOT / RECOVERY |
| `activity` | `sid` | Knob rotation in a decision state, at most one every 10 s |

**Added 2026-10-13 (Victor, adopted in BRIEFING section 2):**

| type | Fields | When |
| --- | --- | --- |
| `cancel` | adds `reason`: `user`, `expired`, `transport` or `battery` | Double press (`user`), the 60 s expiry (`expired`), link loss or buffer exhaustion (`transport`), entering CRITICAL BATTERY (`battery`) |
| `battery` | `battery` (percent) | Rate limited, on threshold crossings, so CRITICAL BATTERY (10% or less) does not look like a device that went quiet. Field name proposed by Carlos 2026-10-14 to match `hello`; Victor to confirm |

`boot` lets the phone tell a reconnect (same `boot`) from a device restart (new `boot`), because the req and seg counters start over after a restart.

## Screen (phone to device)

```
{"sid": 42, "screen": "REVIEW", "rev": 3,
 "screen_lines": ["To: victor@example.com", "Subject: Meeting moved", "..."],
 "status_line": "Ready to confirm",
 "candidates": [], "expires_in_s": 60}
```

- `sid`: new for every write (u16, wraps). Events echo it.
- `screen`: V1 state name in upper case with underscores: `PROCESSING`, `PROCESSING_SLOW`, `REVIEW`, `CONFLICT_AMBIGUOUS`, `CONFLICT_SCHEDULE`, `READ_ONLY_RESULT`, `EXECUTING`, `EXECUTION_UNCERTAIN`, `SUCCESS`, `FAILURE`, `CANCELLED`, `READY`.
- `rev`: full revision (the device puts its low byte in the next audio header).
- `screen_lines`, `status_line`: as the backend sent them. `status_line` alone (same `sid` rules) updates the partial refresh status bar.
- `candidates`: display strings in order; `select` sends the index.
- `expires_in_s`: decision states only, for an on screen countdown if wanted.
- `pending_key` (added 2026-10-19, BRIEFING section 2): optional, the 26 character execution key from the backend's `review` result, sent on REVIEW and on EXECUTION_UNCERTAIN after a reconnect. The device saves it to flash as it sends approve. `failure_stage` is not sent over BLE; the device reads it from its own state.

## Settings (phone to device)

```
{"phone_ready": true, "orientation": "landscape",
 "time": "2026-10-13T14:05:00-05:00", "tz": "America/Chicago"}
```

- **`phone_ready` is new, and needed:** PHONE / BLUETOOTH CHECK needs "phone linked and capture service ready", and nothing in section 2 carries the second half. The phone sends Settings right after `hello`, and again whenever readiness changes (for example the speech model is still loading, or crashed). The device goes to RECORDING only if the link is up and the last `phone_ready` was true.
- Fields not present keep their previous value.

## JSON bigger than one packet

Section 2 says large JSON is split with a sequence number. Proposal: every JSON write on Events, Screen and Settings starts with a 2 byte prefix, even when it fits in one packet, so there is one code path:

| Byte | Meaning |
| --- | --- |
| 0 | message ID (u8, wraps) |
| 1 | bit 7 = last fragment, bits 0 to 6 = fragment index |

The receiver joins fragments with the same message ID in order and parses the JSON once the last one arrives. Audio Ack stays a single unprefixed JSON write (always small).

## Checking the firmware encoder

`phone/tools/audio_reference/` has a Python IMA ADPCM encoder and chunk builder that match the phone byte for byte, and `phone/test/golden/adpcm_chunks.json` has a 0.5 s test signal (`input_pcm`) with the exact packets the phone expects (`chunks_hex`, req 513, rev 2, seg 3). If the firmware encodes `input_pcm` with those IDs and gets the same bytes, the phone will decode it. Regenerate with `python3 tools/audio_reference/make_golden.py` from `phone/`.

With NimBLE on Arduino, the preferred MTU is set with `NimBLEDevice::setMTU(256)`; the phone asks for 517 and the smaller wins.

## Questions for Victor

1. OK with the UUIDs and properties above?
2. OK with `boot` in `hello`, with `hello` sent when Events notifications are turned on, and with `phone_ready` in Settings?
3. OK with the final marker resend after 300 ms?
4. The 2 byte prefix for JSON fragments, or would you rather put the sequence number inside the JSON?
5. Anything the firmware needs that is missing (for example a battery event outside `hello`)?

## Wallpaper (phone to device): PROPOSAL for Victor, 2026-10-21 (P4)

Nothing here is agreed yet. It fills in the one line the contract has (BRIEFING section 2: "JSON chunks with offset and checksum, image data base64", the image already resized and dithered on the phone), so it is a field name agreement in the same spirit as the rest of this file, not a contract change. The phone side is built and tested against this format (`phone/lib/wallpaper/`), with Send switched off until the firmware says it can receive one. If you want something different, say so: on the phone only the chunk encoder and the answer handling change.

**The image.** Exactly the panel's framebuffer, nothing to convert on the device: 1 bit per pixel, rows top to bottom, most significant bit leftmost, **1 = white** (the convention both panels' framebuffers already use, CLAUDE.md Device). 800 x 480 in landscape, 480 x 800 in portrait, following the orientation setting at the moment it was made; 48,000 bytes either way. The phone centre crops the photo to the panel's aspect, resizes and dithers it (Floyd–Steinberg). On the CrowPanel only the top 272 rows of the 800 x 480 canvas are visible; the phone's preview marks that line.

**Each write** (Wallpaper characteristic, write without response) is one JSON message with the usual 2 byte fragment prefix (message ID, then bit 7 last and the index), sized so a chunk always fits one write:

```json
{"wp": 7, "w": 800, "h": 480, "off": 0, "data": "<base64 of the next bytes>"}
```

| Field | Meaning |
| --- | --- |
| `wp` | Transfer ID, 0 to 255, new for each image. A write with a new `wp` abandons any transfer in progress. |
| `w`, `h` | Image size in pixels; the device rejects sizes it cannot show. |
| `off` | Byte offset of `data` in the 48,000 byte image. Chunks arrive in order, each starting where the last ended. |
| `data` | Base64 of the raw bytes from `off`. |
| `crc` | Last chunk only: CRC-16/CCITT-FALSE (the same function as the audio chunks: poly 0x1021, init 0xFFFF, no reflection) over all 48,000 bytes. Its presence marks the end. |

At MTU 256 (253 byte writes) that is **141 image bytes per write, 341 writes**, about 86 KB on air. Transfer time is unmeasured; it only happens when the user picks a new wallpaper.

**The answer** goes on Events, because writes without response get no acknowledgment of their own:

```json
{"type": "wallpaper", "wp": 7, "ok": true}
{"type": "wallpaper", "wp": 7, "ok": false, "resend_from": 28200}
```

- `ok: true` once the CRC matches and the image is stored. The phone shows "Sent".
- `ok: false` with `resend_from` when a chunk was missing (an `off` that does not continue from the last one) or the CRC failed: the phone resends from that byte offset to the end, whole chunks, same `wp`, up to 3 attempts in total. `resend_from: 0` restarts it. The same idea as the audio `resend_from`.
- `ok: false` without `resend_from` means refused for good (for example a size the device cannot show); the phone reports the failure.
- No answer within 10 s of the last chunk counts as a failure on the phone.

**Device side, as I picture it** (your call): keep the image in RAM while chunks arrive, check the CRC, then write it to the `wallpaper` partition already in the partition table, keep the previous wallpaper until the new one is complete, and show it in WALLPAPER instead of the placeholder text in `core/src/screens.cpp`.

**Telling the phone it can.** The phone keeps Send off until `hello` carries `"wallpaper": true`, so an older firmware never receives writes it would drop. Please add that field when the receiver exists.

**Questions for Victor**

1. OK with the fields above, or would you rather have raw bytes with a small binary header, like audio? (JSON matches the contract wording; raw would be about a quarter smaller.)
2. OK with the answer on Events as a new `wallpaper` event type, and with `wallpaper: true` in `hello`?
3. Does the badge want the image in the current orientation, as proposed, or always 800 x 480 with the device rotating it?
4. Is 48,000 bytes in RAM during the transfer fine, or should the device write each chunk to flash as it arrives (then `resend_from` restarts from the first bad chunk)?
