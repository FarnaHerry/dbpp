#!/usr/bin/env bash
# install.sh — Install, upgrade, or uninstall db++ on Linux and macOS.
set -euo pipefail

DEFAULT_REPO="FarnaHerry/dbpp"
TEMP_DIR=""
MACOS_MOUNT_DIR=""

log() { printf '[dbpp] %s\n' "$*"; }
warn() { printf '[dbpp] warning: %s\n' "$*" >&2; }
die() { printf '[dbpp] error: %s\n' "$*" >&2; exit 1; }

cleanup() {
  if [[ -n "$MACOS_MOUNT_DIR" ]] && command -v hdiutil >/dev/null 2>&1; then
    hdiutil detach "$MACOS_MOUNT_DIR" -quiet >/dev/null 2>&1 || \
      hdiutil detach "$MACOS_MOUNT_DIR" -force -quiet >/dev/null 2>&1 || true
  fi
  if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" ]]; then
    rm -rf "$TEMP_DIR"
  fi
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage: install.sh [options]

Options:
  --version <ver>        Install a specific version (e.g. 0.4.7 or v0.4.7)
  --install-dir <dir>    Override install directory
                           Linux default: ~/.local/opt/dbpp
                           macOS default: /Applications (or ~/Applications)
  --bin-dir <dir>        Override symlink directory (default: ~/.local/bin)
  --download-base <url>  Override release download base URL (mirror or file://)
  --repo <owner/name>    GitHub repository (default: FarnaHerry/dbpp)
  --no-desktop           Skip Linux .desktop and icon installation
  --require-checksum     Fail if SHA256SUMS.txt is unavailable
  --skip-checksum        Skip SHA-256 checksum verification
  --uninstall            Uninstall db++ (preserves user data and ADBC drivers)
  -h, --help             Show this help message

Environment variables:
  DBPP_VERSION, DBPP_INSTALL_DIR, DBPP_BIN_DIR, DBPP_DOWNLOAD_BASE, DBPP_REPO
EOF
}

normalize_path() {
  local p="$1"
  while [[ "$p" != "/" && "$p" == */ ]]; do p="${p%/}"; done
  printf '%s' "$p"
}

resolve_canonical_dir() {
  local p="$1"
  if [[ -d "$p" ]]; then
    (cd "$p" 2>/dev/null && pwd -P) || normalize_path "$p"
  else
    normalize_path "$p"
  fi
}

detect_platform() {
  local os_raw arch_raw
  os_raw="$(uname -s)"
  arch_raw="$(uname -m)"
  case "$os_raw" in
    Linux)
      OS="linux"
      case "$arch_raw" in
        x86_64|amd64) ARCH="x86_64" ;;
        aarch64|arm64) ARCH="aarch64" ;;
        *) die "Unsupported Linux architecture: $arch_raw (supported: x86_64, aarch64)" ;;
      esac
      ;;
    Darwin)
      OS="macos"
      case "$arch_raw" in
        arm64|aarch64) ARCH="arm64" ;;
        x86_64) die "Intel macOS (x86_64) prebuilt binaries are not currently published; Apple Silicon (arm64) is supported." ;;
        *) die "Unsupported macOS architecture: $arch_raw" ;;
      esac
      ;;
    *) die "Unsupported operating system: $os_raw (use install.ps1 on Windows)" ;;
  esac
}

