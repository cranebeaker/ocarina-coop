"""Minimal MPQ (.otr) reader.

Only what an SoH OTR needs: v0/v1 headers, encrypted hash/block tables,
sector-based or single-unit files, stored / zlib / bzip2 sectors.

Written because mpyq returns EMPTY BYTES rather than raising for files it
cannot handle, which silently produced zero-length resources in the output
archive. Everything here either returns real data or raises.
"""

import bz2
import struct
import zlib

MPQ_MAGIC = b"MPQ\x1a"

FLAG_IMPLODE = 0x00000100
FLAG_COMPRESS = 0x00000200
FLAG_ENCRYPTED = 0x00010000
FLAG_FIX_KEY = 0x00020000
FLAG_SINGLE_UNIT = 0x01000000
FLAG_DELETE_MARKER = 0x02000000
FLAG_SECTOR_CRC = 0x04000000
FLAG_EXISTS = 0x80000000

COMP_HUFFMAN = 0x01
COMP_ZLIB = 0x02
COMP_PKWARE = 0x08
COMP_BZIP2 = 0x10
COMP_SPARSE = 0x20
COMP_ADPCM_MONO = 0x40
COMP_ADPCM_STEREO = 0x80

MASK32 = 0xFFFFFFFF


def _build_crypt_table():
    table = [0] * 0x500
    seed = 0x00100001

    for i in range(0x100):
        index = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp1 = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp2 = seed & 0xFFFF
            table[index] = temp1 | temp2
            index += 0x100

    return table


CRYPT_TABLE = _build_crypt_table()

HASH_TABLE_OFFSET = 0
HASH_NAME_A = 1
HASH_NAME_B = 2
HASH_FILE_KEY = 3


def mpq_hash(text, hash_type):
    seed1 = 0x7FED7FED
    seed2 = 0xEEEEEEEE

    for ch in text.upper():
        value = ord(ch)
        seed1 = (CRYPT_TABLE[(hash_type << 8) + value] ^ ((seed1 + seed2) & MASK32)) & MASK32
        seed2 = (value + seed1 + seed2 + (seed2 << 5) + 3) & MASK32

    return seed1


def mpq_decrypt(data, key):
    """Decrypt a whole number of little-endian dwords in place-ish."""
    count = len(data) // 4
    if count == 0:
        return b""

    words = list(struct.unpack("<%dI" % count, data[: count * 4]))
    seed2 = 0xEEEEEEEE

    for i in range(count):
        seed2 = (seed2 + CRYPT_TABLE[0x400 + (key & 0xFF)]) & MASK32
        value = (words[i] ^ ((key + seed2) & MASK32)) & MASK32
        key = (((~key << 0x15) & MASK32) + 0x11111111 | (key >> 0x0B)) & MASK32
        seed2 = (value + seed2 + ((seed2 << 5) & MASK32) + 3) & MASK32
        words[i] = value

    return struct.pack("<%dI" % count, *words) + data[count * 4:]


def _decompress_sector(blob, expected_size):
    """A sector whose stored size already equals its target is raw."""
    if len(blob) >= expected_size:
        return blob[:expected_size]

    if not blob:
        raise ValueError("empty compressed sector")

    mask = blob[0]
    payload = blob[1:]

    # Multiple bits set means chained compression; SoH does not produce that
    # and unwinding it blind would silently corrupt data.
    if mask & COMP_ZLIB:
        if mask & ~COMP_ZLIB:
            raise ValueError("chained compression 0x%02X not supported" % mask)
        return zlib.decompress(payload)

    if mask & COMP_BZIP2:
        if mask & ~COMP_BZIP2:
            raise ValueError("chained compression 0x%02X not supported" % mask)
        return bz2.decompress(payload)

    names = {
        COMP_HUFFMAN: "huffman",
        COMP_PKWARE: "PKWARE implode",
        COMP_SPARSE: "sparse",
        COMP_ADPCM_MONO: "ADPCM mono",
        COMP_ADPCM_STEREO: "ADPCM stereo",
    }
    for bit, name in names.items():
        if mask & bit:
            raise ValueError("unsupported compression: %s (0x%02X)" % (name, mask))

    raise ValueError("unknown compression mask 0x%02X" % mask)


