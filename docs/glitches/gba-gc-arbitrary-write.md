# GBA-to-GameCube Arbitrary Write

## In short

The character-name indexed write and the unchecked JoyBus receive buffer
compose into a general GameCube memory write:

1. Channel 2 character-name continuations replace channel 3's signed name
   packet counter.
2. Two channel 3 continuations use that counter to replace
   `Joybus.m_recvBuffer[3].m_length` with an arbitrary signed offset.
3. An orphan channel 3 letter continuation (`0x55`) makes
   `JoyBus::SetRecvBuffer` store its other three bytes at that offset.

The result is a controlled three-byte write anywhere in MEM1. Further orphan
continuations write the following three-byte groups. There is no header, CRC,
0x400-byte bound, item-table lookup or 0-63 selector in the final write.

The strongest static target is not an item or a story flag. It is `Game`'s
pending-script block. While the main game is paused, a short sequence can
write:

```text
m_newGameFlag     = 1
m_nextScriptFlags = 0
m_nextScript      = "startmap\0"
```

Unpausing calls `CGame::CheckScriptChange` and loads the named script. EN and
PAL contain `startmap.cft`, whose debug menu can load Meteor Parasite, the
ending maps, or almost any other map script. JP has the same vulnerable GC and
GBA code, but its disc does **not** contain `startmap.cft`; it needs an existing
JP script name instead.

This is currently a **source-complete and GBA-harness-confirmed exploit
chain**, not a speedrun-ready setup. The remaining large gate is producing the
radar ROP row from a stable retail smith/map layout. The complete GC write has
not yet been replayed end to end in Dolphin.

## The unrestricted GBA sender

The radar row overflow controls `r4-r7` and a second PC. The four ASCII bytes
`0F G` are also two legal Thumb instructions:

```text
30 46    mov r0, r6
20 47    bx  r4
```

Putting the retail `Link_Write|1` address in `r4` and any 32-bit word in `r6`
therefore presents that exact word to the GameCube. This is stronger than
calling `Link_SendEvent` or `Link_SendLetterReply`: no byte is restricted by a
menu field or packet constructor.

The headless harness presents `0x78563455` unchanged on every retail client:

| Region | `Radar_DrawMap` | Pop/return gadget | `Link_Write|1` | Result |
|---|---:|---:|---:|---|
| JP | `02003F94` | `02010251` | `020014C1` | `0x78563455` |
| EN | `0200197C` | `020150C5` | `02002699` | `0x78563455` |
| PAL | `0200197C` | `02015281` | `02002699` | `0x78563455` |

The probes are:

```powershell
build/tools/gba_joybus_harness.exe orig/GCCJGC/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCE01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_en_probe.txt
build/tools/gba_joybus_harness.exe orig/GCCP01/files/dvd/gba/ffcc_cli.bin tools/gba_joybus_radar_raw_write_rop_pal_probe.txt
```

A crash after `Link_Write` is harmless. Once the GC has read the word, the GBA
may be rebooted and the GC-side parser state remains.

## Turning the name index into a receive-length write

`GbaQueue::ChkCMakeName` handles a continuation as:

```cpp
writeIndex = cmakeInfo[channel].m_packetCount * 3 - 2;
cmakeInfo[channel].m_packetCount++;
cmakeInfo[channel].m_name[writeIndex + 0] = byte1;
cmakeInfo[channel].m_name[writeIndex + 1] = byte2;
cmakeInfo[channel].m_name[writeIndex + 2] = byte3;
```

Channel 2 continuation 10 writes channel 3's counter high byte, and
continuation 11 writes its low byte. Raw `Link_Write` removes the former JP
name-alphabet restriction. Set the counter to `C02C`, signed -16340. On all
three regions the next channel 3 continuation begins exactly one byte before
receive buffer 3's length:

| Region | Channel 3 indexed-write base | Receive 3 length | Receive 3 payload |
|---|---:|---:|---:|
| PAL | `802F76D3` | `802EB758` | `802EB760` |
| EN | `802F6693` | `802EA718` | `802EA720` |
| JP | `80312FC3` | `80307048` | `80307050` |

The relative layout is identical, hence the same `C02C` counter in every
region. Two continuation triples can write the complete big-endian length and
clear `m_cmdFlags`:

```text
count C02C:  [disposable preceding byte, length byte 0, length byte 1]
count C02D:  [length byte 2, length byte 3, 00]
```

The disposable byte is the last payload byte of receive buffer 2. It should
not hold a live message during the setup.

## Why one `0x55` word is an arbitrary write

For a nonzero receive length, `JoyBus::SetRecvBuffer` does this without a
range check:

```cpp
buf->m_payload[buf->m_length++] = byte1;
buf->m_payload[buf->m_length++] = byte2;
buf->m_payload[buf->m_length++] = byte3;
```

To write at address `T`, store this signed 32-bit value in the length:

```text
length = T - &Joybus.m_recvBuffer[3].m_payload[0]
```

