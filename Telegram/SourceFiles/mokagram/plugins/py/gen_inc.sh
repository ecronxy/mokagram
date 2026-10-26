#!/bin/sh
# Пересобирает tl_names.py и *.inc из .py (встраиваются в mokagram_plugins.cpp). Запускать после правки .py.
# Встраиваем массивами байт, а не строковыми литералами: у MSVC предел длины литерала (16 КБ на кусок, 64 КБ целиком).
cd "$(dirname "$0")"
python3 -I gen_tl.py > tl_names.py || exit 1
python3 -I gen_tl_schema.py > tl_schema.json.tmp || exit 1
python3 -I - <<'PY' || exit 1
def emit(var, data, out):
    data = data + b"\0"
    with open(out, "w") as f:
        f.write("constexpr char %s[] = {\n" % var)
        for i in range(0, len(data), 24):
            f.write("\t" + ", ".join(str(b - 256 if b > 127 else b) for b in data[i:i + 24]) + ",\n")
        f.write("};\n")
for src, var in [("host", "kHostScript"), ("base_plugin", "kSdkScript"), ("sdk_api", "kApiScript"),
                 ("tl_names", "kTlNamesScript"), ("tl", "kTlScript"), ("fw", "kFwScript")]:
    emit(var, b"\n" + open(src + ".py", "rb").read(), src + ".inc")
emit("kTlSchema", open("tl_schema.json.tmp", "rb").read(), "tl_schema.inc")
PY
rm -f tl_schema.json.tmp