class MPQArchive:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()

        offset = self.data.find(MPQ_MAGIC)
        if offset < 0:
            raise ValueError("not an MPQ archive: no header signature")

        self.base = offset
        header = self.data[offset:offset + 32]
        if len(header) < 32:
            raise ValueError("truncated MPQ header")

        (_magic, self.header_size, self.archive_size, self.format_version,
         self.block_size_shift, hash_offset, block_offset,
         self.hash_count, self.block_count) = struct.unpack("<4sIIHHIIII", header)

        self.sector_size = 512 << self.block_size_shift

        self.hash_table = self._read_table(hash_offset, self.hash_count, "(hash table)")
        self.block_table = self._read_table(block_offset, self.block_count, "(block table)")

    def _read_table(self, offset, count, key_name):
        start = self.base + offset
        size = count * 16
        raw = self.data[start:start + size]
        if len(raw) < size:
            raise ValueError("truncated %s" % key_name)

        raw = mpq_decrypt(raw, mpq_hash(key_name, HASH_FILE_KEY))
        return [struct.unpack("<4I", raw[i * 16:(i + 1) * 16]) for i in range(count)]

    def _find(self, name):
        index = mpq_hash(name, HASH_TABLE_OFFSET) & (self.hash_count - 1)
        want_a = mpq_hash(name, HASH_NAME_A)
        want_b = mpq_hash(name, HASH_NAME_B)

        for probe in range(self.hash_count):
            slot = (index + probe) & (self.hash_count - 1)
            name_a, name_b, locale_platform, block_index = self.hash_table[slot]

            if block_index == 0xFFFFFFFF:
                return None  # empty, never used -> the chain ends here
            if name_a == want_a and name_b == want_b and block_index != 0xFFFFFFFE:
                return block_index

        return None

    def read_file(self, name):
        """Return the file's bytes, or raise. Never returns empty for a
        non-empty file."""
        block_index = self._find(name)
        if block_index is None:
            raise KeyError(name)

        offset, archived_size, size, flags = self.block_table[block_index]

        if not (flags & FLAG_EXISTS) or (flags & FLAG_DELETE_MARKER):
            raise KeyError(name)

        if size == 0:
            return b""

        start = self.base + offset
        raw = self.data[start:start + archived_size]
        if len(raw) < archived_size:
            raise ValueError("%s: truncated file data" % name)

        key = None
        if flags & FLAG_ENCRYPTED:
            base_name = name.replace("/", "\\").rsplit("\\", 1)[-1]
            key = mpq_hash(base_name, HASH_FILE_KEY)
            if flags & FLAG_FIX_KEY:
                key = ((key + offset) ^ size) & MASK32

        compressed = bool(flags & (FLAG_COMPRESS | FLAG_IMPLODE))

        if flags & FLAG_SINGLE_UNIT:
            if key is not None:
                raw = mpq_decrypt(raw, key)
            if not compressed:
                return raw[:size]
            out = _decompress_sector(raw, size)
            if len(out) != size:
                raise ValueError("%s: expected %d bytes, got %d" % (name, size, len(out)))
            return out

        sector_count = (size + self.sector_size - 1) // self.sector_size

        if not compressed:
            if key is not None:
                raw = mpq_decrypt(raw, key)
            return raw[:size]

        table_bytes = (sector_count + 1) * 4
        if flags & FLAG_SECTOR_CRC:
            table_bytes += 4

        table_raw = raw[:table_bytes]
        if key is not None:
            table_raw = mpq_decrypt(table_raw, (key - 1) & MASK32)

        positions = list(struct.unpack("<%dI" % (len(table_raw) // 4), table_raw))

        out = bytearray()
        for i in range(sector_count):
            begin, end = positions[i], positions[i + 1]
            if end < begin or end > len(raw):
                raise ValueError("%s: sector %d out of range" % (name, i))

            blob = raw[begin:end]
            if key is not None:
                blob = mpq_decrypt(blob, (key + i) & MASK32)

            remaining = size - len(out)
            want = min(self.sector_size, remaining)

            piece = _decompress_sector(blob, want)
            if len(piece) != want:
                raise ValueError("%s: sector %d gave %d bytes, wanted %d"
                                 % (name, i, len(piece), want))
            out += piece

        if len(out) != size:
            raise ValueError("%s: expected %d bytes, got %d" % (name, size, len(out)))

        return bytes(out)

    def list_files(self):
        listfile = self.read_file("(listfile)")
        return [line.strip() for line in listfile.decode("utf-8", "replace").splitlines() if line.strip()]
