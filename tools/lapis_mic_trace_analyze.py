#!/usr/bin/env python3
"""Summarize Lapis Android microphone regmap writes without audio samples."""
import argparse
import gzip
import re
from pathlib import Path

CODEC = '0-05'
AFE = '11050000.mt6878-afe-pcm'
REGS = {
    0x4f10: 'VUL9_BASE_MSB', 0x4f14: 'VUL9_BASE',
    0x4f18: 'VUL9_CUR_MSB', 0x4f1c: 'VUL9_CUR',
    0x4f20: 'VUL9_END_MSB', 0x4f24: 'VUL9_END',
    0x4f30: 'VUL9_CON0', 0x2500: 'CONN036', 0x2520: 'CONN037',
    0x2680: 'CONN048', 0x26a0: 'CONN049',
}
CODEC_REGS = {
    0x230c: 'AUD_TOP_CKPDN_CON0', 0x230f: 'AUD_TOP_CKPDN_CON1',
    0x2388: 'AFE_UL_DL_CON0', 0x238b: 'AFE_UL_SRC_CON1',
    0x238f: 'AUDIO_TOP_CON0', 0x23d3: 'AFE_AUD_PAD_TOP',
    0x23db: 'AFE_MIC_ARRAY_CFG0', 0x23de: 'AFE_MTKAIF_MUX_CFG0',
    0x2508: 'AUDENC_ANA_CON0', 0x2509: 'AUDENC_ANA_CON1',
    0x250a: 'AUDENC_ANA_CON2', 0x250b: 'AUDENC_ANA_CON3',
    0x2519: 'AUDENC_ANA_CON17', 0x251a: 'AUDENC_ANA_CON18',
}
EVENT = re.compile(r'\s(\d+\.\d+): regmap_reg_write: (\S+) reg=([0-9a-fA-F]+) val=([0-9a-fA-F]+)')


def analyze(path):
    opener = gzip.open if path.suffix == '.gz' else open
    base = end = None
    count = 0
    with opener(path, 'rt', encoding='utf-8', errors='replace') as f:
        for line in f:
            match = EVENT.search(line)
            if not match:
                continue
            timestamp, dev, reg, value = match.groups()
            reg, value = int(reg, 16), int(value, 16)
            names = REGS if dev == AFE else CODEC_REGS if dev == CODEC else {}
            if reg not in names:
                continue
            print(f'{timestamp} {dev} {names[reg]} [{reg:#06x}] = {value:#x}')
            count += 1
            if dev == AFE and reg == 0x4f14:
                base = value
            elif dev == AFE and reg == 0x4f24:
                end = value
    print(f'\nSelected writes: {count}')
    if base is not None and end is not None and base <= end:
        speaker_start, speaker_end = 0x11060000, 0x11060000 + 48 * 4 * 42
        overlap = max(0, min(end + 1, speaker_end) - max(base, speaker_start))
        print(f'Last observed VUL9 range: {base:#x}..{end:#x} inclusive')
        print(f'Existing XNU speaker ring: {speaker_start:#x}..{speaker_end - 1:#x} inclusive')
        print(f'Observed overlap: {overlap} bytes; do not reuse overlapping regions concurrently')
    else:
        print('VUL9 range unavailable or incomplete; no overlap conclusion')
    return 0 if count else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path, help='trace text or .gz')
    args = parser.parse_args()
    raise SystemExit(analyze(args.trace))


if __name__ == '__main__':
    main()
