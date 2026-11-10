#!/bin/bash

# Verifies that a packaged Windows setup folder is self-contained: every DLL imported by a bundled
# .exe/.dll is either bundled in bin/ (Windows searches the executable's folder first, also for
# modules loaded at runtime such as gdk-pixbuf loaders and Lua modules) or ships with Windows 11.
# The check deliberately does not trust the build machine's System32: DLLs that only redistributables
# install there (Visual C++ runtime, DirectX extras, ...) are reported as missing.
#
# Usage: check-dependencies.sh <setup dir>

setup_dir="${1:?usage: $0 <setup dir>}"
bin_dir="$setup_dir/bin"
system_dir="${SYSTEMROOT:-C:/Windows}/System32"
system_dir="$(cygpath -u "$system_dir" 2>/dev/null || echo "$system_dir")"

declare -A bundled
while IFS= read -r name; do
    bundled["${name,,}"]=1
done < <(find "$bin_dir" -maxdepth 1 -type f -iname '*.dll' -printf '%f\n')

# DLLs present on CI runners and developer machines but not on a fresh Windows 11 installation
not_in_windows='^(msvcp[0-9].*|vcruntime.*|concrt.*|vcomp.*|msvcr[0-9].*|d3dx.*|d3dcompiler_4[0-6]\.dll|xinput1_[0-3]\.dll|xaudio2_[0-7]\.dll)$'

missing=0
while IFS= read -r -d '' module; do
    while IFS= read -r dll; do
        lower="${dll,,}"
        case "$lower" in
            api-ms-win-* | ext-ms-win-*) continue ;;
        esac
        [[ -n ${bundled[$lower]+x} ]] && continue
        if [[ ! $lower =~ $not_in_windows ]] && [[ -f "$system_dir/$dll" || -f "$system_dir/$lower" ]]; then
            continue
        fi
        echo "missing: $dll (imported by ${module#"$setup_dir"/})" >&2
        missing=1
    done < <(objdump -p "$module" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
done < <(find "$setup_dir" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

if [[ $missing -ne 0 ]]; then
    echo "The package in $setup_dir is not self-contained" >&2
    exit 1
fi
echo "All DLL dependencies of $setup_dir are bundled or part of Windows"
