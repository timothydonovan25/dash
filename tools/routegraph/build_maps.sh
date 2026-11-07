#!/usr/bin/env bash
# The dash's map files for a region, from a fresh Geofabrik download
# (docs/navigation.md §3): route/map.rgf (roads, for routing) and
# route/search.idx (addresses and places, for "Where to?").
#
#   tools/routegraph/build_maps.sh [region] [out_dir] [--bbox S W N E]
#   tools/routegraph/build_maps.sh north-america/us/arkansas /tmp/maps
#   tools/routegraph/build_maps.sh north-america/us/arkansas,north-america/us/missouri /tmp/maps
#
# Several regions, comma-separated, make one map (the overlap at a border
# is read once).
#
# Then copy out_dir/route/ to the card's root, or upload both files on the
# Wi-Fi page (Map). Needs pyosmium (pip install osmium; a venv is made in
# out_dir if it is missing) and build-host/rgf_build (cmake -S test -B
# build-host && cmake --build build-host --target rgf_build).
set -euo pipefail
region=${1:-north-america/us/arkansas}
out=${2:-maps}
shift $(( $# >= 2 ? 2 : $# ))
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rgf_build="$root/build-host/rgf_build"
[ -x "$rgf_build" ] || { echo "build $rgf_build first"; exit 1; }
mkdir -p "$out/route"
py=python3
if ! $py -c "import osmium" 2>/dev/null; then
    [ -x "$out/venv/bin/python" ] || { python3 -m venv "$out/venv" && "$out/venv/bin/pip" install -q osmium; }
    py="$out/venv/bin/python"
fi
pbfs=()
names=()
IFS=, read -ra regions <<< "$region"
for r in "${regions[@]}"; do
    n=$(basename "$r")
    names+=("$n")
    pbfs+=("$out/$n-latest.osm.pbf")
    [ -f "$out/$n-latest.osm.pbf" ] || curl -fSL -o "$out/$n-latest.osm.pbf" "https://download.geofabrik.de/$r-latest.osm.pbf"
done
name=$(IFS=-; echo "${names[*]}")
"$py" -I "$here/extract.py" "${pbfs[@]}" "$out/$name.rgi" "$@"
"$rgf_build" "$out/$name.rgi" "$out/route/map.rgf" --check 50 | tail -2
"$rgf_build" --search "$out/$name.sxi" "$out/route/search.idx"
rm -f "$out/$name.rgi" "$out/$name.sxi"
ls -la "$out/route"
