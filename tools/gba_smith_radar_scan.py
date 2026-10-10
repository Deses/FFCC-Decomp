#!/usr/bin/env python3
"""Search JP MakeSmithData records for Radar_DrawMap return-address control.

The GameCube smith-list builder accepts positive inventory IDs through 32767
and copies unusually useful halfwords from their (usually out-of-bounds) item
rows.  This tool models the exact 0x38-byte record sent to the GBA, finds
records that a wrong craft can create for one tribe, and searches record
streams for initializable map headers or a radar row that expands through
saved LR and a retail ROP/name payload.

No emulator memory is modified.  By default the tool reads a running JP
Dolphin.  A raw 24-MiB MEM1 dump can be supplied instead.  The item table is
loaded from param.cfd at runtime, so a DOL alone is not a valid search image.
"""

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path


MEM1_BASE = 0x80000000
MEM1_SIZE = 0x01800000
JP_ITEM_TABLE = 0x80979FC0
ROW_SIZE = 0x48
SAVED_LR_HALFWORD = 296
TRIBES = ("clavat", "lilty", "yuke", "selkie")


class Mem1:
    def __init__(self, data):
        if len(data) != MEM1_SIZE:
            raise ValueError(f"expected {MEM1_SIZE:#x} bytes of MEM1, got {len(data):#x}")
        self.data = data

    def _offset(self, address, size):
        offset = address - MEM1_BASE
        if offset < 0 or offset + size > len(self.data):
            return None
        return offset

    def u16(self, address):
        offset = self._offset(address, 2)
        if offset is None:
            return None
        return struct.unpack_from(">H", self.data, offset)[0]


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def row_address(item_table, item_id):
    return item_table + signed16(item_id & 0xFFFF) * ROW_SIZE


def row_halfword(mem, item_table, item_id, offset):
    return mem.u16(row_address(item_table, item_id) + offset)


def wrong_craft_sources(mem, item_table, tribe):
    """Map result item -> clean gil values for the requested result column.

    A source is called clean when SetSmithData's material loop exits before
    deleting anything: material[0] == 0 or count[0] == 0.  Reads may start on
    either side of the real item table because the base item is loaded signed.
    """
    sources = defaultdict(list)
    result_offset = 0x38 + tribe * 2
    for gil in range(0x10000):
        material = row_halfword(mem, item_table, gil, 0x26)
        count = row_halfword(mem, item_table, gil, 0x2C)
        result = row_halfword(mem, item_table, gil, result_offset)
        price = row_halfword(mem, item_table, gil, 0x24)
        if None in (material, count, result, price):
            continue
        if result >= 401 and (material == 0 or count == 0):
            sources[result].append((gil, price, material, count))
    return sources


def smith_record(mem, item_table, item_id, rate):
    """Return the 28 GBA-native halfwords made for one smith-list record."""
    smith_price = row_halfword(mem, item_table, item_id, 0x24)
    if smith_price is None:
        return None
    price = int(smith_price * rate / 100.0) & 0xFFFFFFFF
    words = [price & 0xFFFF, price >> 16]

    for offset in range(0x26, 0x32, 2):
        value = row_halfword(mem, item_table, item_id, offset)
        if value is None:
            return None
        words.append(value)

    results = []
    for offset in range(0x38, 0x40, 2):
        value = row_halfword(mem, item_table, item_id, offset)
        if value is None:
            return None
        results.append(value)
    words.extend(results)

    for result in results:
        if result == 0:
            words.extend((0, 0, 0, 0))
            continue
        fields = [row_halfword(mem, item_table, result, offset) for offset in (4, 6, 8)]
        if any(value is None for value in fields):
            return None
        words.extend((*fields, 0))
    assert len(words) == 28
    return words


