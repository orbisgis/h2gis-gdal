"""
Installs the H2GIS driver for GDAL in QGIS, or for the GDAL of a Python
environment.

It chooses the zip of the system, the processor and the GDAL version, takes it
from a GitHub release of h2gis-gdal (or from a local zip, such as an artifact of
the CI), extracts the plugin and the H2GIS native library, and registers the
driver at once in the running GDAL.

In QGIS: Plugins > Python Console > Show Editor, open this file and run it
(set LOCAL_ZIP or RELEASE below first if needed). The driver can be used at
once to open a database by its path; GDAL_DRIVER_PATH is also added to the
environment of QGIS (Settings > Options > System > Environment), so that the
driver is loaded at the start of QGIS and the .mv.db files show in the
Browser after a restart.

Outside QGIS, with the Python of the GDAL to use:
    python install_h2gis_driver.py [--zip FILE] [--release TAG] [--dest DIR]
    python install_h2gis_driver.py --uninstall [--dest DIR]

To uninstall in QGIS, set UNINSTALL = True below and run it.

This code is part of the H2GIS project, LGPL 3.0.
"""

import argparse
import ctypes
import io
import json
import os
import platform
import shutil
import sys
import tempfile
import urllib.request
import zipfile

# Release of h2gis-gdal to download from: "latest" (the most recent one,
# pre-releases included), or a tag such as "v0.1.0"
RELEASE = "latest"
# A local zip to install instead of downloading: a zip of the release, or an
# artifact of the CI (a zip that contains it). None to download.
LOCAL_ZIP = None
# True to uninstall: remove the installed files and GDAL_DRIVER_PATH
UNINSTALL = False

REPOSITORY = "https://github.com/orbisgis/h2gis-gdal"


def gdal_version():
    """Major.minor version of the running GDAL, e.g. "3.4"."""
    from osgeo import gdal

    version = gdal.VersionInfo("RELEASE_NAME")
    return ".".join(version.split(".")[:2])


def zip_name(version):
    """Name of the zip for this system, processor and GDAL version."""
    machine = platform.machine().lower()
    conda = os.path.isdir(os.path.join(sys.prefix, "conda-meta"))
    if sys.platform.startswith("linux"):
        if machine not in ("x86_64", "amd64"):
            raise RuntimeError(f"No binary for Linux {machine}: build the driver ({REPOSITORY})")
        return f"ogr_H2GIS-linux-x86_64-gdal{version}.zip"
    if sys.platform == "win32":
        if conda:
            return f"ogr_H2GIS-conda-windows-gdal{version}.zip"
        return f"ogr_H2GIS-windows-osgeo4w-gdal{version}.zip"
    if sys.platform == "darwin":
        if not conda:
            raise RuntimeError(
                "The GDAL of the official QGIS application for macOS is not covered yet: "
                "install QGIS with conda-forge, or build the driver")
        if machine != "arm64":
            raise RuntimeError("No binary for macOS on Intel processors: build the driver")
        return f"ogr_H2GIS-conda-macos-arm64-gdal{version}.zip"
    raise RuntimeError(f"Unsupported system: {sys.platform}")


def plugin_file_name():
    return "ogr_H2GIS.dll" if sys.platform == "win32" else "ogr_H2GIS.so"


def qgis_application():
    """The running QgsApplication, or None outside QGIS."""
    try:
        from qgis.core import QgsApplication
    except ImportError:
        return None
    return QgsApplication.instance()


def base_directory():
    """The directory of the installations: the QGIS profile, or the user data."""
    if qgis_application() is not None:
        from qgis.core import QgsApplication

        return os.path.join(QgsApplication.qgisSettingsDirPath(), "h2gis")
    if sys.platform == "win32":
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "h2gis-gdal")
    return os.path.join(os.path.expanduser("~"), ".local", "share", "h2gis-gdal")


def default_destination(version):
    """A directory per GDAL version: a plugin only loads in its own version."""
    return os.path.join(base_directory(), f"gdal{version}")


