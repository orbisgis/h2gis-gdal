#!/usr/bin/env bash
# End to end test of the driver with the GDAL command line tools: creates a
# database, writes, reads, runs SQL and opens it shared.
#
#   GDAL_DRIVER_PATH=<directory of ogr_H2GIS and of the H2GIS library> tests/smoke_test.sh
#
# The tools of the GDAL the plugin was built for must be in the PATH. Relative
# paths only: on Windows, the tools do not understand the paths of bash.
set -uo pipefail

failures=0
# check <name> <expected text, case insensitive> <output>
check() {
  local name="$1" expected="$2" output="$3"
  if printf '%s\n' "$output" | grep -qiF -- "$expected"; then
    echo "OK   $name"
  else
    echo "FAIL $name: expected '$expected' in:"
    printf '%s\n' "$output" | sed 's/^/     /'
    failures=$((failures + 1))
  fi
}

work=h2gis-smoke-test
rm -rf "$work"
mkdir "$work"
cd "$work" || exit 1
db=test.mv.db

check "driver registered" "H2GIS -vector-" "$(ogrinfo --formats 2>&1)"
check "missing database refused" "unable to open" "$(ogrinfo -ro missing.mv.db 2>&1)"

out=$(ogrinfo -oo CREATE_IF_MISSING=YES $db 2>&1)
check "database created" "successful" "$out"
check "H2GIS functions" "V (String) = 2." "$(ogrinfo -ro $db -sql "SELECT H2GISVERSION() AS V" 2>&1)"
check "H2 version in the metadata" "H2_VERSION=2." "$(ogrinfo -ro $db 2>&1)"

ogrinfo $db -sql "CREATE TABLE PTS(ID INT PRIMARY KEY, NAME VARCHAR, THE_GEOM GEOMETRY(POINT, 0))" > /dev/null 2>&1
ogrinfo $db -sql "INSERT INTO PTS VALUES (1, 'a', 'POINT(0 0)'), (2, 'b', 'POINT(10 0)'), (3, 'c', 'POINT(0 10)')" > /dev/null 2>&1
check "table created by SQL" "Feature Count: 3" "$(ogrinfo -ro -so $db PTS 2>&1)"
check "H2GIS function in a query" "N (Integer64) = 3" \
  "$(ogrinfo -ro $db -sql "SELECT COUNT(*) AS N FROM PTS WHERE ST_Area(ST_Buffer(THE_GEOM, 1)) > 3" 2>&1)"

ogrinfo $db -sql "ALTER TABLE PTS ADD COLUMN EXTRA INT" > /dev/null 2>&1
check "column added by SQL" "EXTRA: Integer" "$(ogrinfo -ro -so $db PTS 2>&1)"

check "spatial filter" "Feature Count: 1" "$(ogrinfo -ro -so -spat -1 -1 1 1 $db PTS 2>&1)"
check "attribute filter" "Feature Count: 2" "$(ogrinfo -ro -so -where "NAME <> 'a'" $db PTS 2>&1)"

printf 'NAME,WKT\nx,"LINESTRING(0 0,1 1)"\ny,"LINESTRING(1 1,2 3)"\n' > lines.csv
ogr2ogr -update -append $db lines.csv -nln LINES -nlt LINESTRING -oo GEOM_POSSIBLE_NAMES=WKT -oo KEEP_GEOM_COLUMNS=NO > /dev/null 2>&1
check "layer written by ogr2ogr" "Feature Count: 2" "$(ogrinfo -ro -so $db LINES 2>&1)"
check "geometry type" "Geometry: Line String" "$(ogrinfo -ro -so $db LINES 2>&1)"

ogr2ogr -f GeoJSON lines.geojson $db LINES > /dev/null 2>&1
# The JSON layout depends on the GDAL version
check "export" '"NAME":"y"' "$(tr -d ' ' < lines.geojson 2>&1)"

check "read-only refuses writes" "read-only" "$(ogrinfo -ro $db -sql "DELETE FROM PTS" 2>&1)"
check "shared opening (AUTO_SERVER)" "Feature Count: 3" "$(ogrinfo -ro -oo AUTO_SERVER=YES -so $db PTS 2>&1)"

cd ..
rm -rf "$work"
if [ "$failures" -gt 0 ]; then
  echo "$failures failure(s)"
  exit 1
fi
echo "All the checks pass"