def pc_hits(words, first_record_words=28):
    """Yield token starts that overwrite saved LR with a GBA EWRAM address."""
    for start in range(len(words) - 2):
        expanded = 0
        cursor = start
        while cursor < len(words) - 2 and expanded < SAVED_LR_HALFWORD:
            token = words[cursor]
            cursor += 1
            expanded += (token & 0x3FF) if token & 0x8000 else 1
        if expanded != SAVED_LR_HALFWORD or cursor + 1 >= len(words):
            continue
        low, high = words[cursor:cursor + 2]
        if low & 0x8000 or high & 0x8000 or not (low & 1):
            continue
        target = low | (high << 16)
        if 0x02000001 <= target <= 0x0203FFFF:
            yield start, cursor, target, first_record_words


def expand_row(words, start, limit):
    """Expand radar tokens, stopping after limit output halfwords."""
    output = []
    cursor = start
    while cursor < len(words) and len(output) < limit:
        token = words[cursor]
        cursor += 1
        if token & 0x8000:
            count = token & 0x3FF
            output.extend([0x1000 | ((token >> 10) & 3)] * min(count, limit - len(output)))
        else:
            output.append(token)
    return output, cursor


def build_smith_buffer(records, slots=None, shop_args=(0, 0, 0, 0)):
    """Build the byte layout MakeSmithData writes for an ordered recipe list."""
    if slots is None:
        slots = list(range(len(records)))
    count = len(records)
    record_start = (count + 1 + 3) & ~3
    output = bytearray(record_start + count * 0x38 + 16)
    output[0] = count
    output[1:1 + count] = bytes(slots)
    cursor = record_start
    for record in records:
        struct.pack_into("<28H", output, cursor, *record)
        cursor += 0x38
    for value in shop_args:
        struct.pack_into("<I", output, cursor, value)
        cursor += 4
    return output


def inspect_map_overlay(map_data, smith_data, pointer_lo=0x20, pointer_hi=0x7E):
    """Describe pointer alignments that leave map structures in the download."""
    results = []
    for pointer in range(pointer_lo, pointer_hi + 1):
        arena = bytearray(pointer + len(map_data))
        arena[pointer:pointer + len(map_data)] = map_data
        arena[:len(smith_data)] = smith_data
        download = arena[pointer:pointer + min(0xD20, len(map_data))]
        if len(download) < 0x40:
            continue
        map_offset = struct.unpack_from("<I", download, 0x0C)[0]
        width, height = struct.unpack_from("<HH", download, 0x14)
        if map_offset > len(download) - 9:
            continue
        x, y, data_offset = struct.unpack_from("<hhI", download, map_offset)
        data = map_offset + data_offset
        if data < 0 or data > len(download) - 4:
            continue
        row_count = min(height >> 3, len(download) - (map_offset + 8), 512)
        if row_count <= 0:
            continue
        row_cursor = data
        overflow_rows = []
        for row in range(row_count):
            token_count = download[map_offset + 8 + row]
            end = row_cursor + token_count * 2
            if end > len(download):
                break
            words = list(struct.unpack_from(f"<{token_count}H", download, row_cursor)) if token_count else []
            row_hits = list(pc_hits(words, 0))
            if row_hits:
                overflow_rows.extend((row, token_count, hit[2]) for hit in row_hits)
            row_cursor = end
        results.append((pointer, map_offset, width, height, x, y, data_offset, data, overflow_rows))
    return results


