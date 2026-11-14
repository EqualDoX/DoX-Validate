#!/usr/bin/env bash
# Downloads and installs the pinned competitor tools for the benchmark into bench/.tools/ (gitignored),
# and builds the dox-validate binary used by the benchmark into build-bench/.
#
#   bench/setup_tools.sh            # JRE + veraPDF 1.30.4 + 1.30.2 + cpdf + VellumPdf + outloud + helper + dox-validate
#   bench/setup_tools.sh --no-build # skip the dox-validate build
#
# Every download is verified against a pinned SHA-256 before it is unpacked or executed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TOOLS="$HERE/.tools"
DL="$TOOLS/downloads"
mkdir -p "$DL"

# Eclipse Temurin 21 LTS JRE (HotSpot), linux x64.
JRE_VERSION="21.0.13+9"
JRE_URL="https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jre_x64_linux_hotspot_21.0.13_9.tar.gz"
JRE_SHA256="a535a1df9c6284d9618872984e026ae0352d1f7e9e0605425c0e6114f47351dd"
JRE_DIR="$TOOLS/jre-$JRE_VERSION"

# veraPDF greenfield (the only parser veraPDF 1.30 ships; the PDFBox build was last released for 1.28).
# 1.30.4: current stable release. 1.30.2: the release whose rule semantics dox-validate follows.
declare -A VERAPDF_SHA256=(
  [1.30.4]="04415ef3f149526a3048b9889723e81320ddf013bd891bcc7f0f297f3eadcc23"
  [1.30.2]="6cc6341cb1af644044054b81f00a6590a7918abb18f762243de115258bcad838"
)

fetch() {  # fetch URL FILE SHA256
  local url="$1" file="$2" sha="$3"
  if [[ ! -f "$file" ]] || ! echo "$sha  $file" | sha256sum -c --status; then
    echo "downloading $url"
    curl -fL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  echo "$sha  $file" | sha256sum -c --quiet || { echo "SHA-256 mismatch: $file" >&2; rm -f "$file"; exit 1; }
}

# --- JRE
JRE_TGZ="$DL/$(basename "${JRE_URL//%2B/+}")"
fetch "$JRE_URL" "$JRE_TGZ" "$JRE_SHA256"
if [[ ! -x "$JRE_DIR/bin/java" ]]; then
  rm -rf "$JRE_DIR" "$TOOLS/jre.tmp"
  mkdir -p "$TOOLS/jre.tmp"
  tar -xzf "$JRE_TGZ" -C "$TOOLS/jre.tmp"
  mv "$TOOLS/jre.tmp"/jdk-* "$JRE_DIR"
  rmdir "$TOOLS/jre.tmp"
fi
"$JRE_DIR/bin/java" -version

# --- veraPDF (unattended IzPack installation; default packs)
for v in 1.30.4 1.30.2; do
  zip="$DL/verapdf-greenfield-$v-installer.zip"
  fetch "https://software.verapdf.org/releases/1.30/verapdf-greenfield-$v-installer.zip" "$zip" "${VERAPDF_SHA256[$v]}"
  dest="$TOOLS/verapdf-$v"
  if [[ ! -x "$dest/verapdf" ]]; then
    tmp="$TOOLS/verapdf-$v.unpack"
    rm -rf "$tmp" "$dest"
    mkdir -p "$tmp"
    unzip -q "$zip" -d "$tmp"
    printf 'INSTALL_PATH=%s\n' "$dest" > "$tmp/auto-install.properties"
    "$JRE_DIR/bin/java" -jar "$tmp/verapdf-greenfield-$v/verapdf-izpack-installer-$v.jar" \
      -options "$tmp/auto-install.properties" > "$tmp/install.log" 2>&1 || { cat "$tmp/install.log" >&2; exit 1; }
    rm -rf "$tmp"
  fi
  JAVA_HOME="$JRE_DIR" "$dest/verapdf" --version | head -1
done

# --- cpdf (Coherent PDF command line tools, AGPL binaries; secondary competitor: partial PDF/UA-1 Matterhorn verifier)
CPDF_VERSION="2.9.3"
CPDF_URL="https://github.com/coherentgraphics/cpdf-binaries/releases/download/v$CPDF_VERSION/cpdf-binaries-master.zip"
CPDF_SHA256="ef31ac61f9952d8abc50b8b10a33895eb42169588cb5206c1f7ae38ff06c75ea"
CPDF_ZIP="$DL/cpdf-binaries-$CPDF_VERSION.zip"
fetch "$CPDF_URL" "$CPDF_ZIP" "$CPDF_SHA256"
if [[ ! -x "$TOOLS/cpdf-$CPDF_VERSION/cpdf" ]]; then
  rm -rf "$TOOLS/cpdf-$CPDF_VERSION" "$TOOLS/cpdf.unpack"
  mkdir -p "$TOOLS/cpdf.unpack" "$TOOLS/cpdf-$CPDF_VERSION"
  unzip -q "$CPDF_ZIP" -d "$TOOLS/cpdf.unpack"
  install -m 755 "$TOOLS/cpdf.unpack/cpdf-binaries-master/Linux-Intel-64bit/cpdf" "$TOOLS/cpdf-$CPDF_VERSION/cpdf"
  cp "$TOOLS/cpdf.unpack/cpdf-binaries-master/LICENSE.md" "$TOOLS/cpdf-$CPDF_VERSION/"
  rm -rf "$TOOLS/cpdf.unpack"
fi
"$TOOLS/cpdf-$CPDF_VERSION/cpdf" -version | head -1

# --- VellumPdf.Cli (vellum-preflight, Apache-2.0; PDF/UA-1 validator with its own engine): release binary, linux x64
VELLUM_VERSION="2.3.3"
VELLUM_URL="https://github.com/Tim81/VellumPDF/releases/download/v$VELLUM_VERSION/vellum-preflight-$VELLUM_VERSION-linux-x64"
VELLUM_SHA256="518e31d029109c839dac55b34e5c4deffc5bb1dfcbd6dc1698ca02c5046ac790"
fetch "$VELLUM_URL" "$DL/vellum-preflight-$VELLUM_VERSION" "$VELLUM_SHA256"
install -D -m 755 "$DL/vellum-preflight-$VELLUM_VERSION" "$TOOLS/vellum-$VELLUM_VERSION/vellum-preflight"
"$TOOLS/vellum-$VELLUM_VERSION/vellum-preflight" --version

# --- outloud (Apache-2.0; PDF/UA-1 conformance layer plus heuristic checks, Python): its own virtual environment
OUTLOUD_VERSION="0.2.1"
if [[ ! -x "$TOOLS/outloud-$OUTLOUD_VERSION/bin/outloud" ]]; then
  python3 -m venv "$TOOLS/outloud-$OUTLOUD_VERSION"
  "$TOOLS/outloud-$OUTLOUD_VERSION/bin/pip" install --quiet "outloud==$OUTLOUD_VERSION"
fi
"$TOOLS/outloud-$OUTLOUD_VERSION/bin/outloud" --version

# --- measurement helper (wall time, CPU time and peak RSS of one tool process; see bench/rusage_exec.c)
cc -O2 -Wall -o "$TOOLS/rusage_exec" "$HERE/rusage_exec.c"

# --- dox-validate (Release build of the checked-out commit, separate from the developer build tree)
if [[ "${1:-}" != "--no-build" ]]; then
  cmake -S "$ROOT" -B "$ROOT/build-bench" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
  ninja -C "$ROOT/build-bench" dox-validate
  "$ROOT/build-bench/dox-validate" --version
  git -C "$ROOT" rev-parse HEAD
fi