download_file() {
  local url="$1" dest="$2"
  if [[ "$url" == file://* ]]; then
    local local_path="${url#file://}"
    [[ -f "$local_path" ]] || return 1
    cp "$local_path" "$dest"
    return 0
  fi
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 --retry-delay 1 "$url" -o "$dest"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$dest" "$url"
  else
    die "Neither curl nor wget is available to download files."
  fi
}

resolve_latest_version() {
  local repo="$1"
  local latest_url="https://github.com/${repo}/releases/latest"
  local api_url="https://api.github.com/repos/${repo}/releases/latest"
  local effective="" tag=""

  if command -v curl >/dev/null 2>&1; then
    effective="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$latest_url" 2>/dev/null || true)"
  elif command -v wget >/dev/null 2>&1; then
    effective="$(wget --spider --max-redirect=0 "$latest_url" 2>&1 \
      | sed -n 's/^[Ll]ocation:[[:space:]]*\([^[:space:]]*\).*/\1/p' | head -n 1 || true)"
  else
    die "Neither curl nor wget is available."
  fi

  if [[ "$effective" == */releases/tag/* ]]; then
    tag="${effective##*/releases/tag/}"
    tag="${tag%%[?#]*}"
  fi
  if [[ -z "$tag" ]]; then
    local api_json=""
    if command -v curl >/dev/null 2>&1; then
      api_json="$(curl -fsSL "$api_url" 2>/dev/null || true)"
    else
      api_json="$(wget -qO- "$api_url" 2>/dev/null || true)"
    fi
    tag="$(printf '%s' "$api_json" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1 || true)"
  fi

  tag="${tag#v}"
  [[ -n "$tag" ]] || die "Failed to determine latest release version for ${repo}. Pass --version <ver> explicitly."
  printf '%s' "$tag"
}

compute_sha256() {
  local file="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$file" | awk '{print tolower($1)}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$file" | awk '{print tolower($1)}'
  else
    die "Neither sha256sum nor shasum is available for checksum verification."
  fi
}

verify_checksum() {
  local asset_path="$1" asset_name="$2" sums_url="$3" require_checksum="$4" skip_checksum="$5"
  if [[ "$skip_checksum" == "1" ]]; then
    warn "Skipping SHA-256 verification (--skip-checksum)."
    return 0
  fi

  local sums_file="$TEMP_DIR/SHA256SUMS.txt"
  if ! download_file "$sums_url" "$sums_file" 2>/dev/null; then
    [[ "$require_checksum" == "1" ]] && die "SHA256SUMS.txt could not be downloaded from ${sums_url}."
    warn "SHA256SUMS.txt not found for this release; skipping checksum verification."
    return 0
  fi

  local expected=""
  expected="$(awk -v target="$asset_name" '
    {
      fname = $2; sub(/^\*/, "", fname); sub(/^\.\//, "", fname)
      if (fname == target) { print tolower($1); exit }
    }
  ' "$sums_file")"
  [[ -n "$expected" ]] || die "Asset ${asset_name} is missing from SHA256SUMS.txt."

  local actual
  actual="$(compute_sha256 "$asset_path")"
  [[ "$actual" == "$expected" ]] || die "SHA-256 mismatch for ${asset_name}: expected ${expected}, got ${actual}"
  log "Verified SHA-256 checksum for ${asset_name}."
}

guard_linux_install_dir() {
  local install_dir="$1"
  local xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
  local data_dir target_dir
  data_dir="$(resolve_canonical_dir "${xdg_data}/dbpp")"
  target_dir="$(resolve_canonical_dir "$install_dir")"
  if [[ "$target_dir" == "$data_dir" || -f "${install_dir}/dbpp.sqlite3" ]]; then
    die "Refusing install_dir '${install_dir}': collides with db++ user data directory (${data_dir})."
  fi
}

refresh_linux_desktop_caches() {
  local xdg_data="$1"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "${xdg_data}/applications" >/dev/null 2>&1 || true
  command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -f -t "${xdg_data}/icons/hicolor" >/dev/null 2>&1 || true
}

install_linux_desktop() {
  local install_dir="$1" bin_dir="$2"
  local xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
  local apps_dir="${xdg_data}/applications"
  local icons_dir="${xdg_data}/icons/hicolor/256x256/apps"
  local desktop_src="${install_dir}/share/applications/dbpp.desktop"
  local icon_src="${install_dir}/share/icons/hicolor/256x256/apps/dbpp.png"

  mkdir -p "$apps_dir"
  if [[ -f "$desktop_src" ]]; then
    awk -v bin="$bin_dir" '{
      if ($0 ~ /^Exec=dbpp([[:space:]]|$)/) {
        sub(/^Exec=dbpp/, "Exec=" bin "/dbpp")
      }
      print
    }' "$desktop_src" > "${apps_dir}/dbpp.desktop"
  else
    cat > "${apps_dir}/dbpp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=db++
GenericName=Database Manager
Comment=ADBC 数据管理器
Exec=${bin_dir}/dbpp %f
Icon=dbpp
StartupWMClass=dbpp
Terminal=false
Categories=Development;Database;
EOF
  fi
  if [[ -f "$icon_src" ]]; then
    mkdir -p "$icons_dir"
    cp -f "$icon_src" "${icons_dir}/dbpp.png"
  fi
  refresh_linux_desktop_caches "$xdg_data"
}

atomic_replace_dir() {
  local staged_src="$1" target_dir="$2"
  local parent_dir
  parent_dir="$(dirname "$target_dir")"
  mkdir -p "$parent_dir"

  local sibling_new="${parent_dir}/.$(basename "$target_dir").new.$$"
  local sibling_old="${parent_dir}/.$(basename "$target_dir").old.$$"
  rm -rf "$sibling_new" "$sibling_old"
  cp -a "$staged_src" "$sibling_new"

  local had_existing=0
  if [[ -e "$target_dir" ]]; then
    had_existing=1
    mv "$target_dir" "$sibling_old"
  fi
  if ! mv "$sibling_new" "$target_dir"; then
    rm -rf "$sibling_new" || true
    [[ "$had_existing" -eq 1 && -e "$sibling_old" ]] && mv "$sibling_old" "$target_dir" || true
    die "Failed to move staged installation into ${target_dir}."
  fi
  [[ "$had_existing" -eq 1 ]] && rm -rf "$sibling_old" || true
}

print_path_hint_if_needed() {
  local bin_dir="$1"
  case ":${PATH:-}:" in
    *":${bin_dir}:"*) ;;
    *)
      warn "${bin_dir} is not in PATH. Add it to your shell profile:"
      printf '  export PATH="%s:$PATH"\n' "$bin_dir"
      ;;
  esac
}

