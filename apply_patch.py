import os, sys, json, zlib, zipfile, shutil

REPO_DIR = os.path.dirname(os.path.abspath(__file__))

def xor_bytes(a: bytes, b: bytes) -> bytes:
    la, lb = len(a), len(b)
    m = max(la, lb)
    if m == 0:
        return b""
    a_pad = a + b"\x00" * (m - la)
    b_pad = b + b"\x00" * (m - lb)
    return (int.from_bytes(a_pad, "little") ^ int.from_bytes(b_pad, "little")).to_bytes(m, "little")

def main():
    if len(sys.argv) < 2:
        game_dir = input("Enter path to your Far Cry installation folder: ").strip().strip('"')
    else:
        game_dir = sys.argv[1].strip().strip('"')

    obj_pak = os.path.join(game_dir, "FCData", "Objects.pak")
    if not os.path.exists(obj_pak):
        print(f"ERROR: Could not find FCData\\Objects.pak in {game_dir}")
        sys.exit(1)

    with open(obj_pak, "rb") as f:
        bv_ref = f.read(15 * 1024 * 1024)

    with open(os.path.join(REPO_DIR, "deltas", "manifest.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)

    pak_cache = {}
    def get_pak(pak_rel):
        if pak_rel not in pak_cache:
            pak_cache[pak_rel] = zipfile.ZipFile(os.path.join(game_dir, pak_rel), "r")
        return pak_cache[pak_rel]

    entries = manifest["entries"]
    print(f"Applying Far Cry — Sheesh Mod ({manifest['version']}) — {len(entries)} records...")
    for i, e in enumerate(entries, 1):
        op = e["op"]
        dst = os.path.join(game_dir, e["target"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if op == "COPY_ASSET":
            src = os.path.join(REPO_DIR, e["asset_file"].replace("/", os.sep))
            shutil.copy2(src, dst)
        elif op == "EXTRACT_FROM_PAK":
            data = get_pak(e["source_pak"]).read(e["pak_entry"])
            with open(dst, "wb") as fo:
                fo.write(data)
        elif op == "DELTA_REF_OBJECTS_PAK":
            comp = open(os.path.join(REPO_DIR, e["delta_file"].replace("/", os.sep)), "rb").read()
            raw_xor = zlib.decompress(comp)
            out = xor_bytes(bv_ref[:e["size"]], raw_xor)[:e["size"]]
            with open(dst, "wb") as fo:
                fo.write(out)
        elif op == "DELTA_AGAINST_PAK_ENTRY":
            v_bytes = get_pak(e["source_pak"]).read(e["pak_entry"])
            comp = open(os.path.join(REPO_DIR, e["delta_file"].replace("/", os.sep)), "rb").read()
            raw_xor = zlib.decompress(comp)
            out = xor_bytes(v_bytes, raw_xor)[:e["size"]]
            with open(dst, "wb") as fo:
                fo.write(out)

    for zf in pak_cache.values():
        zf.close()
    print("SUCCESS! Far Cry — Sheesh Mod has been applied.")

if __name__ == "__main__":
    main()
