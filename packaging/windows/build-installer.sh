#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 5 ]]; then
    printf 'Usage: %s VERSION WIN_BUILD_DIR QT_DIR VC_REDIST_DLL_DIR OUTPUT_DIR [DLL_DIR ...]\n' "$0" >&2
    exit 2
fi

version=$1
binary_dir=$(realpath "$2")
qt_dir=$(realpath "$3")
crt_dir=$(realpath "$4")
output_dir=$(realpath -m "$5")
shift 5

if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'Expected a numeric version with three components.\n' >&2
    exit 2
fi

project_dir=$(cd -- "$(dirname -- "$0")/../.." && pwd)
system_dir=$(wslpath -u 'C:\Windows\System32')
mkdir -p "$output_dir"
staging=$(mktemp -d "$output_dir/.meowlive-installer.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
mkdir -p "$staging/payload/platforms" "$staging/payload/config"
install -m 0644 "$binary_dir/meowlive-server.exe" "$binary_dir/meowlive-desktop.exe" "$staging/payload/"
install -m 0644 "$binary_dir/meowlive-update.exe" "$staging/payload/"
install -m 0644 "$qt_dir/plugins/platforms/qwindows.dll" "$staging/payload/platforms/"
install -m 0644 "$project_dir/config/server.example.toml" "$staging/payload/config/"
mkdir -p "$staging/payload/tools"
install -m 0644 "$project_dir/packaging/windows/import-rust-data.py" "$staging/payload/tools/"
mkdir -p "$staging/payload/migrations"
install -m 0644 "$project_dir"/adapters/storage/migrations/*.sql "$staging/payload/migrations/"
mkdir -p "$staging/payload/voice"
install -m 0644 "$project_dir"/packaging/windows/voice/*.py "$staging/payload/voice/"
install -m 0644 "$project_dir"/packaging/windows/voice/*.json "$staging/payload/voice/"

qt_translation="$qt_dir/translations/qtbase_zh_CN.qm"
if [[ -f $qt_translation ]]; then
    mkdir -p "$staging/payload/translations"
    install -m 0644 "$qt_translation" "$staging/payload/translations/"
fi

declare -A shipped=()
queue=("$staging/payload/meowlive-server.exe" "$staging/payload/meowlive-desktop.exe"
       "$staging/payload/platforms/qwindows.dll" "$staging/payload/meowlive-update.exe")
for ((index=0; index<${#queue[@]}; index++)); do
    input=${queue[index]}
    imports=$(llvm-readobj --file-headers --coff-imports "$input")
    if [[ $imports != *IMAGE_FILE_MACHINE_AMD64* ]]; then
        printf 'Expected x64 PE input: %s\n' "$input" >&2
        exit 1
    fi
    while IFS= read -r dll; do
        name=${dll,,}
        if [[ -n ${shipped[$name]:-} || $name == api-ms-* || $name == ext-ms-* ]]; then
            continue
        fi
        case "$name" in
            vcruntime*.dll|msvcp*.dll|concrt*.dll)
                search=("$crt_dir")
                ;;
            qt6*.dll)
                search=("$qt_dir/bin")
                ;;
            *)
                search=("$binary_dir" "$qt_dir/bin" "$@" "$system_dir")
                ;;
        esac
        dependency=''
        for directory in "${search[@]}"; do
            if [[ -d $directory ]]; then
                dependency=$(find "$directory" -maxdepth 1 -type f -iname "$dll" -print -quit)
                if [[ -n $dependency ]]; then
                    break
                fi
            fi
        done
        if [[ -z $dependency ]]; then
            printf 'Missing runtime dependency %s imported by %s\n' "$dll" "$input" >&2
            exit 1
        fi
        shipped[$name]=1
        if [[ $dependency != "$system_dir/"* ]]; then
            install -m 0644 "$dependency" "$staging/payload/$dll"
            queue+=("$staging/payload/$dll")
        fi
    done < <(awk '/^[[:space:]]+Name: .*\.[dD][lL][lL]$/ { print $2 }' <<< "$imports")
done

python3 - "$staging" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
payload = root / "payload"
files = sorted(path.relative_to(payload) for path in payload.rglob("*") if path.is_file())
folders = sorted({path.parent for path in files}, key=lambda path: (len(path.parts), str(path)))
with (root / "install-files.nsh").open("w", encoding="utf-8") as stream:
    for folder in folders:
        subdir = "\\" + str(folder).replace("/", "\\") if folder.parts else ""
        stream.write(f'  SetOutPath "$INSTDIR{subdir}"\n')
        for path in files:
            if path.parent == folder:
                stream.write(f'  File "payload/{path.as_posix()}"\n')
with (root / "uninstall-files.nsh").open("w", encoding="utf-8") as stream:
    for path in files:
        name = str(path).replace("/", "\\")
        stream.write(f'  Delete "$INSTDIR\\{name}"\n')
    for folder in reversed(folders):
        if folder.parts:
            name = str(folder).replace("/", "\\")
            stream.write(f'  RMDir "$INSTDIR\\{name}"\n')
PY

cp "$project_dir/packaging/windows/installer.nsi" "$staging/"
artifact="MeowLive2D-$version-windows-x64-setup.exe"
(
    cd "$staging"
    makensis -V2 "-DVERSION=$version" "-DARTIFACT=$artifact" installer.nsi
)
mv -- "$staging/$artifact" "$output_dir/$artifact"
(
    cd "$output_dir"
    sha256sum "$artifact" > "$artifact.sha256"
)
