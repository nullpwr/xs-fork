#!/usr/bin/env python3
# Embed cacert.pem as a C byte array for runtime parsing by BearSSL.
import sys, os

inp = sys.argv[1] if len(sys.argv) > 1 else "src/tls/cacert.pem"
out = sys.argv[2] if len(sys.argv) > 2 else "src/tls/xs_ca_bundle.c"

with open(inp, "rb") as f:
    data = f.read()

with open(out, "w", newline="\n") as f:
    f.write("/* generated from " + os.path.basename(inp) + " by gen_ca_bundle.py; do not edit by hand */\n")
    f.write("#include <stddef.h>\n\n")
    f.write("const unsigned char xs_ca_pem[] = {\n")
    for i, b in enumerate(data):
        if i % 16 == 0:
            f.write("    ")
        f.write("0x%02x," % b)
        if i % 16 == 15:
            f.write("\n")
        else:
            f.write(" ")
    if len(data) % 16 != 0:
        f.write("\n")
    f.write("};\n\n")
    f.write("const size_t xs_ca_pem_len = %d;\n" % len(data))

print("wrote %s (%d bytes source -> %d entries)" % (out, len(data), len(data)))