install_linux() {
  local archive="$1" install_dir="$2" bin_dir="$3" no_desktop="$4"
  guard_linux_install_dir "$install_dir"

  local extract_dir="$TEMP_DIR/extract"
  mkdir -p "$extract_dir"
  tar -xzf "$archive" -C "$extract_dir"

  local payload_dir="" candidate=""
  if [[ -f "${extract_dir}/dbpp" && ! -d "${extract_dir}/dbpp" ]]; then
    payload_dir="$extract_dir"
  else
    for candidate in "${extract_dir}"/*/dbpp; do
      if [[ -f "$candidate" ]]; then payload_dir="$(dirname "$candidate")"; break; fi
    done
  fi
  [[ -n "$payload_dir" && -f "${payload_dir}/dbpp" ]] || die "Invalid Linux archive: dbpp executable not found."
  chmod +x "${payload_dir}/dbpp"

  atomic_replace_dir "$payload_dir" "$install_dir"
  mkdir -p "$bin_dir"
  ln -sfn "${install_dir}/dbpp" "${bin_dir}/dbpp"
  [[ "$no_desktop" != "1" ]] && install_linux_desktop "$install_dir" "$bin_dir"

  log "Verifying installed binary..."
  local version_out=""
  if ! version_out="$("${bin_dir}/dbpp" --version 2>&1)"; then
    printf '%s\n' "$version_out" >&2
    if command -v ldd >/dev/null 2>&1; then
      ldd "${install_dir}/dbpp" 2>&1 | grep -E 'not found|GLIBC' >&2 || true
    fi
    die "Installed dbpp failed self-check (--version). Ensure GTK4 (libgtk-4), libepoxy, libsoup-3.0, and GLIBC >= 2.39 are available."
  fi
  log "Installed: ${version_out}"
  log "Binary symlink: ${bin_dir}/dbpp -> ${install_dir}/dbpp"
  print_path_hint_if_needed "$bin_dir"
}

install_macos() {
  local dmg_path="$1" install_dir="$2" bin_dir="$3"
  mkdir -p "$install_dir"
  MACOS_MOUNT_DIR="$TEMP_DIR/mnt"
  mkdir -p "$MACOS_MOUNT_DIR"

  hdiutil attach -nobrowse -quiet -mountpoint "$MACOS_MOUNT_DIR" "$dmg_path"
  local app_src="${MACOS_MOUNT_DIR}/dbpp.app"
  [[ -d "$app_src" ]] || die "dbpp.app not found inside DMG."

  local stage_app="$TEMP_DIR/dbpp.app"
  if command -v ditto >/dev/null 2>&1; then ditto "$app_src" "$stage_app"; else cp -R "$app_src" "$stage_app"; fi
  hdiutil detach "$MACOS_MOUNT_DIR" -quiet || true
  MACOS_MOUNT_DIR=""

  local target_app="${install_dir}/dbpp.app"
  atomic_replace_dir "$stage_app" "$target_app"
  mkdir -p "$bin_dir"
  ln -sfn "${target_app}/Contents/MacOS/dbpp" "${bin_dir}/dbpp"

  local version_out=""
  version_out="$("${bin_dir}/dbpp" --version)" || die "Installed dbpp failed self-check (--version)."
  log "Installed: ${version_out}"
  log "App bundle: ${target_app}"
  log "CLI symlink: ${bin_dir}/dbpp"
  log "Note: If macOS Gatekeeper blocks first GUI launch, right-click dbpp.app -> Open or run:"
  printf '  xattr -dr com.apple.quarantine "%s"\n' "$target_app"
  print_path_hint_if_needed "$bin_dir"
}

uninstall_dbpp() {
  local install_dir="$1" bin_dir="$2"
  local link_path="${bin_dir}/dbpp"
  if [[ "$OS" == "linux" ]]; then
    guard_linux_install_dir "$install_dir"
    if [[ -L "$link_path" ]]; then
      local target
      target="$(readlink "$link_path" || true)"
      if [[ "$target" == "${install_dir}/"* || "$target" == "${install_dir}/dbpp" ]]; then
        rm -f "$link_path" && log "Removed symlink ${link_path}"
      else
        warn "Leaving ${link_path} untouched (points to ${target}, outside ${install_dir})."
      fi
    fi
    [[ -d "$install_dir" ]] && rm -rf "$install_dir" && log "Removed ${install_dir}"
    local xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
    rm -f "${xdg_data}/applications/dbpp.desktop" "${xdg_data}/icons/hicolor/256x256/apps/dbpp.png"
    refresh_linux_desktop_caches "$xdg_data"
    log "Uninstalled db++. User data (${xdg_data}/dbpp/dbpp.sqlite3) and ADBC drivers were preserved."
  else
    local target_app="${install_dir}/dbpp.app"
    if [[ -L "$link_path" ]]; then
      local target
      target="$(readlink "$link_path" || true)"
      if [[ "$target" == "${target_app}/"* ]]; then
        rm -f "$link_path" && log "Removed symlink ${link_path}"
      else
        warn "Leaving ${link_path} untouched (points to ${target}, outside ${target_app})."
      fi
    fi
    [[ -d "$target_app" ]] && rm -rf "$target_app" && log "Removed ${target_app}"
    log "Uninstalled db++. User data (~/Library/Application Support/dev.farna.dbpp) was preserved."
  fi
}

main() {
  local version="${DBPP_VERSION:-}"
  local install_dir="${DBPP_INSTALL_DIR:-}"
  local bin_dir="${DBPP_BIN_DIR:-$HOME/.local/bin}"
  local download_base="${DBPP_DOWNLOAD_BASE:-}"
  local repo="${DBPP_REPO:-$DEFAULT_REPO}"
  local no_desktop="0" require_checksum="0" skip_checksum="0" do_uninstall="0"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --version) [[ $# -ge 2 ]] || die "--version requires an argument"; version="$2"; shift 2 ;;
      --install-dir) [[ $# -ge 2 ]] || die "--install-dir requires an argument"; install_dir="$2"; shift 2 ;;
      --bin-dir) [[ $# -ge 2 ]] || die "--bin-dir requires an argument"; bin_dir="$2"; shift 2 ;;
      --download-base) [[ $# -ge 2 ]] || die "--download-base requires an argument"; download_base="$2"; shift 2 ;;
      --repo) [[ $# -ge 2 ]] || die "--repo requires an argument"; repo="$2"; shift 2 ;;
      --no-desktop) no_desktop="1"; shift ;;
      --require-checksum) require_checksum="1"; shift ;;
      --skip-checksum) skip_checksum="1"; shift ;;
      --uninstall) do_uninstall="1"; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "Unknown option: $1 (run with --help)" ;;
    esac
  done

  detect_platform
  if [[ -z "$install_dir" ]]; then
    if [[ "$OS" == "linux" ]]; then
      install_dir="$HOME/.local/opt/dbpp"
    elif [[ -d "/Applications" && -w "/Applications" ]]; then
      install_dir="/Applications"
    else
      install_dir="$HOME/Applications"
    fi
  fi
  install_dir="$(normalize_path "$install_dir")"
  bin_dir="$(normalize_path "$bin_dir")"

  if [[ "$do_uninstall" == "1" ]]; then
    uninstall_dbpp "$install_dir" "$bin_dir"
    return 0
  fi

  version="${version#v}"
  if [[ -z "$version" ]]; then
    log "Resolving latest db++ release..."
    version="$(resolve_latest_version "$repo")"
  fi

  local asset_name=""
  if [[ "$OS" == "linux" ]]; then
    asset_name="dbpp-${version}-linux-${ARCH}.tar.gz"
  else
    asset_name="dbpp-${version}-macos-${ARCH}.dmg"
  fi
  download_base="${download_base:-https://github.com/${repo}/releases/download/v${version}}"
  download_base="${download_base%/}"

  TEMP_DIR="$(mktemp -d)"
  local asset_path="${TEMP_DIR}/${asset_name}"
  log "Downloading ${asset_name} (${OS}/${ARCH})..."
  download_file "${download_base}/${asset_name}" "$asset_path" || die "Failed to download ${download_base}/${asset_name}"
  verify_checksum "$asset_path" "$asset_name" "${download_base}/SHA256SUMS.txt" "$require_checksum" "$skip_checksum"

  if [[ "$OS" == "linux" ]]; then
    install_linux "$asset_path" "$install_dir" "$bin_dir" "$no_desktop"
  else
    install_macos "$asset_path" "$install_dir" "$bin_dir"
  fi
}

main "$@"
