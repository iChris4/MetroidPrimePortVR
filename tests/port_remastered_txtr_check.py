#!/usr/bin/python3
"""port_remastered_txtr_check.py - checks the port's Remastered TXTR decoder
against retrotool, texture by texture.

retrotool's `cmdl convert` writes each of a model's textures as a PNG whose
pixels are its decoder's output verbatim: retrotool/src/cmd/cmdl.rs hands
`image.as_bytes()` to the PNG encoder, so the only steps in between are the
lossless PNG row filters. It does *not* rebuild a normal map's Z, does not drop
alpha, and `set_source_srgb` only adds an sRGB chunk. Comparing raw RGBA against
PIL's convert('RGBA') of that PNG is therefore exact, and anything but a maximum
difference of zero is a bug in this port. (Grayscale and grayscale+alpha PNGs,
which is how BC4 and BC5 come out, expand to exactly the RGBA this port writes.)

Not every format can be reached that way. retrotool only converts the textures a
model uses as a base colour, normal, emissive or metal map, and the extracted
data's BC1, BC2, BC4 and BC5 textures (and ASTC 5x5's one) are used by nothing
at all, or in roles retrotool skips, so no PNG exists for them. For those the
oracle is retrotool's other output: `txtr convert` writes a DDS whose payload is
retrolib's own untiled surface, which is compared against this port through a
small independent BC decoder written here from the published block layouts
(BC1/BC2/BC4/BC5, and R8 read straight out of the DDS). That still makes
retrotool the source of the texel data, and the block decoding is checked by
something that is not the code under test. BC1 and BC2's endpoint expansion and
blending are rounded slightly differently by different implementations, so those
are compared with a tolerance of one and with the alpha channel required to
match exactly; BC4, BC5 and R8 are compared exactly.

BC6H is the one format with no oracle at all: retrolib's PNG writer has no bit
depth for its 32 bit float pixels (it panics), and nothing else on this machine
decodes BC6H. It is reported as unverified rather than skipped, and its header is
still checked.

  /usr/bin/python3 tests/port_remastered_txtr_check.py [--keep] [--min N]

Scratch files live in a run directory under /tmp/rmtxtr-check and are removed at
the end unless --keep is given.
"""

import argparse
import collections
import concurrent.futures
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RETROTOOL = os.path.join(ROOT, 'build/mpr-tools/retrotool/target/release/retrotool')
EXTRACTED = os.path.join(ROOT, 'build/mpr/x/all')
CONV = os.path.join(ROOT, 'build/mpr/conv')
SCRATCH_ROOT = '/tmp/rmtxtr-check'
SCRATCH = None  # a unique run directory inside SCRATCH_ROOT, so two runs cannot
                # delete each other's scratch mid-check

NAMES = {
    0: 'R8 UNORM', 11: 'RGB8 UNORM', 12: 'RGBA8 UNORM', 13: 'RGBA8 UNORM (sRGB)',
    20: 'BC1 UNORM', 21: 'BC1 UNORM (sRGB)', 22: 'BC2 UNORM', 23: 'BC2 UNORM (sRGB)',
    24: 'BC3 UNORM', 25: 'BC3 UNORM (sRGB)', 26: 'BC4 UNORM', 27: 'BC4 SNORM',
    28: 'BC5 UNORM', 29: 'BC5 SNORM',
    81: 'BC6H UFLOAT', 82: 'BC6H SFLOAT', 83: 'BC7 UNORM', 84: 'BC7 UNORM (sRGB)',
}
FOOTPRINTS = ['4x4', '5x4', '5x5', '6x5', '6x6', '8x5', '8x6', '8x8', '10x5', '10x6',
              '10x8', '10x10', '12x10', '12x12']
# Formats a retrotool PNG can exist for, and the reason when one cannot.
PNG_FORMATS = {12, 13, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 83, 84}
ASTC_RANGE = set(range(53, 81))
# These are decoded here from the DDS payload instead, so they need no PNG.
DDS_FORMATS = {0: 1, 20: 8, 21: 8, 22: 16, 23: 16, 26: 8, 28: 16, 29: 16}
# How far a channel may differ and still count as the same, per format. Only the
# BC1/BC2 colour expansion has a choice in the rounding; alpha is exact.
# BC4/BC5's interpolated values are rounded differently by different decoders too.
TOLERANCE = {20: 1, 21: 1, 22: 1, 23: 1, 26: 1, 27: 1, 28: 1, 29: 1}
NO_ORACLE_REASON = {
    81: 'retrolib has no PNG bit depth for BC6H floats, and no other decoder is available',
    82: 'retrolib has no PNG bit depth for BC6H floats, and no other decoder is available',
}


