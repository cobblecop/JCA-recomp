"""Extract the game executable from your own Jackie Chan Adventures (PAL) disc image.

The recompiler works on the raw PS2 ELF, not on the ISO, so this pulls
SCES_524.12 out of the ISO9660 root directory and writes it to game/.

Usage:
    python3 tools/extract_iso.py [path/to/disc.iso]
"""
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_DIR = ROOT / "game"
DEFAULT_ISO = GAME_DIR / "Jackie Chan Adventures (Europe, Australia) (En,Fr,De,Es,It,Nl,Pt).iso"
ELF_NAME = "SCES_524.12"
SECTOR = 2048


def read_root_entries(f):
    """Parse the ISO9660 Primary Volume Descriptor and return root dir entries."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[0:1] != b'\x01' or pvd[1:6] != b'CD001':
        print("[-] Not a valid ISO9660 disc (missing CD001 PVD).")
        return []

    # Root directory record starts at offset 156 (0x9C) within the PVD.
    off = 156
    rec_len = pvd[off]
    loc_le = struct.unpack('<I', pvd[off + 2:off + 6])[0]
    data_len = struct.unpack('<I', pvd[off + 10:off + 14])[0]

    f.seek(loc_le * SECTOR)
    root = f.read(data_len)

    entries = []
    pos = 0
    while pos < len(root):
        rlen = root[pos]
        if rlen == 0:
            break
        loc = struct.unpack('<I', root[pos + 2:pos + 6])[0]
        size = struct.unpack('<I', root[pos + 10:pos + 14])[0]
        flags = root[pos + 25]
        fi_len = root[pos + 32]
        fi_start = pos + 33
        name = root[fi_start:fi_start + fi_len]
        if b';1' in name:
            name = name.split(b';1')[0]
        # skip '.' and '..'
        if name in (b'', b'\x00', b'\x01'):
            pos += rlen
            continue
        entries.append({
            'name': name.decode('ascii', errors='replace'),
            'loc': loc,
            'size': size,
            'is_dir': bool(flags & 0x8),
        })
        pos += rlen
    return entries


def extract_elf(iso_path, out_path):
    if not iso_path.is_file():
        print(f"[-] Error: Could not find ISO file at '{iso_path}'.")
        print("    Pass the path to your disc image: python3 tools/extract_iso.py path/to/disc.iso")
        return False

    print(f"[*] Opening ISO: {iso_path}...")
    with open(iso_path, 'rb') as f:
        entries = read_root_entries(f)

    if not entries:
        return False

    target = None
    for entry in entries:
        if entry['name'].upper() == ELF_NAME.upper():
            target = entry
            break

    if target is None:
        print(f"[-] Could not locate '{ELF_NAME}' in the ISO root directory.")
        print("    This is not the PAL (Europe/Australia) release this recomp targets.")
        print("    Entries found:")
        for entry in entries:
            print(f"      {entry['name']}  (LBA={entry['loc']}, size={entry['size']})")
        return False

    print(f"[+] Found '{ELF_NAME}' at LBA={target['loc']}, size={target['size']}")

    with open(iso_path, 'rb') as f:
        f.seek(target['loc'] * SECTOR)
        data = f.read(target['size'])

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, 'wb') as out:
        out.write(data)

    print(f"[+] Success! Extracted '{ELF_NAME}' ({len(data)} bytes) to {out_path}")
    return True


if __name__ == "__main__":
    src = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else DEFAULT_ISO
    ok = extract_elf(src, GAME_DIR / ELF_NAME)
    sys.exit(0 if ok else 1)
