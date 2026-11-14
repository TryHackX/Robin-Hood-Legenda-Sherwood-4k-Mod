"""Extract sprite frames from Robin Hood: The Legend of Sherwood sprite bank.

Format (see docs/SPRITE_BANK.md):
  robinhood.dic = u32 unknown, u16 nPal, nPal x {u16 nCodes, nCodes x 4 x u16 RGB565},
                  u32 nFrames, nFrames x {u16 w, u16 h, u32 offset, u32 size, u16 palIdx}
  robinhood.bks = raw frame data concatenated (offsets are absolute file offsets).
  palIdx == 0xFFFF -> 16-bit row RLE (u16 first, u16 last, pixels; FFFF FFFF = empty row)
  else             -> 4x1 vector-quantised: ceil(w/4) u16 codes per row, code -> 4 RGB565 px.
  0x07C0 = colour key (transparent); 0x001F in RLE frames = "replace colour" marker.

Usage (from project dir):
  python tools/hd/extract_sprites.py --info
  python tools/hd/extract_sprites.py --frame N --out file.png
  python tools/hd/extract_sprites.py --range A B --outdir dir [--rle-only|--vq-only]
Marker pixels (0x001F) are written transparent in the main PNG and as magenta in <name>_marker.png
(only when a frame has any). --keep-marker paints them magenta in the main PNG instead.
"""
import argparse, collections, os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.normpath(os.path.join(HERE, '..', '..', 'hd', 'work'))
KEY = 0x07C0
MARK = 0x001F


class Bank:
    def __init__(self, dic=os.path.join(WORK, 'robinhood.dic'), bks=os.path.join(WORK, 'robinhood.bks')):
        d = open(dic, 'rb').read()
        p = 0
        self.unk0, npal = struct.unpack_from('<IH', d, p); p += 6
        self.pals = []
        for _ in range(npal):
            n, = struct.unpack_from('<H', d, p); p += 2
            pal = np.frombuffer(d, '<u2', n * 4, p).reshape(n, 4).copy(); p += n * 8
            pal[pal == 0x07E0] = KEY            # same fix-up as 0x005069D0
            self.pals.append(pal)
        nfr, = struct.unpack_from('<I', d, p); p += 4
        self.frames = np.frombuffer(d, np.dtype([('w', '<u2'), ('h', '<u2'), ('off', '<u4'),
                                                ('size', '<u4'), ('pal', '<u2')]), nfr, p)
        assert p + 14 * nfr == len(d), 'trailing bytes in .dic'
        self.bks = np.memmap(bks, np.uint8, 'r')

    def raw(self, i):
        f = self.frames[i]
        return bytes(self.bks[int(f['off']):int(f['off']) + int(f['size'])])

    def decode16(self, i):
        """Return (h, w) uint16 RGB565 array, colour key where transparent."""
        f = self.frames[i]
        w, h, pal = int(f['w']), int(f['h']), int(f['pal'])
        data = np.frombuffer(self.raw(i), '<u2')
        if pal != 0xFFFF:
            cw = (w + 3) // 4
            codes = data[:cw * h].reshape(h, cw)
            img = self.pals[pal][codes].reshape(h, cw * 4)[:, :w]
            return img
        img = np.full((h, w), KEY, np.uint16)
        p = 0
        for y in range(h):
            first = int(data[p]); last = int(data[p + 1]); p += 2
            if last == 0xFFFF:              # empty row (first is FFFF or 0 in the data)
                continue
            if first == 0xFFFF or last < first or last >= w:
                raise ValueError('frame %d row %d: bad span %04x %04x' % (i, y, first, last))
            n = last - first + 1
            img[y, first:last + 1] = data[p:p + n]; p += n
        if p != len(data):
            raise ValueError('frame %d: consumed %d of %d words' % (i, p, len(data)))
        return img

    def kind(self, i):
        return 'rle16' if self.frames[i]['pal'] == 0xFFFF else 'vq'