def latest_release():
    """Tag of the most recent release, pre-releases included (GitHub API)."""
    api = REPOSITORY.replace("https://github.com/", "https://api.github.com/repos/") + "/releases?per_page=10"
    request = urllib.request.Request(api, headers={"Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(request) as response:
            releases = json.load(response)
    except Exception as error:
        raise RuntimeError(f"Cannot list the releases of {REPOSITORY}: {error}") from error
    for release in releases:
        if not release.get("draft"):
            return release["tag_name"]
    raise RuntimeError(f"No release published in {REPOSITORY}")


def read_zip(name, local_zip, release):
    """Bytes of the zip, from the local file or from the GitHub release."""
    if local_zip:
        with open(local_zip, "rb") as f:
            data = f.read()
        # An artifact of the CI is a zip that holds the zip of the release
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            inner = [n for n in archive.namelist() if n.endswith(".zip")]
            if inner:
                if name not in inner:
                    raise RuntimeError(f"{local_zip} holds {', '.join(inner)}, not {name}")
                return archive.read(name)
        return data
    if release == "latest":
        # releases/latest/download ignores the pre-releases: ask the API
        try:
            release = latest_release()
            print(f"Most recent release: {release}")
        except RuntimeError as error:
            print(f"{error}: trying the latest release that is not a pre-release")
    if release == "latest":
        url = f"{REPOSITORY}/releases/latest/download/{name}"
    else:
        url = f"{REPOSITORY}/releases/download/{release}/{name}"
    print(f"Downloading {url}")
    try:
        with urllib.request.urlopen(url) as response:
            return response.read()
    except Exception as error:
        raise RuntimeError(f"Cannot download {url}: {error}. No binary for this GDAL version, or a "
                           f"pre-release only (set RELEASE to its tag, e.g. v0.0.1)? "
                           f"See {REPOSITORY}/releases") from error


def extract(data, destination):
    """Extract the plugin and the library; return the path of the plugin."""
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = archive.namelist()
        if plugin_file_name() not in names:
            raise RuntimeError(f"No {plugin_file_name()} in the zip: {', '.join(names)}")
        os.makedirs(destination, exist_ok=True)
        # Extract next to it first: a library loaded by a running process
        # cannot be overwritten on Windows
        tmp = tempfile.mkdtemp(dir=destination)
        try:
            archive.extractall(tmp)
            for name in names:
                target = os.path.join(destination, name)
                try:
                    os.replace(os.path.join(tmp, name), target)
                except PermissionError as error:
                    raise RuntimeError(f"Cannot replace {target}, in use: restart QGIS "
                                       "and run the installation before opening an H2GIS database") from error
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return os.path.join(destination, plugin_file_name())


def register(plugin_path):
    """Register the driver in the running GDAL; False if it was already there."""
    from osgeo import gdal

    if gdal.GetDriverByName("H2GIS") is not None:
        return False
    ctypes.CDLL(plugin_path).RegisterOGRH2GIS()
    if gdal.GetDriverByName("H2GIS") is None:
        raise RuntimeError("GDAL refused the driver: see the GDAL messages (plugin for another GDAL version?)")
    return True


def qgis_environment():
    """The custom environment variables of QGIS, as "mode|NAME=VALUE" entries."""
    from qgis.core import QgsSettings

    entries = QgsSettings().value("qgis/customEnvVars", []) or []
    return [entries] if isinstance(entries, str) else list(entries)


def configure_qgis(destination):
    """Add GDAL_DRIVER_PATH to the environment of QGIS, for its next starts."""
    from qgis.core import QgsSettings

    entries = [e for e in qgis_environment() if "|GDAL_DRIVER_PATH=" not in e]
    entries.append(f"overwrite|GDAL_DRIVER_PATH={destination}")
    settings = QgsSettings()
    settings.setValue("qgis/customEnvVars", entries)
    settings.setValue("qgis/customEnvVarsUse", True)


def unconfigure_qgis(base):
    """Remove the GDAL_DRIVER_PATH of the installation from the environment of QGIS."""
    from qgis.core import QgsSettings

    entries = qgis_environment()
    kept = [e for e in entries
            if not ("|GDAL_DRIVER_PATH=" in e and os.path.normpath(e.split("=", 1)[1]).startswith(os.path.normpath(base)))]
    if len(kept) == len(entries):
        return False
    QgsSettings().setValue("qgis/customEnvVars", kept)
    return True


def install(local_zip=LOCAL_ZIP, release=RELEASE, destination=None):
    version = gdal_version()
    name = zip_name(version)
    destination = destination or default_destination(version)
    print(f"GDAL {version}, {sys.platform} {platform.machine()}: {name}")

    plugin = extract(read_zip(name, local_zip, release), destination)
    print(f"Installed in {destination}")

    if register(plugin):
        print("The H2GIS driver is registered: H2GIS databases (.mv.db) can be opened now")
    else:
        print("The H2GIS driver was already loaded: restart to use the new version")

    if qgis_application() is not None:
        configure_qgis(destination)
        print("GDAL_DRIVER_PATH added to the environment of QGIS (Settings > Options > System): "
              "restart QGIS to see the .mv.db files in the Browser")
    else:
        separator = ";" if sys.platform == "win32" else ":"
        print("To load the driver in other GDAL programs, set:")
        print(f"  GDAL_DRIVER_PATH={destination}" + separator + "...")


DRIVER_FILES = ("ogr_H2GIS.so", "ogr_H2GIS.dll", "h2gis.so", "h2gis.dll", "h2gis.dylib")


def remove_installation(directory):
    """
    Remove the files of the driver from a directory, and the directory if it
    is then empty: never the other files of a directory given by mistake.
    Return the number of files removed.
    """
    removed = 0
    for name in DRIVER_FILES:
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            try:
                os.remove(path)
            except PermissionError as error:
                # Windows: a library loaded by the running process cannot be deleted
                raise RuntimeError(f"Cannot delete {path}, in use: remove GDAL_DRIVER_PATH, restart, "
                                   "and run the uninstallation before opening an H2GIS database") from error
            removed += 1
    try:
        os.rmdir(directory)
    except OSError:
        pass  # not empty: other files, kept
    return removed


def uninstall(destination=None):
    """
    Remove the installed files (of all the GDAL versions, or of destination)
    and, in QGIS, the GDAL_DRIVER_PATH that points to them. A driver already
    loaded stays usable until the end of the process.
    """
    if destination:
        directories = [destination]
        target = destination
    else:
        target = base_directory()
        directories = [os.path.join(target, d) for d in (os.listdir(target) if os.path.isdir(target) else [])
                       if d.startswith("gdal")]
    removed = sum(remove_installation(d) for d in directories if os.path.isdir(d))
    if not destination and os.path.isdir(target):
        try:
            os.rmdir(target)
        except OSError:
            pass
    print(f"Removed {removed} file(s) from {target}" if removed else f"Nothing installed in {target}")

    if qgis_application() is not None:
        if unconfigure_qgis(target):
            print("GDAL_DRIVER_PATH removed from the environment of QGIS")
        print("Restart QGIS: the driver stays loaded until then")
    else:
        print("Remove the directory from GDAL_DRIVER_PATH if you added it")


def main():
    parser = argparse.ArgumentParser(description="Install or uninstall the H2GIS driver for GDAL")
    parser.add_argument("--zip", help="local zip to install (a zip of the release or a CI artifact)")
    parser.add_argument("--release", default=RELEASE, help='release of h2gis-gdal: "latest" or a tag')
    parser.add_argument("--dest", help="installation directory")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the installed files (all the GDAL versions, or --dest)")
    args = parser.parse_args()
    try:
        if args.uninstall:
            uninstall(args.dest)
        else:
            install(args.zip, args.release, args.dest)
    except RuntimeError as error:
        sys.exit(f"Error: {error}")


if qgis_application() is not None:
    # Run from the Python console or the script editor of QGIS
    try:
        uninstall() if UNINSTALL else install()
    except RuntimeError as error:
        print(f"Error: {error}")
elif __name__ == "__main__":
    main()