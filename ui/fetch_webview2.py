#!/usr/bin/env python3
"""Fetch the WebView2 loader the Windows build embeds.

The bytes are Microsoft's redistributable loader, version 1.0.4314.35.
ui/webview2.LICENSE.txt is that loader's license. The file is not
committed. A Windows build writes it, and windres stores it in the
program.
"""

import hashlib
import io
import pathlib
import sys
import urllib.request
import zipfile

VER = "1.0.4314.35"
URL = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/" + VER
MEMBER = "runtimes/win-x64/native/WebView2Loader.dll"
SHA = "d3fce90bdeecb08596e377dea6a316354dd8949e32ff09b89d51be00c14e21b6"


def main(argv):
    if len(argv) != 1:
        sys.stderr.write("usage: fetch_webview2.py DEST\n")
        return 2
    dest = pathlib.Path(argv[0])
    if dest.is_file():
        have = hashlib.sha256(dest.read_bytes()).hexdigest()
        if have == SHA:
            return 0
    try:
        with urllib.request.urlopen(URL, timeout=60) as resp:
            package = resp.read()
    except OSError as exc:
        sys.stderr.write("grove: could not fetch the WebView2 loader: %s\n" % exc)
        return 1
    try:
        with zipfile.ZipFile(io.BytesIO(package)) as zipped:
            dll = zipped.read(MEMBER)
    except (zipfile.BadZipFile, KeyError, OSError) as exc:
        sys.stderr.write("grove: the WebView2 package has no loader: %s\n" % exc)
        return 1
    digest = hashlib.sha256(dll).hexdigest()
    if digest != SHA:
        sys.stderr.write("grove: WebView2 loader hash is %s\n" % digest)
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(dll)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