def to_rgba(img, keep_marker=False):
    r = (img >> 11) & 31; g = (img >> 5) & 63; b = img & 31
    out = np.empty(img.shape + (4,), np.uint8)
    out[..., 0] = (r << 3) | (r >> 2)
    out[..., 1] = (g << 2) | (g >> 4)
    out[..., 2] = (b << 3) | (b >> 2)
    out[..., 3] = np.where(img == KEY, 0, 255)
    mark = img == MARK
    if keep_marker:
        out[mark] = (255, 0, 255, 255)
    else:
        out[mark] = (0, 0, 0, 0)
    return out, mark


def save(bank, i, path, keep_marker=False):
    img = bank.decode16(i)
    rgba, mark = to_rgba(img, keep_marker)
    Image.fromarray(rgba, 'RGBA').save(path)
    if mark.any():
        m = np.zeros(img.shape + (4,), np.uint8); m[mark] = (255, 0, 255, 255)
        Image.fromarray(m, 'RGBA').save(os.path.splitext(path)[0] + '_marker.png')
    return int(mark.sum())


def info(bank):
    f = bank.frames
    rle = f['pal'] == 0xFFFF
    print('frames %d  (rle16 %d, vq %d)  palettes %d  dic.u32[0]=%d' % (len(f), rle.sum(), (~rle).sum(), len(bank.pals), bank.unk0))
    print('palette sizes', collections.Counter(len(p) for p in bank.pals).most_common(5))
    print('bks size %d, last frame end %d' % (len(bank.bks), int(f['off'][-1]) + int(f['size'][-1])))
    for name, m in (('rle16', rle), ('vq', ~rle)):
        area = f['w'][m].astype(int) * f['h'][m]
        hist = np.histogram(np.maximum(f['w'][m], f['h'][m]), [0, 16, 32, 64, 128, 256, 512, 1024, 4096])[0]
        print('%-5s max(w,h) hist [<16,<32,<64,<128,<256,<512,<1024,<4096]: %s  total px %d' % (name, [int(x) for x in hist], area.sum()))
    # contiguous runs of each kind
    runs = []; s = 0
    for k in range(1, len(f) + 1):
        if k == len(f) or rle[k] != rle[s]:
            runs.append((s, k - 1, 'rle16' if rle[s] else 'vq')); s = k
    print('kind runs: %d, first 12: %s' % (len(runs), runs[:12]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--info', action='store_true')
    ap.add_argument('--frame', type=int)
    ap.add_argument('--out')
    ap.add_argument('--range', type=int, nargs=2)
    ap.add_argument('--outdir')
    ap.add_argument('--rle-only', action='store_true')
    ap.add_argument('--vq-only', action='store_true')
    ap.add_argument('--keep-marker', action='store_true')
    a = ap.parse_args()
    bank = Bank()
    if a.info:
        info(bank)
    if a.frame is not None:
        f = bank.frames[a.frame]
        n = save(bank, a.frame, a.out or 'frame_%06d.png' % a.frame, a.keep_marker)
        print('frame %d %dx%d %s pal=%d size=%d marker_px=%d' % (a.frame, f['w'], f['h'], bank.kind(a.frame), f['pal'], f['size'], n))
    if a.range:
        os.makedirs(a.outdir or '.', exist_ok=True)
        done = errs = 0
        for i in range(a.range[0], a.range[1] + 1):
            k = bank.kind(i)
            if (a.rle_only and k != 'rle16') or (a.vq_only and k != 'vq'):
                continue
            try:
                save(bank, i, os.path.join(a.outdir or '.', '%06d.png' % i), a.keep_marker); done += 1
            except Exception as e:
                errs += 1
                if errs < 5: print('ERR', e, file=sys.stderr)
        print('written %d, errors %d' % (done, errs))


if __name__ == '__main__':
    main()