def fmt_source(source):
    prefix = ""
    if len(source) == 5:
        tribe, gil, price, material, count = source
        prefix = f"tribe={tribe} "
    else:
        gil, price, material, count = source
    signed = signed16(gil)
    return f"{prefix}gil={gil} ({signed:+d}) price={price} material0={material} count0={count}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mem1", type=Path,
                        help="raw 24-MiB MEM1 dump instead of live Dolphin")
    parser.add_argument("--item-table", type=lambda x: int(x, 0), default=JP_ITEM_TABLE)
    parser.add_argument("--tribe", choices=TRIBES + ("mixed",), default="lilty",
                        help="result column, or mixed if items may be transferred between tribes")
    parser.add_argument("--rate", type=int, default=100, help="smith price percentage (default: 100)")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--pair", nargs=2, type=lambda x: int(x, 0), metavar=("A", "B"),
                        help="inspect one ordered pair instead of searching all pairs")
    parser.add_argument("--headers", action="store_true",
                        help="list craftable record windows that form a plausible map header")
    parser.add_argument("--zero-lz", action="store_true",
                        help="with --headers, also require 10 00 00 00 at map offset 0x20")
    parser.add_argument("--rop-chain", choices=("event", "item", "cmdslot", "letter", "routeletter", "cmdwrite", "equipwrite", "routeequipwrite"),
                        help="find three-record chains that execute a legal JP name payload")
    parser.add_argument("--letter-index", type=lambda x: int(x, 0), default=-72,
                        help="signed DeleteItemIdx target for letter chains (default: -72)")
    parser.add_argument("--letter-item", type=lambda x: int(x, 0), default=1,
                        help="nonzero low-half item value for letter chains (default: 1)")
    parser.add_argument("--cmd-index", type=lambda x: int(x, 0), default=238,
                        help="u8 ChgCmdLst destination for cmdwrite chains (default: 238/tribe)")
    parser.add_argument("--cmd-value", type=lambda x: int(x, 0), default=0x1234,
                        help="signed or raw 16-bit value for cmdwrite chains (default: 0x1234)")
    parser.add_argument("--equip-index", type=lambda x: int(x, 0), default=5,
                        help="signed or raw equip index for equipwrite chains (default: 5/inventory slot 0)")
    parser.add_argument("--equip-value", type=lambda x: int(x, 0), default=-102,
                        help="signed or raw byte written by equipwrite chains (default: -102)")
    parser.add_argument("--map", type=Path,
                        default=Path("orig/GCCJGC/files/dvd/gba/m01_1.mcd"),
                        help="valid map staged before the smith overwrite")
    args = parser.parse_args()
    if not -0x8000 <= args.letter_index <= 0x7FFF:
        parser.error("--letter-index must be a signed 16-bit value")
    if not 1 <= args.letter_item <= 0xFFFF:
        parser.error("--letter-item must be 1..65535")
    if not 0 <= args.cmd_index <= 0xFF:
        parser.error("--cmd-index must be 0..255")
    if not -0x8000 <= args.cmd_value <= 0xFFFF:
        parser.error("--cmd-value must be a signed or raw 16-bit value")
    if not -0x80 <= args.equip_index <= 0xFF:
        parser.error("--equip-index must be a signed or raw 8-bit value")
    if not -0x80 <= args.equip_value <= 0xFF:
        parser.error("--equip-value must be a signed or raw 8-bit value")
    if args.zero_lz and not args.headers:
        parser.error("--zero-lz requires --headers")

    if args.mem1:
        data = args.mem1.read_bytes()
    else:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from cles_scan import read_dolphin_mem1
        data = read_dolphin_mem1(b"GCCJGC")
    mem = Mem1(data)
    if args.tribe == "mixed":
        sources = defaultdict(list)
        for tribe_index, tribe_name in enumerate(TRIBES):
            for item_id, tribe_sources in wrong_craft_sources(mem, args.item_table, tribe_index).items():
                sources[item_id].extend((tribe_name, *source) for source in tribe_sources)
    else:
        tribe = TRIBES.index(args.tribe)
        sources = wrong_craft_sources(mem, args.item_table, tribe)
    records = {}
    for item_id in sorted(sources):
        if item_id > 32767:  # MakeSmithData's signed item ID fails itemId >= 401.
            continue
        record = smith_record(mem, args.item_table, item_id, args.rate)
        if record is not None:
            records[item_id] = record

    print(f"tribe={args.tribe} clean output IDs={len(sources)} smith records={len(records)} rate={args.rate}")
    if args.pair:
        missing = [item for item in args.pair if item not in records]
        if missing:
            raise SystemExit(f"pair item(s) not clean-craftable/safe for {args.tribe}: {missing}")
        first, second = args.pair
        smith_data = build_smith_buffer([records[first], records[second]])
        print(f"smith bytes={len(smith_data)} records=({first},{second})")
        print("A source: " + fmt_source(sources[first][0]))
        print("B source: " + fmt_source(sources[second][0]))
        plausible = inspect_map_overlay(args.map.read_bytes(), smith_data)
        print(f"pointer alignments with in-download map/info/data={len(plausible)}")
        for pointer, map_offset, width, height, x, y, data_offset, data, overflows in plausible:
            suffix = ""
            if overflows:
                suffix = " OVERFLOW " + ",".join(
                    f"row={row}/tokens={count}/pc={target:08X}" for row, count, target in overflows)
            print(f"  +{pointer:02X}: map={map_offset:04X} size={width}x{height} "
                  f"origin=({x},{y}) dataOff={data_offset:04X} data={data:04X}{suffix}")
        return

    if args.headers:
        headers = []
        record_items = sorted(records)
        record_bytes = {item: struct.pack("<28H", *records[item]) for item in record_items}
        if args.zero_lz:
            # The exact four-byte LZ header is selective enough to index first,
            # avoiding an items^2 * 56 brute force.  A header at offsets 0..20
            # is wholly within one record; later offsets need its successor.
            target = b"\x10\x00\x00\x00"
            for offset in range(0x38):
                lz_at = offset + 0x20
                first_match = record_items
                second_match = record_items
                if lz_at < 0x38:
                    take = min(4, 0x38 - lz_at)
                    first_match = [item for item in record_items
                                   if record_bytes[item][lz_at:lz_at + take] == target[:take]]
                    if take < 4:
                        second_match = [item for item in record_items
                                        if record_bytes[item][:4 - take] == target[take:]]
                else:
                    at = lz_at - 0x38
                    second_match = [item for item in record_items
                                    if record_bytes[item][at:at + 4] == target]

                if offset <= 0x14:
                    for first in first_match:
                        blob = record_bytes[first]
                        map_offset = struct.unpack_from("<I", blob, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", blob, offset + 0x14)
                        if 0x24 <= map_offset <= 0xD17 and 8 <= width <= 0x1000 and 8 <= height <= 0x1000:
                            headers.append((map_offset, first, None, offset, width, height))
                    continue

                for first in first_match:
                    for second in second_match:
                        pair_bytes = record_bytes[first] + record_bytes[second]
                        map_offset = struct.unpack_from("<I", pair_bytes, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", pair_bytes, offset + 0x14)
                        if 0x24 <= map_offset <= 0xD17 and 8 <= width <= 0x1000 and 8 <= height <= 0x1000:
                            headers.append((map_offset, first, second, offset, width, height))
        else:
            for first in record_items:
                first_bytes = record_bytes[first]
                for second in record_items:
                    pair_bytes = first_bytes + record_bytes[second]
                    for offset in range(0, 0x38, 4):
                        map_offset = struct.unpack_from("<I", pair_bytes, offset + 0x0C)[0]
                        width, height = struct.unpack_from("<HH", pair_bytes, offset + 0x14)
                        if not (0x18 <= map_offset <= 0xD17):
                            continue
                        if not (8 <= width <= 0x1000 and 8 <= height <= 0x1000):
                            continue
                        headers.append((map_offset, first, second, offset, width, height))
        headers.sort()
        qualifier = "initializable zero-LZ" if args.zero_lz else "plausible two-record"
        print(f"{qualifier} header windows={len(headers)}")
        for map_offset, first, second, offset, width, height in headers[:args.limit]:
            second_text = "*" if second is None else str(second)
            print(f"  map={map_offset:04X} size={width}x{height} records=({first},{second_text}) A+{offset}")
            print("    A source: " + fmt_source(sources[first][0]))
            if second is not None:
                print("    B source: " + fmt_source(sources[second][0]))
        return
    if args.rop_chain:
        # Radar's first overwritten PC is line[296].  The GilScreen gadgets
        # load r4-r7 from line[302..309] and the next PC from line[310].  The
        # cmdwrite gadget first pops r3, shifting those positions by one word.
        # A legal JP name at DETAIL_BUF turns the registers into the selected
        # outbound command.
        event_chain = args.rop_chain == "event"
        item_chain = args.rop_chain == "item"
        letter_chain = args.rop_chain == "letter"
        route_letter_chain = args.rop_chain == "routeletter"
        cmdwrite_chain = args.rop_chain == "cmdwrite"
        equipwrite_chain = args.rop_chain == "equipwrite"
        route_equipwrite_chain = args.rop_chain == "routeequipwrite"
        any_letter_chain = letter_chain or route_letter_chain
        gadget = (0x0200570D if route_equipwrite_chain else
                  0x02002555 if cmdwrite_chain else
                  0x02010251 if event_chain or any_letter_chain else 0x02010201)
        send_function = (0x02001E01 if event_chain else
                         0x02001F39 if item_chain else
                         0x02002129 if args.rop_chain == "cmdslot" else
                         0x020020F9 if equipwrite_chain else
                         0x02001E31)
        name_entry = 0x02035803 if route_letter_chain or cmdwrite_chain or route_equipwrite_chain else 0x02035801
        letter_value = ((args.letter_index & 0xFFFF) << 16) | args.letter_item
        cmd_value = args.cmd_value & 0xFFFF
        record_items = sorted(records)
        record_blobs = {item: struct.pack("<28H", *records[item]) for item in record_items}
        word_index = defaultdict(list)
        word_low_index = defaultdict(list)
        half_index = defaultdict(list)
        if route_letter_chain or cmdwrite_chain or route_equipwrite_chain:
            for item, blob in record_blobs.items():
                for offset in range(0, len(blob) - 3, 4):
                    word = struct.unpack_from("<I", blob, offset)[0]
                    word_index[(offset, word)].append(item)
                    word_low_index[(offset, word & 0xFF)].append((item, word))
                if cmdwrite_chain:
                    for offset in range(0, len(blob) - 1, 2):
                        half_index[(offset, struct.unpack_from("<H", blob, offset)[0])].append(item)
        first_stages = []
        for first in record_items:
            for second in record_items:
                words = records[first] + records[second]
                for start, _, target, _ in pc_hits(words):
                    if target == gadget:
                        first_stages.append((first, second, start))
        print(f"epilogue-gadget first stages={len(first_stages)}")

        chains = []
        for first, second, start in first_stages:
            for third in record_items:
                needed = 302 if route_equipwrite_chain else 314 if cmdwrite_chain else 312
                expanded, _ = expand_row(records[first] + records[second] + records[third], start, needed)
                if len(expanded) < needed:
                    continue
                if expanded[296] | (expanded[297] << 16) != gadget:
                    continue
                reg_start = 288 if route_equipwrite_chain else 304 if cmdwrite_chain else 302
                regs = [expanded[index] | (expanded[index + 1] << 16)
                        for index in range(reg_start, reg_start + 8, 2)]
                pc_index = 300 if route_equipwrite_chain else 312 if cmdwrite_chain else 310
                next_pc = expanded[pc_index] | (expanded[pc_index + 1] << 16)
                arg_ok = True
                if item_chain:
                    arg_ok = regs[2] & 0xFF == 0xA6
                elif args.rop_chain == "cmdslot":
                    arg_ok = regs[2] & 0xFFFF == 72
                elif equipwrite_chain:
                    arg_ok = ((regs[1] & 0xFF) == (args.equip_index & 0xFF) and
                              (regs[2] & 0xFF) == (args.equip_value & 0xFF))
                if route_equipwrite_chain:
                    if next_pc != name_entry:
                        continue
                    if (regs[0] & 0xFF) != (args.equip_index & 0xFF):
                        continue
                    if ((regs[3] + regs[2]) & 0xFFFFFFFF) != 0x02018DB9:
                        continue
                    load_address = (regs[1] + 52) & 0xFFFFFFFF
                    layouts = []
                    buffer_offset = load_address - 0x02032800
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3
                        stream_offset = buffer_offset - record_start
                        if stream_offset < 0:
                            continue
                        slot, within = divmod(stream_offset, 0x38)
                        if slot >= count or within > 0x34 or within & 3:
                            continue
                        for source_item, source_word in word_low_index.get(
                                (within, args.equip_value & 0xFF), ()):
                            if slot < 3 and source_item != (first, second, third)[slot]:
                                continue
                            layouts.append((count, slot, source_item, load_address, source_word))
                    if layouts:
                        chains.append((first, second, third, start, regs,
                                       "えkk7D8G", layouts[0]))
                elif cmdwrite_chain:
                    if next_pc != name_entry:
                        continue
                    restored_r3 = expanded[302] | (expanded[303] << 16)
                    if ((regs[3] + regs[2]) & 0xFFFFFFFF) != 0x0200AD4F:
                        continue
                    if regs[2] & 0xFF != args.cmd_index:
                        continue
                    base_address = (regs[1] + 52) & 0xFFFFFFFF
                    value_address = (restored_r3 + regs[0] + 0xF4) & 0xFFFFFFFF
                    layouts = []
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3

                        def locate(address, size):
                            stream_offset = address - 0x02032800 - record_start
                            if stream_offset < 0:
                                return None
                            slot, within = divmod(stream_offset, 0x38)
                            if slot >= count or within > 0x38 - size or within & (size - 1):
                                return None
                            return slot, within

                        base_loc = locate(base_address, 4)
                        value_loc = locate(value_address, 2)
                        if base_loc is None or value_loc is None:
                            continue
                        base_slot, base_within = base_loc
                        value_slot, value_within = value_loc
                        base_items = set(word_index.get((base_within, restored_r3), ()))
                        value_items = set(half_index.get((value_within, cmd_value), ()))
                        chain_items = (first, second, third)
                        if base_slot < 3:
                            base_items &= {chain_items[base_slot]}
                        if value_slot < 3:
                            value_items &= {chain_items[value_slot]}
                        if base_slot == value_slot:
                            both = base_items & value_items
                            if not both:
                                continue
                            base_item = value_item = min(both)
                        else:
                            if not base_items or not value_items:
                                continue
                            base_item = min(base_items)
                            value_item = min(value_items)
                        layouts.append((count, base_slot, base_item, base_address, restored_r3,
                                        value_slot, value_item, value_address))
                    if layouts:
                        chains.append((first, second, third, start, regs,
                                       "えkk7D8G", layouts[0]))
                elif route_letter_chain:
                    if next_pc != name_entry:
                        continue
                    load_address = (regs[1] + 52) & 0xFFFFFFFF
                    if not (0x02032800 <= load_address <= 0x020357FC) or load_address & 3:
                        continue
                    required_word = (letter_value - regs[2]) & 0xFFFFFFFF
                    layouts = []
                    buffer_offset = load_address - 0x02032800
                    for count in range(3, 65):
                        record_start = (count + 4) & ~3
                        stream_offset = buffer_offset - record_start
                        if stream_offset < 0:
                            continue
                        slot, within = divmod(stream_offset, 0x38)
                        if slot >= count or within > 0x34 or within & 3:
                            continue
                        for source_item in word_index.get((within, required_word), ()):
                            if slot < 3 and source_item != (first, second, third)[slot]:
                                continue
                            layouts.append((count, slot, source_item, load_address, required_word))
                    if not layouts:
                        continue
                    if regs[0] == send_function:
                        chains.append((first, second, third, start, regs, "えkk3D G", layouts[0]))
                    if regs[3] == send_function:
                        chains.append((first, second, third, start, regs, "えkk3D8G", layouts[0]))
                elif letter_chain:
                    if next_pc != name_entry:
                        continue
                    # "kB3D" computes r3 = r6 - r5.  If the callable is in
                    # r7, "cB3D" can instead compute r3 = r6 - r4.  The
                    # harness uses 0x00480001/2, but any popped pair with this
                    # modular difference produces the requested attachment.
                    if (regs[0] == send_function and
                            (regs[2] - regs[1]) & 0xFFFFFFFF == letter_value):
                        chains.append((first, second, third, start, regs, "kB3D G", None))
                    if regs[3] == send_function:
                        if (regs[2] - regs[1]) & 0xFFFFFFFF == letter_value:
                            chains.append((first, second, third, start, regs, "kB3D8G", None))
                        if (regs[2] - regs[0]) & 0xFFFFFFFF == letter_value:
                            chains.append((first, second, third, start, regs, "cB3D8G", None))
                else:
                    if regs[0] == send_function and next_pc == name_entry and arg_ok:
                        chains.append((first, second, third, start, regs, None, None))
                    if regs[3] == send_function and next_pc == name_entry and arg_ok:
                        chains.append((first, second, third, start, regs, None, None))

        print(f"complete smith-to-name {args.rop_chain} chains={len(chains)}")
        for first, second, third, start, regs, chain_name, layout in chains[:args.limit]:
            branch_reg = "r4" if regs[0] == send_function else "r7"
            if any_letter_chain:
                name = chain_name
                argument = args.letter_index
            elif cmdwrite_chain:
                name = chain_name
                argument = f"index={args.cmd_index} value={cmd_value:04X}"
            elif route_equipwrite_chain:
                name = chain_name
                argument = f"index={args.equip_index & 0xFF:02X} value={args.equip_value & 0xFF:02X}"
            elif equipwrite_chain:
                name = "(F1F G" if branch_reg == "r4" else "(F1F8G"
                argument = f"index={args.equip_index & 0xFF:02X} value={args.equip_value & 0xFF:02X}"
            elif args.rop_chain == "cmdslot":
                name = "qB G" if branch_reg == "r4" else "qB8G"
                argument = -72
            else:
                name = "1F2F G" if branch_reg == "r4" else "1F2F8G"
                argument = regs[2] & 0xFF
            print(f"  records=({first},{second},{third}) start=A+{start} "
                  f"name={name!r} arg={argument} branch={branch_reg}")
            print("    regs=" + " ".join(f"r{4 + i}={value:08X}" for i, value in enumerate(regs)))
            print("    A source: " + fmt_source(sources[first][0]))
            print("    B source: " + fmt_source(sources[second][0]))
            print("    C source: " + fmt_source(sources[third][0]))
            if layout is not None and cmdwrite_chain:
                (count, base_slot, base_item, base_address, base_value,
                 value_slot, value_item, value_address) = layout
                print(f"    list count={count} base slot={base_slot} record={base_item} "
                      f"loads {base_value:08X} at {base_address:08X}")
                print("    base source: " + fmt_source(sources[base_item][0]))
                print(f"    value slot={value_slot} record={value_item} "
                      f"loads {cmd_value:04X} at {value_address:08X}")
                print("    value source: " + fmt_source(sources[value_item][0]))
            elif layout is not None:
                count, slot, source_item, load_address, required_word = layout
                print(f"    list count={count} slot={slot} record={source_item} "
                      f"loads {required_word:08X} at {load_address:08X}")
                print("    load source: " + fmt_source(sources[source_item][0]))
        return

    hits = []
    record_items = sorted(records)
    for first in record_items:
        for second in record_items:
            words = records[first] + records[second]
            for start, address_at, target, _ in pc_hits(words):
                hits.append((target, first, second, start, address_at, words[start:address_at + 2]))

    hits.sort()
    print(f"two-record EWRAM PC hits={len(hits)}")
    for target, first, second, start, address_at, tokens in hits[:args.limit]:
        where = f"A+{start}" if start < 28 else f"B+{start - 28}"
        address_where = f"A+{address_at}" if address_at < 28 else f"B+{address_at - 28}"
        print(f"target={target:08X} records=({first},{second}) tokens={where}..{address_where + '+1'}")
        print("  " + " ".join(f"{word:04X}" for word in tokens))
        print("  A source: " + fmt_source(sources[first][0]))
        print("  B source: " + fmt_source(sources[second][0]))


if __name__ == "__main__":
    main()
