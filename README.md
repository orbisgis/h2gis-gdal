# H2GIS driver for GDAL/OGR

A GDAL/OGR vector driver for [H2GIS](https://www.h2gis.org) databases
(`.mv.db` files), built as a plugin outside of the GDAL distribution.
It uses the H2GIS native library (GraalVM native image / Mandrel).

## Requirements

- GDAL with its development files (headers and CMake config). The plugin must
  be built for the GDAL version it is loaded by: the GDAL C++ ABI changes
  between minor versions.
- The H2GIS native library, C API version 3 or later: `h2gis.so`,
  `h2gis.dylib` or `h2gis.dll`. It is loaded at run time and is not needed to
  build the driver. An older library is refused with a message.

## Binaries

The GitHub Actions workflow (`.github/workflows/build.yml`) builds the plugin
for two GDAL versions on all platforms, the latest stable one and the one of
the QGIS LTR, and on Linux also for the GDAL of the LTS distributions (the
versions are set at the top of the workflow). It attaches the binaries to a
GitHub release on a `v*` tag:

| Binary | For |
|---|---|
| `ogr_H2GIS-linux-x86_64-gdal3.4.so`, `3.8`, `3.10`, `3.12`, `3.13` | Any Linux distribution with this GDAL version (Ubuntu 22.04: 3.4, Ubuntu 24.04: 3.8, Debian 13: 3.10...), and conda-forge on Linux |
| `ogr_H2GIS-windows-osgeo4w-gdal*.dll` | QGIS and QGIS LTR for Windows (OSGeo4W, standalone installers) |
| `ogr_H2GIS-conda-macos-arm64-gdal*.so`, `ogr_H2GIS-conda-windows-gdal*.dll` | QGIS installed with conda-forge |

The Linux binaries are built against the GDAL headers of conda-forge, on an
old distribution: they only depend on `libgdal.so.<N>`, whose name is set by
GDAL and is the same in every distribution, and on an old glibc. The workflow
checks them in the GDAL of several distributions.

Take the binary of the GDAL version of your QGIS (*Help > About*), rename it
`ogr_H2GIS.so` (`ogr_H2GIS.dll` on Windows) and put it in the `GDAL_DRIVER_PATH`
directory, with the H2GIS native library next to it. A plugin built for
another GDAL version is refused with a message: for another GDAL version,
build the plugin (below), it compiles with GDAL 3.4 to 3.13.

The H2GIS native library does not depend on GDAL: one `h2gis.so` for Linux,
one `h2gis.dll` for Windows, one `h2gis.dylib` per macOS processor.

The official QGIS application for macOS ships its own GDAL build is not covered yet.

## Build

```sh
# Go to the root of the repository
cd ../../h2gis-gdal
# Delete the folder that contains the build of the h2gis-gdal lib
rm -rf build
# Prepare the compilation
# -S .        sources in the current directory (where CMakeLists.txt is)
# -B build    build files and result in build/, created if needed
# -DCMAKE_BUILD_TYPE=Release  optimized build, without debug information 
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# Run the compilation. It produces the ogr_H2GIS.so lib in build folder
cmake --build build
```

If CMake finds another GDAL than the one QGIS uses, give its prefix:
`-DCMAKE_PREFIX_PATH=$(gdal-config --prefix)`, or `$CONDA_PREFIX` in a conda
environment. This produces `build/ogr_H2GIS.so` (`ogr_H2GIS.dll` on Windows).

### Run H2GIS driver and tests from the root folder 
```sh
# Go to the root of the repository
# Download the H2GIS native library for Linux next to the plugin.
# H2GIS_NATIVE_URL: the GitHub release of H2GIS to download it from
#   (graalvm-snapshot: the latest build of master; or a version, e.g. .../download/v2.2.6).
# The script downloads the library (h2gis-linux-x86_64.so) and the SHA256SUMS of the
# release, checks the checksum, and saves the library as build/h2gis.so: the name and
# the place where the plugin looks for it.
H2GIS_NATIVE_URL=https://github.com/orbisgis/H2GIS/releases/download/graalvm-snapshot \
  bash .github/scripts/download-h2gis.sh h2gis-linux-x86_64.so build/h2gis.so
export GDAL_DRIVER_PATH=$PWD/build
ogrinfo -ro /tmp/migisdb.mv.db
bash tests/smoke_test.sh
```

## Auto install

GDAL loads the plugins of the directories listed in `GDAL_DRIVER_PATH`. The
native library is searched in the `H2GIS_LIBRARY` configuration option, then
next to the plugin, then in the default library path.

A python script is provided to automatically install the last release of the H2GIS GDAL driver.
To check the releases go to https://github.com/orbisgis/h2gis-gdal/releases

```sh
# Go to the scripts folder. Run one of the following commands.
# Note : You can also download and run this script alone.
python3 install_h2gis_driver.py                    # Install the most recent release
python3 install_h2gis_driver.py --release v0.0.1   # Install a specific release
python3 install_h2gis_driver.py --dest DIR         # Install into another directory
python3 install_h2gis_driver.py --uninstall        # Uninstall the driver
```

## Use

```sh
ogrinfo --formats | grep H2GIS # Check driver

# Create an empty database
ogrinfo -oo CREATE_IF_MISSING=YES empty.mv.db

# List tables
ogrinfo -ro mydb.mv.db 
# Import a file
ogr2ogr -f GeoJSON roads.geojson mydb.mv.db ROADS
# Update a table
ogr2ogr -update -append mydb.mv.db roads.geojson -nln ROADS
```

A database can also be opened with the `H2GIS:` prefix, without extension:
`ogrinfo H2GIS:/path/to/mydb`.

An existing database is opened without ever creating one: a typo in the path
gives an error, not an empty database. A database without the H2GIS functions
is refused, so an H2 database is never converted. To create an empty
database, use the open option `CREATE_IF_MISSING=YES`: it gets the H2GIS
functions (about 11 MB, mostly `SPATIAL_REF_SYS`), `USER` and `PASSWORD`
become its administrator, and its directory must exist.

### SQL

SQL statements are run by H2, with all the H2GIS functions, like the
PostgreSQL driver does with PostgreSQL:

```sh
ogrinfo -ro mydb.mv.db -sql "SELECT TYPE, COUNT(*) AS N FROM BUILDING GROUP BY TYPE"
ogr2ogr -f GPKG zones.gpkg mydb.mv.db -nln ZONES \
  -sql "SELECT ID, ST_Buffer(THE_GEOM, 10) AS ZONE FROM ROADS"
ogrinfo mydb.mv.db -sql "CREATE TABLE BIG AS SELECT * FROM BUILDING WHERE ST_Area(THE_GEOM) > 500"
```

- A query (`SELECT`, `WITH`, `VALUES`, `TABLE`, `CALL`, `EXPLAIN`, `SHOW`...)
  returns a read-only result layer, read by batches. Its FIDs are the row
  numbers; its geometry fields have any geometry type, and the SRS of the SRID
  of their first geometries. The attribute and spatial filters set on a result
  layer are evaluated by GDAL.
- Another statement returns no layer. It is refused when the database is
  opened read-only.
- The layers follow the schema changes made by SQL on the same dataset, like
  in the GeoPackage driver: the tables created by `CREATE TABLE` or
  `CREATE VIEW` become layers, the dropped ones are removed (their layer
  objects become invalid, as after `DeleteLayer()`), and the definition of a
  table changed by `ALTER TABLE` (columns added, dropped, renamed, geometry
  columns, spatial indexes) is updated in place. Only the tables named in the
  statement are read again, about 2 ms each; a script of several statements,
  a renamed table, `DROP ... CASCADE` or `DROP SCHEMA` read all the tables
  again (about 0.8 ms per table of the database).
- A dataset opened elsewhere (another `GDALOpen()`, a QGIS layer) keeps its
  view of the schema until it is reopened. The data changes (`INSERT`,
  `UPDATE`, `DELETE`) are seen at once by every dataset.
- With `-dialect OGRSQL` or `-dialect SQLITE`, the statement is run by GDAL
  on the layers, as with any other driver; GDAL turns `ALTER TABLE ADD COLUMN`
  and `DROP TABLE` into `CreateField()` and `DeleteLayer()`.

### Open options

| Option | Default | Description |
|---|---|---|
| `USER` | `sa` | User name |
| `PASSWORD` | empty | Password |
| `SCHEMA` | all | Schemas to list, as a SQL `LIKE` pattern (case sensitive) |
| `LIST_ALL_TABLES` | `YES` | Also list the tables without geometry |
| `CREATE_IF_MISSING` | `NO` | Create an empty H2GIS database when the file does not exist |
| `AUTO_SERVER` | `NO` | Share the database with other processes (H2 automatic mixed mode) |

### Sharing a database between processes

H2 locks the database file: a database opened by a process (QGIS, a Java
application) cannot be opened by another one. With `-oo AUTO_SERVER=YES`
(`;AUTO_SERVER=TRUE` in the JDBC URL of a Java application), the first process
that opens the database serves it to the others, over a local TCP port chosen
by H2 and written in the `.lock.db` file; when it closes the database, another
one takes over. Nothing has to be started: every process uses the option.

```sh
ogrinfo -ro -oo AUTO_SERVER=YES mydb.mv.db
ogr2ogr -update -append -doo AUTO_SERVER=YES mydb.mv.db roads.geojson -nln ROADS
```

### Layer creation options

| Option | Default | Description |
|---|---|---|
| `GEOMETRY_NAME` | `THE_GEOM` | Name of the geometry column |
| `FID` | `FID` | Name of the FID column (`BIGINT` identity primary key). <br/>A field of the data with this name is refused: give the FID column another name |
| `SPATIAL_INDEX` | `YES` | Create a spatial index, when the dataset is closed |
| `LAUNDER` | `YES` | Names of the table and of its columns in upper case letters, digits and `_` |
| `OVERWRITE` | `NO` | Replace an existing table of the same name (refused if a view depends on it) |


## License

LGPL 3.0