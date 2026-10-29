#!/usr/bin/env python3
"""Regenerate ../ca_bundle.h (root CA bundle for NetworkClientSecure::setCACertBundle).

Steps:
  1. Pick the roots listed in ROOTS from a PEM bundle (default: Git for Windows'
     mingw64 ca-bundle.crt) and save them to tools/ca_roots.pem.
  2. Run the arduino-esp32 core's tools/gen_crt_bundle.py on them.
  3. Convert its output to the format ESP-IDF 5.5 expects (see below) and write
     ca_bundle.h as a C array.

Why step 3: core 3.3.11's gen_crt_bundle.py still writes the legacy layout
  [u16 BE count] { [u16 BE name_len][u16 BE key_len][name][key] }...
but the IDF 5.5.x esp_crt_bundle_set() linked into the core only accepts
  [u32 LE offset of cert i]... { [u16 LE name_len][u16 LE key_len][name][key] }...
(offset table first; checked by esp_crt_check_bundle()). The core's own embedded
default bundle uses the new layout. A legacy blob is rejected and the core then
silently falls back to its built-in full bundle, so the firmware also checks
esp_crt_bundle_set() at boot and logs the result.

Usage (from the sketch folder or anywhere):
  python tools/make_ca_bundle.py
  python tools/make_ca_bundle.py --ca-file "C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
  python tools/make_ca_bundle.py --pem tools/ca_roots.pem      # reuse the saved roots
"""

import argparse
import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

ROOTS = [
    "ISRG Root X1",
    "ISRG Root X2",
    "DigiCert Global Root CA",
    "DigiCert Global Root G2",
    "USERTrust RSA Certification Authority",
    "USERTrust ECC Certification Authority",
]

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.dirname(HERE)
DEFAULT_CA_FILE = r"C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt"
DEFAULT_CORE_TOOL = os.path.join(
    os.environ.get("LOCALAPPDATA", ""), "Arduino15", "packages", "esp32", "hardware", "esp32", "3.3.11", "tools",
    "gen_crt_bundle.py")
IDF_MAX_CERTS = 200  # CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS in the core's sdkconfig