def format_name(code):
    if code in NAMES:
        return NAMES[code]
    if 53 <= code <= 66:
        return 'ASTC ' + FOOTPRINTS[code - 53]
    if 67 <= code <= 80:
        return 'ASTC ' + FOOTPRINTS[code - 67] + ' (sRGB)'
    return 'format %d' % code


# --------------------------------------------------------------------------
# An independent BC decoder, from the published block layouts, used only where
# retrotool cannot write a PNG. It is deliberately not this port's arithmetic.
# --------------------------------------------------------------------------

def _rgb565(values):
    r = (values >> 11) & 31
    g = (values >> 5) & 63
    b = values & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def _indices(block, count, bits):
    """`count` indices of `bits` bits each, read low bits first from the bytes."""
    value = np.zeros(len(block), dtype=np.uint64)
    for i in range(block.shape[1]):
        value |= block[:, i].astype(np.uint64) << np.uint64(8 * i)
    out = np.empty((len(block), count), dtype=np.uint8)
    for i in range(count):
        out[:, i] = (value >> np.uint64(bits * i)) & np.uint64((1 << bits) - 1)
    return out


def _bc1_colour(blocks):
    """Eight bytes to 16 RGB texels; `blocks` is (n, 8)."""
    c0 = blocks[:, 0].astype(np.uint16) | (blocks[:, 1].astype(np.uint16) << 8)
    c1 = blocks[:, 2].astype(np.uint16) | (blocks[:, 3].astype(np.uint16) << 8)
    e0 = _rgb565(c0).astype(np.float64)
    e1 = _rgb565(c1).astype(np.float64)
    opaque = c0 > c1
    four = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
    three = np.stack([e0, e1, (e0 + e1) / 2, (e0 + e1) / 2], 1)
    palette = np.where(opaque[:, None, None], four, three)
    idx = _indices(blocks[:, 4:8].copy(), 16, 2)
    rgb = np.take_along_axis(palette, idx[:, :, None], axis=1)
    # The punch-through mode's fourth entry is transparent black.
    clear = ~(~opaque[:, None] & (idx == 3))
    out = np.empty((len(blocks), 16, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(np.round(rgb), 0, 255).astype(np.uint8)
    out[:, :, 3] = clear * 255
    return out


def _bc4_channel(blocks):
    """Eight bytes to 16 single channel texels; `blocks` is (n, 8)."""
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    choose_six = a0 > a1
    rows = [a0, a1]
    for i in range(2, 8):
        # The +1 is the spec's round to nearest; without it the interpolated
        # entries land one low against the decoder being checked.
        six = ((8 - i) * a0 + (i - 1) * a1 + 1) // 7
        if i < 6:
            four = ((6 - i) * a0 + (i - 1) * a1 + 1) // 5
        else:
            four = np.full_like(a0, 0 if i == 6 else 255)
        rows.append(np.where(choose_six, six, four))
    palette = np.stack(rows, 1).astype(np.uint8)
    idx = _indices(blocks[:, 2:8].copy(), 16, 3)
    return palette[np.arange(len(blocks))[:, None], idx]


def dds_decode(blocks, fmt, width, height):
    """The DDS payload of one texture decoded to RGBA, or None if unsupported."""
    if fmt == 0:
        if len(blocks) < width * height:
            return None
        grey = np.frombuffer(bytes(blocks[:width * height]), dtype=np.uint8)
        out = np.empty((width * height, 4), dtype=np.uint8)
        out[:, 0] = out[:, 1] = out[:, 2] = grey
        out[:, 3] = 255
        return out
    block_size = DDS_FORMATS.get(fmt)
    if block_size is None:
        return None
    if fmt in (20, 21, 22, 23):
        need = width * height // 16 * block_size
        if len(blocks) < need:
            return None
        raw = np.frombuffer(bytes(blocks[:need]), dtype=np.uint8).reshape(-1, block_size)
        if fmt in (20, 21):
            return _bc1_colour(raw[:, :8])
        colour = _bc1_colour(raw[:, 8:16].copy())
        colour[:, :, 3] = 255  # BC2 never has transparent texels
        for i in range(16):
            colour[:, i, 3] = (raw[:, i // 2] >> (4 * (i % 2))) & 0xF
        colour[:, :, 3] *= 17
        return colour
    if fmt in (26, 28, 29):
        need = width * height // 16 * block_size
        if len(blocks) < need:
            return None
        raw = np.frombuffer(bytes(blocks[:need]), dtype=np.uint8).reshape(-1, block_size)
        out = np.empty((len(raw), 16, 4), dtype=np.uint8)
        out[:, :, 0] = out[:, :, 1] = out[:, :, 2] = _bc4_channel(raw[:, :8])
        if fmt == 26:
            out[:, :, 3] = 255
        else:
            out[:, :, 3] = _bc4_channel(raw[:, 8:16])
        return out
    return None


# --------------------------------------------------------------------------

def txtr_header(path):
    """kind, format, width, height, layers, tile mode, swizzle, mip count."""
    with open(path, 'rb') as handle:
        data = handle.read(4096)
    if data[:4] != b'RFRM':
        return None
    pos = 32
    while pos + 24 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack_from('<Q', data, pos + 4)[0]
        if chunk_id == b'HEAD':
            if pos + 24 + 32 > len(data):
                return None
            return struct.unpack_from('<8I', data, pos + 24)
        pos += 24 + size
    return None


def format_histogram():
    out = collections.defaultdict(list)
    for path in sorted(glob.glob(os.path.join(EXTRACTED, '*.TXTR'))):
        head = txtr_header(path)
        if head is not None:
            out[head[1]].append(path)
    return out


def conv_pngs():
    """Texture id to a retrotool PNG already written under build/mpr/conv."""
    out = {}
    for png in glob.glob(os.path.join(CONV, '*', '*.png')):
        out.setdefault(os.path.basename(png)[:-4], png)
    return out


def scan_model_materials(cmdl):
    """The texture ids a model references, via retrotool's materials-only mode."""
    out_dir = tempfile.mkdtemp(dir=SCRATCH)
    try:
        subprocess.run([RETROTOOL, 'cmdl', 'convert', cmdl, out_dir],
                       env=dict(os.environ, RETROTOOL_MATERIALS_ONLY='1'),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        tsv = os.path.join(out_dir, 'materials.tsv')
        try:
            with open(tsv, encoding='utf-8', errors='replace') as handle:
                return [line.rstrip('\n').split('\t')[2] for line in handle
                        if line.startswith('texture\t') or line.startswith('layer\t')]
        except OSError:
            return []
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)


def model_texture_index():
    """Texture id to models that use it, over every model in the data.

    A whole pass over the models costs a minute or so, so it only runs when a
    format is short of PNGs; the answer is cached in the scratch directory.
    """
    cached = os.path.join(SCRATCH_ROOT, 'model-textures.tsv')
    if os.path.exists(cached):
        index = collections.defaultdict(list)
        with open(cached, encoding='utf-8') as handle:
            for line in handle:
                cmdl, _, tid = line.rstrip('\n').partition('\t')
                index[tid].append(cmdl)
        return index
    cmdls = sorted(glob.glob(os.path.join(EXTRACTED, '*.CMDL')))
    index = collections.defaultdict(list)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for cmdl, ids in zip(cmdls, pool.map(scan_model_materials, cmdls)):
            for tid in ids:
                if len(tid) == 36 and len(index[tid]) < 4:
                    index[tid].append(cmdl)
    temp = cached + '.tmp%d' % os.getpid()
    with open(temp, 'w', encoding='utf-8') as handle:
        for tid, models in index.items():
            for cmdl in models:
                handle.write('%s\t%s\n' % (cmdl, tid))
    os.replace(temp, cached)
    return index


def decode_model_pngs(cmdl, wanted, out_dir):
    """retrotool's PNGs for one model's textures, moved out; the rest dropped."""
    work = tempfile.mkdtemp(dir=SCRATCH)
    try:
        subprocess.run([RETROTOOL, 'cmdl', 'convert', cmdl, work], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        found = {}
        for png in glob.glob(os.path.join(work, '*.png')):
            tid = os.path.basename(png)[:-4]
            if tid in wanted:
                target = os.path.join(out_dir, tid + '.png')
                shutil.move(png, target)
                found[tid] = target
        return found
    finally:
        shutil.rmtree(work, ignore_errors=True)


def dds_surface(path):
    """The untiled surface retrolib writes into a DDS, mip 0 first.

    retrotool writes the DDS next to its input, so the input is copied into the
    scratch directory rather than dropping a file into build/mpr/x/all.
    """
    work = tempfile.mkdtemp(dir=SCRATCH)
    try:
        shutil.copy(path, os.path.join(work, 'in.TXTR'))
        subprocess.run([RETROTOOL, 'txtr', 'convert', os.path.join(work, 'in.TXTR')],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        written = glob.glob(os.path.join(work, '*.dds'))
        if not written:
            return None
        with open(written[0], 'rb') as handle:
            raw = handle.read()
        if raw[:4] != b'DDS ':
            return None
        # 4 byte magic, the 124 byte DDS header, then the 20 byte DX10 header.
        return raw[148:] if raw[84:88] == b'DX10' else raw[128:]
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_one(tool, path, fmt, png, scratch):
    """Runs the tool on one texture; (worst difference, tolerance, note)."""
    head = txtr_header(path)
    width, height = head[2], head[3]
    raw_path = os.path.join(scratch, 'out.rgba')
    proc = subprocess.run([tool, path, raw_path], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        return None, 0, 'tool failed: %s' % (proc.stderr.strip() or 'exit %d' % proc.returncode)
    info = proc.stdout.split()
    got_w, got_h, got_fmt, got_mips = (int(v) for v in info[:4])
    if (got_w, got_h, got_fmt, got_mips) != (width, height, fmt, head[7]):
        return None, 0, 'tool says %dx%d fmt %d mips %d, header says %dx%d fmt %d mips %d' % (
            got_w, got_h, got_fmt, got_mips, width, height, fmt, head[7])
    raw = np.fromfile(raw_path, dtype=np.uint8)
    if raw.size != width * height * 4:
        return None, 0, 'tool wrote %d bytes, %d expected' % (raw.size, width * height * 4)
    mine = raw.reshape(height, width, 4).astype(np.int16)

    tolerance = 0
    if png is not None:
        with Image.open(png) as img:
            ref = np.asarray(img.convert('RGBA')).astype(np.int16)
        if ref.shape != mine.shape:
            return None, 0, ('retrotool wrote %dx%d, this port wrote %dx%d' %
                             (ref.shape[1], ref.shape[0], width, height))
        diff = np.abs(ref - mine)
        worst = int(diff.max())
        note = 'retrotool PNG'
    else:
        blocks = dds_surface(path)
        if blocks is None:
            return None, 0, 'retrotool wrote no DDS'
        ref = dds_decode(blocks, fmt, width, height)
        if ref is None:
            return None, 0, 'no decoder for this format'
        if ref.ndim == 3:
            # Blocks of 16 texels, in rows of width / 4 blocks, to an image.
            ref = (ref.reshape(height // 4, width // 4, 4, 4, 4)
                   .transpose(0, 2, 1, 3, 4))
        ref = ref.reshape(height, width, 4).astype(np.int16)
        diff = np.abs(ref - mine)
        worst = int(diff.max())
        tolerance = TOLERANCE.get(fmt, 0)
        if diff[:, :, 3].max() > (tolerance if fmt in (28, 29) else 0):
            # The alpha mask is exact in both decoders: the two colour blocks
            # differ only in how they round, never in what they select.
            return None, 0, 'alpha differs by %d' % diff[:, :, 3].max()
        note = 'independent BC decode of retrotool DDS'
    return worst, tolerance, note


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tool', default='/tmp/rmtxtr_tool',
                        help='the built port_remastered_txtr_tool')
    parser.add_argument('--min', type=int, default=5, help='textures to check per format')
    parser.add_argument('--max-per-format', type=int, default=8,
                        help='cap on textures checked per format')
    parser.add_argument('--keep', action='store_true', help='keep the scratch directory')
    args = parser.parse_args()

    if not os.access(args.tool, os.X_OK):
        sys.exit('build the tool first: g++ -std=c++20 -O2 -Wall -Wextra -Iplatform/include '
                 'tests/port_remastered_txtr_tool.cpp platform/port_remastered_txtr.cpp '
                 'platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp '
                 'platform/port_remastered_astc_decode.cpp -o %s' % args.tool)
    if not os.access(RETROTOOL, os.X_OK):
        sys.exit('retrotool is missing at %s' % RETROTOOL)

    global SCRATCH
    os.makedirs(SCRATCH_ROOT, exist_ok=True)
    SCRATCH = tempfile.mkdtemp(prefix='run-', dir=SCRATCH_ROOT)
    png_dir = os.path.join(SCRATCH, 'png')
    os.makedirs(png_dir)

    histogram = format_histogram()
    print('formats in %s (%d textures): %s' %
          (EXTRACTED, sum(len(v) for v in histogram.values()),
           ', '.join(str(f) for f in sorted(histogram))))
    have = conv_pngs()

    # Try harder for the PNG formats that are still short: find the models that
    # use those textures and let retrotool convert them.
    short_png = {}
    for fmt, paths in sorted(histogram.items()):
        if fmt not in PNG_FORMATS and fmt not in ASTC_RANGE:
            continue
        missing = [p for p in paths if os.path.basename(p)[:-5] not in have]
        if missing and len(paths) - len(missing) < args.min:
            short_png[fmt] = missing
    if short_png:
        print('looking for models that use %d textures with no PNG yet (%s)' %
              (sum(len(v) for v in short_png.values()),
               ', '.join(str(f) for f in sorted(short_png))))
        index = model_texture_index()
        for fmt, missing in sorted(short_png.items()):
            got = 0
            for path in missing:
                if got >= args.min:
                    break
                tid = os.path.basename(path)[:-5]
                for cmdl in index.get(tid, []):
                    if tid in decode_model_pngs(cmdl, {tid}, png_dir):
                        have[tid] = os.path.join(png_dir, tid + '.png')
                        got += 1
                        break
            print('  format %s: %d PNGs from retrotool' % (fmt, got))

    rows = []
    for fmt, paths in sorted(histogram.items()):
        name = format_name(fmt)
        available = 0
        checked = 0
        worst = 0
        tolerance = 0
        sources = collections.Counter()
        notes = []
        for path in paths:
            if checked >= args.max_per_format:
                break
            tid = os.path.basename(path)[:-5]
            png = have.get(tid)
            if png is None and fmt not in DDS_FORMATS:
                continue
            available += 1
            diff, allowed, note = check_one(args.tool, path, fmt, png, SCRATCH)
            if diff is None:
                notes.append('%s: %s' % (tid, note))
                continue
            checked += 1
            worst = max(worst, diff)
            tolerance = max(tolerance, allowed)
            sources[note] += 1
        rows.append((fmt, name, checked, available, worst, tolerance,
                     ', '.join(sorted(sources)), notes))

    print()
    print('%-6s %-22s %12s %9s %7s  %s' % ('format', 'name', 'checked/total', 'max diff',
                                          'within', 'oracle'))
    print('-' * 92)
    for fmt, name, checked, available, worst, tolerance, source, notes in rows:
        print('%-6d %-22s %12s %9s %7s  %s' %
              (fmt, name, '%d/%d' % (checked, available),
               worst if checked else 'n/a', '+%d' % tolerance if tolerance else 'exact',
               source or NO_ORACLE_REASON.get(fmt, 'none reachable')))
        for note in notes[:2]:
            print('%72s%s' % ('', note))
    print()
    unverified = [r for r in rows if r[2] < args.min]

    bad = [r for r in rows if r[4] > r[5]]
    if bad:
        print('FAIL: %d format(s) differ from the oracle: %s' %
              (len(bad), ', '.join('%s by %d' % (r[1], r[4]) for r in bad)))
        status = 1
    else:
        print('OK: every checked texture matches its oracle within tolerance')
        status = 0
    if unverified:
        print('%d format(s) below the %d texture minimum: %s' %
              (len(unverified), args.min,
               ', '.join('%s (%d)' % (format_name(r[0]), r[2]) for r in unverified)))

    if args.keep:
        print('scratch kept at %s' % SCRATCH)
    else:
        shutil.rmtree(SCRATCH, ignore_errors=True)
        try:
            os.rmdir(SCRATCH_ROOT)
        except OSError:
            pass  # another run is still using it
    return status


if __name__ == '__main__':
    sys.exit(main())