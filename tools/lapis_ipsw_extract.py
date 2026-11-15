#!/usr/bin/env python3
"""Pull the parts the lapis port needs out of an iPhone X IPSW (read-only).

  lapis_ipsw_extract.py iPhone10,3_16.7.x_....ipsw OUTDIR

An IPSW is a zip with a BuildManifest.plist. This reads the manifest, picks the
restore (Erase) identity for D22AP, and extracts into OUTDIR:
  DeviceTree.im4p   -> for `lapis_mkadt.py --product`
  OS.dmg            -> the iOS root filesystem
  SystemOS.dmg      -> the SystemOS cryptex (dyld shared cache lives here)
  AppOS.dmg         -> the AppOS cryptex
then prints the next commands (lapis_mkadt.py --product, mkiosroot.sh).

Only reads the IPSW and writes those four files. No Apple material enters git;
OUTDIR is yours. Needs nothing but the Python standard library.
"""
import plistlib
import sys
import zipfile

# component name in the manifest -> output file
WANT = {
    "DeviceTree": "DeviceTree.im4p",
    "OS": "OS.dmg",
    "Cryptex1,SystemOS": "SystemOS.dmg",
    "Cryptex1,AppOS": "AppOS.dmg",
}
# older manifests name the cryptexes differently
ALIASES = {
    "SystemOS.dmg": ("Cryptex1,SystemOS", "SystemOS", "SystemVolume"),
    "AppOS.dmg": ("Cryptex1,AppOS", "AppOS", "AppVolume"),
}


def pick_identity(manifest):
    best = None
    for ident in manifest["BuildIdentities"]:
        info = ident.get("Info", {})
        dclass = (info.get("DeviceClass") or "").lower()
        variant = (info.get("Variant") or "")
        if "d22ap" not in dclass:
            continue
        # prefer an Erase (clean restore) identity over an Update one
        if "Erase" in variant or best is None:
            best = ident
        if "Erase" in variant:
            break
    return best


def path_for(ident, names):
    m = ident["Manifest"]
    for n in names:
        if n in m:
            p = m[n].get("Info", {}).get("Path")
            if p:
                return n, p
    return None, None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ipsw, out = sys.argv[1], sys.argv[2].rstrip("/")
    import os
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(ipsw) as z:
        manifest = plistlib.loads(z.read("BuildManifest.plist"))
        ver = manifest.get("ProductVersion", "?")
        ident = pick_identity(manifest)
        if ident is None:
            sys.exit("no D22AP (iPhone X) build identity in this IPSW")
        print(f"IPSW: iOS {ver}, identity {ident.get('Info', {}).get('DeviceClass')} "
              f"{ident.get('Info', {}).get('Variant', '')}")
        names = z.namelist()
        done = {}
        for outfile, aliases in {**{v: (k,) for k, v in WANT.items()}, **ALIASES}.items():
            if outfile in done:
                continue
            comp, path = path_for(ident, aliases)
            if not path:
                print(f"  (not in manifest: {outfile})")
                continue
            if path not in names:
                print(f"  (manifest path missing in zip: {path})")
                continue
            dst = os.path.join(out, outfile)
            print(f"  {comp:20} {path}  ->  {outfile} ({z.getinfo(path).file_size // (1<<20)} MiB)")
            with z.open(path) as src, open(dst, "wb") as f:
                while True:
                    b = src.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
            done[outfile] = dst
    print("\nNext:")
    print(f"  python3 tools/lapis_mkadt.py build/phone.adt --with-a78 \\")
    print(f"    --product {out}/DeviceTree.im4p")
    if all(os.path.exists(f"{out}/{x}") for x in ("OS.dmg", "SystemOS.dmg", "AppOS.dmg")):
        print(f"  sudo tools/ios-sweet/mkiosroot.sh {out}/OS.dmg {out}/SystemOS.dmg "
              f"{out}/AppOS.dmg build/iosroot.img 16G")
    else:
        print("  (some dmgs are missing; check the manifest component names)")


if __name__ == "__main__":
    main()
