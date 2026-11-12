#!/bin/bash

# Windows packaging script. This does the following:
# 1. Run "install" target from CMake into setup folder
# 2. Copy runtime dependencies into setup folder
# 3. Create version file and execute NSIS to create installer

if [ -z $1 ]; then
    build_dir="$(pwd)"
else
    build_dir="$(cd $1; pwd)"
fi
setup_dir_name="dist"  # Same as in the NSIS scripts
installer_name="xournalpp-setup.exe"
if [ "${XOURNALPP_FORK_BUILD:-0}" = "1" ]; then
    installer_name="xournal-studio-setup.exe"
fi
setup_dir="$build_dir/$setup_dir_name"
script_dir=$(dirname $(readlink -f "$0"))
echo "Installing to $setup_dir and making installer $build_dir/$installer_name"

prefix=${MSYSTEM_PREFIX:-/ucrt64}
echo "Set prefix to ${prefix}"

# delete old setup, if there
echo "clean dist folder"
rm -rf "$setup_dir"
rm -rf "$build_dir/$installer_name"

mkdir "$setup_dir"
mkdir "$setup_dir"/lib

echo "copy installed files"
(cd $build_dir && cmake --install . --prefix "$setup_dir")

# Fork: the libraries are copied below ("copy dependencies of all bundled modules"), from the imports of every bundled
# module. ldd ran xournalpp.exe under a debugger to list them: slow, and it starts the program.

echo "Installing GTK/Glib translations"
# Copy system locale files
for trans in "$build_dir"/po/*.gmo; do
    # Bail if there are no translations at all
    [ -f "$trans" ] || break;

    # Retrieve locale from name of translation file
    locale=$(basename -s .gmo $trans)
    locale_no_country=$(echo $locale | sed 's/_.*//')

    # GTK / GLib Translation
    for f in "glib20.mo" "gdk-pixbuf.mo" "gtk30.mo" "gtk30-properties.mo"; do
        install -Dvm644 "$prefix/share/locale/$locale/LC_MESSAGES/$f" "$setup_dir/share/locale/$locale/LC_MESSAGES/$f" \
          || ([ "$locale" != "$locale_no_country" ] \
              && install -Dvm644 "$prefix/share/locale/$locale_no_country/LC_MESSAGES/$f" "$setup_dir/share/locale/$locale_no_country/LC_MESSAGES/$f")
    done
done

echo "copy pixbuf libs"
cp -r "$prefix"/lib/gdk-pixbuf-2.0 "$setup_dir"/lib/