def extract_roots(ca_file):
    """Return {label: pem} for the ROOTS found in a '# Label' annotated PEM bundle."""
    found = {}
    label = None
    block = None
    with open(ca_file, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if block is not None:
                block.append(line)
                if line == "-----END CERTIFICATE-----":
                    if label in ROOTS:
                        if label in found:
                            raise SystemExit("duplicate root label in %s: %s" % (ca_file, label))
                        found[label] = "\n".join(block) + "\n"
                    block = None
                    label = None
                continue
            if line.startswith("# "):
                label = line[2:].strip()
            elif line == "-----BEGIN CERTIFICATE-----":
                block = [line]
    missing = [r for r in ROOTS if r not in found]
    if missing:
        raise SystemExit("roots not found in %s: %s" % (ca_file, ", ".join(missing)))
    return found


def pem_der(pem):
    body = "".join(l for l in pem.splitlines() if l and not l.startswith("-----"))
    return base64.b64decode(body)


def parse_legacy(blob):
    (count,) = struct.unpack_from(">H", blob, 0)
    pos = 2
    certs = []
    for _ in range(count):
        name_len, key_len = struct.unpack_from(">HH", blob, pos)
        pos += 4
        name = blob[pos:pos + name_len]
        pos += name_len
        key = blob[pos:pos + key_len]
        pos += key_len
        certs.append((name, key))
    if pos != len(blob):
        raise SystemExit("legacy bundle: %d trailing bytes" % (len(blob) - pos))
    return certs


def build_idf5(certs):
    table_len = 4 * len(certs)
    offsets = []
    body = b""
    for name, key in certs:
        offsets.append(table_len + len(body))
        body += struct.pack("<HH", len(name), len(key)) + name + key
    return struct.pack("<%dI" % len(offsets), *offsets) + body


def check_idf5(blob):
    """Mirror of esp_crt_check_bundle() in ESP-IDF 5.5 plus a full walk."""
    (first,) = struct.unpack_from("<I", blob, 0)
    if first == 0 or first % 4 != 0:
        raise SystemExit("idf5 bundle: bad offset table")
    count = first // 4
    if count > IDF_MAX_CERTS:
        raise SystemExit("idf5 bundle: too many certs")
    offsets = struct.unpack_from("<%dI" % count, blob, 0)
    for i in range(count):
        name_len, key_len = struct.unpack_from("<HH", blob, offsets[i])
        end = offsets[i] + 4 + name_len + key_len
        expected = offsets[i + 1] if i + 1 < count else len(blob)
        if end != expected:
            raise SystemExit("idf5 bundle: cert %d length mismatch" % i)
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--ca-file", default=None, help="annotated PEM bundle to pick ROOTS from (default: Git mingw64)")
    src.add_argument("--pem", default=None, help="use an existing PEM file with exactly the roots to embed")
    ap.add_argument("--core-tool", default=DEFAULT_CORE_TOOL, help="arduino-esp32 tools/gen_crt_bundle.py")
    ap.add_argument("--out", default=os.path.join(SKETCH, "ca_bundle.h"))
    args = ap.parse_args()

    roots_pem_path = os.path.join(HERE, "ca_roots.pem")
    if args.pem:
        roots_pem_path = os.path.abspath(args.pem)
        with open(roots_pem_path, "r", encoding="utf-8") as f:
            pem_text = f.read()
        labels = [l[2:].strip() for l in pem_text.splitlines() if l.startswith("# ")]
    else:
        ca_file = args.ca_file or DEFAULT_CA_FILE
        roots = extract_roots(ca_file)
        labels = list(ROOTS)
        pem_text = "".join("# %s\n%s\n" % (r, roots[r]) for r in ROOTS)
        with open(roots_pem_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(pem_text)
        print("wrote %s (%d roots from %s)" % (roots_pem_path, len(ROOTS), ca_file))

    # Fingerprints for the header comment.
    fps = []
    for block in pem_text.split("-----BEGIN CERTIFICATE-----")[1:]:
        pem = "-----BEGIN CERTIFICATE-----" + block.split("-----END CERTIFICATE-----")[0] + "-----END CERTIFICATE-----\n"
        fps.append(hashlib.sha256(pem_der(pem)).hexdigest())

    with tempfile.TemporaryDirectory() as tmp:
        tmp_pem = os.path.join(tmp, "roots.pem")
        with open(tmp_pem, "w", encoding="utf-8", newline="\n") as f:
            f.write(pem_text)
        subprocess.run([sys.executable, args.core_tool, "-q", "--input", tmp_pem], cwd=tmp, check=True)
        with open(os.path.join(tmp, "x509_crt_bundle"), "rb") as f:
            legacy = f.read()

    certs = parse_legacy(legacy)
    if len(certs) != len(fps):
        raise SystemExit("core tool produced %d certs, expected %d" % (len(certs), len(fps)))
    blob = build_idf5(certs)
    count = check_idf5(blob)

    lines = [
        "// Generated by tools/make_ca_bundle.py - do not edit by hand.",
        "// Regenerate: python tools/make_ca_bundle.py   (see README.md)",
        "//",
        "// ESP-IDF 5.x certificate bundle: u32 LE offset table, then per cert",
        "// u16 LE subject length, u16 LE key length, subject DER, SubjectPublicKeyInfo DER.",
        "// Sorted by subject (binary search on the ESP). Roots (SHA-256 of the certificate):",
    ]
    for label, fp in zip(labels, fps):
        lines.append("//   %-40s %s" % (label, fp))
    lines += [
        "",
        "#ifndef CA_BUNDLE_H_",
        "#define CA_BUNDLE_H_",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "constexpr size_t CA_BUNDLE_CERT_COUNT = %d;" % count,
        "",
        "static const uint8_t ca_bundle_bin[%d] = {" % len(blob),
    ]
    for i in range(0, len(blob), 16):
        lines.append("  " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ",")
    lines += ["};", "", "#endif", ""]

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d certs, %d bytes; legacy core output %d bytes)" % (args.out, count, len(blob), len(legacy)))


if __name__ == "__main__":
    main()
