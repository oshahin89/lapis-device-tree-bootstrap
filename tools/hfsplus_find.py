#!/usr/bin/env python3
"""Read-only HFS+ lookup without mounting (no sudo): where a file's bytes are.

    tools/hfsplus_find.py IMAGE NAME [NAME...]   every file with that name:
        its path, size, and data-fork extents (allocation blocks)
    tools/hfsplus_find.py IMAGE --cat PATH OUT    copy a file's data fork out

Volume header at 1024 (Apple TN1150), the catalog B-tree's leaf records
(folder 1, file 2), the first 8 extents of each fork (enough for the files
this is used for: it refuses a fork with more). The image is never written.
"""
import struct, sys


class HFS:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(1024)
        vh = self.f.read(512)
        if vh[0:2] not in (b'H+', b'HX'):
            raise SystemExit('not HFS+')
        self.bs = struct.unpack_from('>I', vh, 40)[0]
        self.cat = self.fork(vh, 0x110)
        self.cat_size = struct.unpack_from('>Q', vh, 0x110)[0]
        node0 = self.read_fork(self.cat, 0, 512)
        self.node_size = struct.unpack_from('>H', node0, 14 + 18)[0]
        self.first_leaf = struct.unpack_from('>I', node0, 14 + 10)[0]

    @staticmethod
    def fork(buf, off):
        ext = []
        for i in range(8):
            start, count = struct.unpack_from('>II', buf, off + 16 + i * 8)
            if count:
                ext.append((start, count))
        return ext

    def read_fork(self, ext, offset, size):
        out = b''
        pos = 0
        for start, count in ext:
            length = count * self.bs
            if offset < pos + length and size > 0:
                inner = max(0, offset - pos)
                take = min(size, length - inner)
                self.f.seek(start * self.bs + inner)
                out += self.f.read(take)
                offset += take
                size -= take
            pos += length
        return out

    def records(self):
        node = self.first_leaf
        while node:
            buf = self.read_fork(self.cat, node * self.node_size, self.node_size)
            flink, = struct.unpack_from('>I', buf, 0)
            kind = struct.unpack_from('>b', buf, 8)[0]
            n, = struct.unpack_from('>H', buf, 10)
            if kind != -1:
                raise SystemExit('leaf chain broken')
            for i in range(n):
                off, = struct.unpack_from('>H', buf, self.node_size - 2 * (i + 1))
                klen, parent, nlen = struct.unpack_from('>HIH', buf, off)
                name = buf[off + 8:off + 8 + 2 * nlen].decode('utf-16-be', 'replace')
                rec = off + 2 + klen
                rec += rec & 1
                rtype, = struct.unpack_from('>h', buf, rec)
                yield parent, name, rtype, buf, rec
            node = flink

    def scan(self):
        folders, files = {}, []
        for parent, name, rtype, buf, rec in self.records():
            if rtype == 1:
                cnid, = struct.unpack_from('>I', buf, rec + 8)
                folders[cnid] = (parent, name)
            elif rtype == 2:
                cnid, = struct.unpack_from('>I', buf, rec + 8)
                size, = struct.unpack_from('>Q', buf, rec + 88)
                files.append((parent, name, cnid, size, self.fork(buf, rec + 88)))
        self.folders = folders
        return files

    def path(self, parent, name):
        parts = [name]
        while parent in self.folders and parent != 2:
            parent, n = self.folders[parent]
            parts.append(n)
        return '/' + '/'.join(reversed(parts))


def main():
    h = HFS(sys.argv[1])
    files = h.scan()
    if sys.argv[2] == '--cat':
        want, out = sys.argv[3], sys.argv[4]
        for parent, name, cnid, size, ext in files:
            if h.path(parent, name) == want:
                open(out, 'wb').write(h.read_fork(ext, 0, size))
                print(f'{want}: {size} bytes -> {out}')
                return
        raise SystemExit(f'{want}: not found')
    names = set(sys.argv[2:])
    for parent, name, cnid, size, ext in files:
        if name in names:
            alloc = sum(c for _, c in ext) * h.bs
            print(f'{h.path(parent, name)}  size {size}  allocated {alloc}  block size {h.bs}  extents {ext}')


main()