echo "copy pixbuf lib dependencies"
# These modules are loaded dynamically, so ldd on xournalpp.exe does not cover
# their dependencies. Follow indirect dependencies too (e.g. the SVG loader
# needs librsvg, which in turn needs libunwind on ARM64).
declare -A copied_pixbuf_dependencies
mapfile -d '' pixbuf_libraries < <(find "$prefix/lib/gdk-pixbuf-2.0" -type f -name "*.dll" -print0)
for ((i = 0; i < ${#pixbuf_libraries[@]}; ++i)); do
    while IFS= read -r dependency; do
        if [[ -n ${copied_pixbuf_dependencies[$dependency]+copied} ]]; then
            continue
        fi
        copied_pixbuf_dependencies[$dependency]=1
        # Only bundle libraries from MSYS2, not Windows system DLLs.
        if [[ -f "$prefix/bin/$dependency" ]]; then
            cp "$prefix/bin/$dependency" "$setup_dir/bin/" || exit 1
            pixbuf_libraries+=("$prefix/bin/$dependency")
        fi
    done < <(strings -a "${pixbuf_libraries[$i]}" | grep -Ei '^[[:alnum:]_+.-]+\.dll$' | sort -u)
done

echo "copy icons"
cp -r "$prefix"/share/icons "$setup_dir"/share/

echo "copy glib shared"
cp -r "$prefix"/share/glib-2.0 "$setup_dir"/share/

echo "copy poppler shared"
cp -r "$prefix"/share/poppler "$setup_dir"/share/

echo "copy fontconfig configuration"
# Fork: fontconfig looks for ../etc/fonts next to its DLL; without it, it has no font directories outside MSYS2
# ("Cannot load default config file"). The configuration points it at the Windows font folders.
mkdir -p "$setup_dir"/etc
cp -rL "$prefix"/etc/fonts "$setup_dir"/etc/

echo "copy gtksourceview shared"
cp -r "$prefix"/share/gtksourceview-4 "$setup_dir"/share

echo "copy gspawn-win64-helper"
cp "$prefix"/bin/gspawn-win64-helper{,-console}.exe "$setup_dir"/bin/

echo "copy gdbus"
cp "$prefix"/bin/gdbus.exe "$setup_dir"/bin

echo "copy gtk3-demo"
cp "$prefix"/bin/gtk3-demo.exe "$setup_dir"/bin

echo "copy lua-gobject and dependencies"
cp "$prefix"/bin/libgirepository-2.0-0.dll "$setup_dir"/bin
mkdir -p "$setup_dir"/lib/lua/5.5/LuaGObject
cp "$prefix"/lib/lua/5.5/LuaGObject/lua_gobject_core.dll "$setup_dir"/lib/lua/5.5/LuaGObject
cp "$prefix"/lib/libgirepository-2.0.dll.a "$setup_dir"/lib
mkdir "$setup_dir"/lib/girepository-1.0
cp "$prefix"/lib/girepository-1.0/*.typelib "$setup_dir"/lib/girepository-1.0
mkdir -p "$setup_dir"/share/lua/5.5
cp "$prefix"/share/lua/5.5/LuaGObject.lua "$setup_dir"/share/lua/5.5
cp -r "$prefix"/share/lua/5.5/LuaGObject/ "$setup_dir"/share/lua/5.5

echo "copy qpdf"
cp "$prefix"/bin/libqpdf*.dll "$setup_dir"/bin
cp "$prefix"/lib/libqpdf* "$setup_dir"/lib

echo "copy dependencies of all bundled modules"
# Fork: ldd above only covers xournalpp.exe and the loop above only the pixbuf loaders. The wrapper, the Lua module,
# the gspawn/gdbus helpers and their libraries have their own imports: follow the imports of every bundled .exe/.dll
# until all MSYS2 libraries are in bin/. Windows system DLLs are not in $prefix/bin and stay out.
declare -A scanned_modules
while true; do
    copied_any=0
    while IFS= read -r -d '' module; do
        [[ -n ${scanned_modules[$module]+x} ]] && continue
        scanned_modules[$module]=1
        while IFS= read -r dependency; do
            if [[ ! -e "$setup_dir/bin/$dependency" && -f "$prefix/bin/$dependency" ]]; then
                echo "  $dependency (for ${module#"$setup_dir"/})"
                cp "$prefix/bin/$dependency" "$setup_dir/bin/" || exit 1
                copied_any=1
            fi
        done < <(objdump -p "$module" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
    done < <(find "$setup_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
    [[ $copied_any -eq 0 ]] && break
done

# Fork: a local installation (build-local.ps1) runs on this machine and needs neither the check nor the installer
if [ "${XOURNALPP_LOCAL_INSTALL:-0}" = "1" ]; then
    echo "finished (local installation: no check for other machines, no installer)"
    exit 0
fi

echo "check that the package runs on a plain Windows installation"
bash "$script_dir/check-dependencies.sh" "$setup_dir" || exit 1

echo "create installer"
version=$(cat "$build_dir/VERSION" | sed '1!d')
makensis=$(command -v makensis.exe || command -v makensis || true)
if [ -z "$makensis" ]; then
    for candidate in \
        "$prefix/bin/makensis.exe" \
        "/c/Program Files (x86)/NSIS/makensis.exe" \
        "/c/Program Files (x86)/NSIS/Bin/makensis.exe"; do
        if [ -x "$candidate" ]; then
            makensis="$candidate"
            break
        fi
    done
fi
if [ -z "$makensis" ]; then
    echo "NSIS makensis.exe was not found on PATH or in the standard installation directories" >&2
    exit 1
fi

if [ "${XOURNALPP_FORK_BUILD:-0}" = "1" ]; then
    "$makensis" -NOCD \
        -DXOURNALPP_VERSION="$version" \
        -DSETUP_DIR="$setup_dir" \
        -DOUTPUT_INSTALLER_FILE="$build_dir/$installer_name" \
        -DSCRIPT_DIR="$script_dir" \
        "$script_dir/xournal-studio.nsi"
else
    "$makensis" -NOCD \
        -DXOURNALPP_VERSION="$version" \
        -DSETUP_DIR="$setup_dir" \
        -DOUTPUT_INSTALLER_FILE="$build_dir/$installer_name" \
        -DSCRIPT_DIR="$script_dir" \
        "$script_dir/xournalpp.nsi"
fi

echo "finished"