Then present wire bytes `55 AA BB CC` on channel 3. `RecvGBA` first appends
the word to its secondary queue, `GBARecvSend` removes that same entry, and
`SetRecvBuffer` writes `AA BB CC` at `T`. A missing `0x15` header does not
underflow the secondary queue. Because the word is a middle continuation, no
CRC is checked and no `ReplyLetter` handler runs.

Additional `0x55` words write `T+3`, `T+6`, and so on. A normal-looking raw
header `15 00 00 00` afterward calls `ClrRecvBuffer` and is a useful cleanup.

## Direct script warp

The relevant `CGame` fields are contiguous:

```text
Game + CB28  int m_newGameFlag
Game + CB2C  u32 m_nextScriptFlags
Game + CB30  char m_nextScript[256]
```

`CGame::SetNextScript` copies the name and sets `m_newGameFlag = 1`.
`CheckScriptChange` clears the flag, loads the named CFlat script, and only
performs a new-game reset when `m_nextScriptFlags` is nonzero.

Static addresses are:

| Region | Pending flag | Reset flags | Script name |
|---|---:|---:|---:|
| PAL | `8022B9E8` | `8022B9EC` | `8022B9F0` |
| EN | `8022A9A8` | `8022A9AC` | `8022A9B0` |
| JP | `802472E8` | `802472EC` | `802472F0` |

For EN `startmap`, point receive buffer 3 at `8022A9A8` and stream these six
payload triples while the main game is paused:

```text
00 00 00   01 00 00   00 00 73   74 61 72   74 6D 61   70 00 00
| m_newGameFlag | | reset flags | |       "startmap\0"        |
```

The second triple makes the flag nonzero before the name is complete. Pausing
is therefore required: JoyBus reception continues, while the main loop cannot
observe the flag until the full block and optional cleanup have arrived.

This is better than the original `G59` plan:

- it uses only static addresses;
- it works from any script rather than requiring a Moogle-hole exit;
- it can name any script present on the disc;
- one forged length streams the flag, reset flags and name together.

For EN/PAL, `startmap`'s BATTLE submenu exposes `METEO 0`, `METEO 2`,
`METEO 3`, `LAST 5`, `LAST 3`, and `LAST 4`. `map_jump_from_startmap` maps
those last three choices directly to `last_5`, `last_3`, and `last_4`.
`last_5` is the post-Meteor ending map. Which choice is accepted as a timed
run completion, and whether a direct `last_5` load starts in a usable state,
still need live testing.

JP can directly name an existing script such as `last_5` or `meteo_2`, but
cannot use `startmap`: the JP `mog_0` script contains the same default
`loadCFlat('startmap')` branch, yet the JP disc omits both `startmap.cft` and
its debug file.

## Exact packet planner

[`tools/gba_gc_write_plan.py`](../../tools/gba_gc_write_plan.py) prints the
raw words, byte order, target addresses and forged length. Examples:

```powershell
python tools/gba_gc_write_plan.py --region EN --script startmap
python tools/gba_gc_write_plan.py --region PAL --script startmap
python tools/gba_gc_write_plan.py --region JP --script last_5
python tools/gba_gc_write_plan.py --region EN --target 0x8021DE91 --data 000008
```

The emitted schedule uses only ports 2 and 3:

1. On port 2, send a name header and nine zero continuations.
2. Send continuation 10 as `5C 01 00 C0`.
3. Send continuation 11 as `5C 2C 00 00`.
4. On port 3, send the two calculated `0x5C` length writes.
5. Send the listed `0x55` data words, then optional cleanup.

Allow `GbaQueue::ExecutQueue` to process each `0x1C`/`0x5C` word before the
next one. Rebooting a GBA after its successful send is safe. A physical setup
can use two GBAs or move one powered/rebooted GBA from channel 2 to channel 3,
provided reconnect does not clear the already-corrupted GC records.

## Other targets

Once the transport is usable, exact item writes are no longer the interesting
ceiling:

- `Game.m_gameWork` permits exact year, event-flag and event-work values;
- the low three bytes of a normal small CFlat global can be replaced while
  preserving its zero high byte;
- pointers, pending map state and script names are addressable;
- Big Chungus can be written directly, although the negative-letter
  `DeleteItemIdx(-72)` route is shorter.

Writing only `m_scriptSysVal0` to a high year is not a complete progression
skip. The normal `changeYear()` script also derives region state—for example,
year 5 sets `eventWork[1]=5` and `[2]=5`, while year 8 or later sets them to 6
and 1. A raw year write bypasses that derivation. Direct script loading is a
cleaner high-value target.

## Remaining gates

- The radar PC-control probe uses a synthetic map row. A stable retail
  smith/list sequence that builds the complete map header and row is still
  unsolved.
- The current `0F G` payload emits one arbitrary word per execution. The
  packet plan therefore needs repeated executions/reboots unless a batching
  payload or callable retail multiword sender is found.
- The two-port counter schedule is source-complete but has not been performed
  against a live GC process.
- The direct-script block must be streamed while paused and then tested for
  scene/map initialization on each target script.

Those are substantial route-construction problems, but they are downstream
of the memory primitive. There is no remaining 0-63 check, food-row lottery,
or fixed `0xFFFF` value restriction in the final write.
