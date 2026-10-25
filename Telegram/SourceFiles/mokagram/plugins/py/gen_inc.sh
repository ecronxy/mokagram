#!/bin/sh
# Пересобирает tl_names.py и *.inc из .py (встраиваются в mokagram_plugins.cpp). Запускать после правки .py.
cd "$(dirname "$0")"
python3 -I gen_tl.py > tl_names.py || exit 1
for f in host:kHostScript base_plugin:kSdkScript sdk_api:kApiScript tl_names:kTlNamesScript tl:kTlScript fw:kFwScript; do
  src=${f%%:*}; var=${f##*:}
  { printf 'constexpr auto %s = R"PY(\n' "$var"; cat "$src.py"; printf ')PY";\n'; } > "$src.inc"
done
{ printf 'constexpr auto kTlSchema = R"JSON('; python3 -I gen_tl_schema.py || exit 1; printf ')JSON";\n'; } > tl_schema.inc
