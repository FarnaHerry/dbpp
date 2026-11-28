#!/usr/bin/env bash
# setup-repo.sh — Configure the db++ Linux RPM/APT repository and install db++.
set -euo pipefail

PKG_NAME="dbpp"
REPO_ID="dbpp"
DISPLAY_NAME="db++"
DEFAULT_BASE_URL="https://farnaherry.github.io/dbpp"

BASE_URL="${DBPP_REPO_URL:-$DEFAULT_BASE_URL}"
REPO_ONLY="0"
UNINSTALL="0"

log() { printf '[%s] %s\n' "$PKG_NAME" "$*"; }
warn() { printf '[%s] warning: %s\n' "$PKG_NAME" "$*" >&2; }
die() { printf '[%s] error: %s\n' "$PKG_NAME" "$*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: setup-repo.sh [options]

Configure the official signed RPM/APT repository for ${DISPLAY_NAME} and install ${PKG_NAME}.

Options:
  --repo-only        Only configure repository & GPG key; skip package install
  --uninstall        Remove ${PKG_NAME} repository configuration and GPG key
  --base-url <url>   Override repository base URL (default: ${DEFAULT_BASE_URL})
  -h, --help         Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-only) REPO_ONLY="1"; shift ;;
    --uninstall) UNINSTALL="1"; shift ;;
    --base-url)
      [[ $# -ge 2 ]] || die "--base-url requires an argument"
      BASE_URL="${2%/}"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1 (run with --help)" ;;
  esac
done

BASE_URL="${BASE_URL%/}"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  die "Root privileges are required to modify system repositories. Re-run with sudo:
  curl -fsSL ${BASE_URL}/setup-repo.sh | sudo bash"
fi

download_to() {
  local url="$1" dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 --retry-delay 1 "$url" -o "$dest"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$dest" "$url"
  else
    die "Neither curl nor wget is installed."
  fi
}

uninstall_repo() {
  local removed="0"
  for f in \
    "/etc/yum.repos.d/${REPO_ID}.repo" \
    "/etc/zypp/repos.d/${REPO_ID}.repo" \
    "/etc/apt/sources.list.d/${REPO_ID}.list" \
    "/etc/apt/sources.list.d/${REPO_ID}.sources" \
    "/etc/apt/keyrings/${REPO_ID}.gpg"
  do
    if [[ -e "$f" || -L "$f" ]]; then
      rm -f "$f"
      log "Removed $f"
      removed="1"
    fi
  done
  if [[ "$removed" == "0" ]]; then
    log "No ${DISPLAY_NAME} repository configuration found."
  fi
  log "Repository configuration removed. To uninstall the package itself, run:"
  log "  sudo dnf remove ${PKG_NAME}   (Fedora/RHEL)  or  sudo apt remove ${PKG_NAME}   (Debian/Ubuntu)"
}

if [[ "$UNINSTALL" == "1" ]]; then
  uninstall_repo
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

KEY_FILE="${TMP_DIR}/gpg.key"
log "Downloading GPG public key from ${BASE_URL}/gpg.key..."
download_to "${BASE_URL}/gpg.key" "$KEY_FILE" || die "Failed to download ${BASE_URL}/gpg.key"

check_user_local_shadow() {
  local user_home=""
  if [[ -n "${SUDO_USER:-}" ]]; then
    user_home="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6 || true)"
    if [[ -n "$user_home" && -e "${user_home}/.local/bin/${PKG_NAME}" ]]; then
      warn "Detected user-level install at ${user_home}/.local/bin/${PKG_NAME}."
      warn "If ${user_home}/.local/bin precedes /usr/bin in PATH, your shell may still invoke the user-level copy."
      warn "To remove the user-level copy (keeps user data), run as ${SUDO_USER}: ${user_home}/.local/opt/${PKG_NAME} or curl -fsSL https://github.com/FarnaHerry/dbpp/releases/latest/download/install.sh | bash -s -- --uninstall"
    fi
  fi
}

if command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then
  command -v gpg >/dev/null 2>&1 || die "gpg is required to dearmor the APT repository key. Install gnupg and retry."
  install -d -m 0755 /etc/apt/keyrings /etc/apt/sources.list.d
  KEYRING_PATH="/etc/apt/keyrings/${REPO_ID}.gpg"
  rm -f "${KEYRING_PATH}.tmp"
  gpg --dearmor --yes -o "${KEYRING_PATH}.tmp" "$KEY_FILE"
  chmod 0644 "${KEYRING_PATH}.tmp"
  mv -f "${KEYRING_PATH}.tmp" "$KEYRING_PATH"

  ARCH="$(dpkg --print-architecture)"
  cat > "/etc/apt/sources.list.d/${REPO_ID}.list" <<EOF
deb [arch=${ARCH} signed-by=${KEYRING_PATH}] ${BASE_URL}/apt stable main
EOF
  chmod 0644 "/etc/apt/sources.list.d/${REPO_ID}.list"
  log "Configured APT repository: /etc/apt/sources.list.d/${REPO_ID}.list (${ARCH})"

  apt-get update -o Dir::Etc::sourcelist="sources.list.d/${REPO_ID}.list" -o Dir::Etc::sourceparts="-" -o APT::Get::List-Cleanup="0" || apt-get update
  if [[ "$REPO_ONLY" != "1" ]]; then
    log "Installing ${PKG_NAME}..."
    apt-get install -y "$PKG_NAME"
    log "Installed: $("${PKG_NAME}" --version 2>/dev/null || echo ok)"
    check_user_local_shadow
  else
    log "Repository configured. Install with: sudo apt-get install -y ${PKG_NAME}"
  fi
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1 || command -v microdnf >/dev/null 2>&1; then
  rpm --import "$KEY_FILE"
  install -d -m 0755 /etc/yum.repos.d
  REPO_FILE="/etc/yum.repos.d/${REPO_ID}.repo"
  cat > "$REPO_FILE" <<EOF
[${REPO_ID}]
name=${DISPLAY_NAME} Repository
baseurl=${BASE_URL}/rpm/\$basearch/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=${BASE_URL}/gpg.key
metadata_expire=300
EOF
  chmod 0644 "$REPO_FILE"
  log "Configured RPM repository: ${REPO_FILE}"

  PKG_MGR="dnf"
  if ! command -v dnf >/dev/null 2>&1; then
    if command -v microdnf >/dev/null 2>&1; then PKG_MGR="microdnf"; else PKG_MGR="yum"; fi
  fi

  if [[ "$REPO_ONLY" != "1" ]]; then
    log "Installing ${PKG_NAME} via ${PKG_MGR}..."
    "$PKG_MGR" makecache -y --disablerepo='*' --enablerepo="${REPO_ID}" >/dev/null 2>&1 || true
    "$PKG_MGR" install -y "$PKG_NAME"
    log "Installed: $("${PKG_NAME}" --version 2>/dev/null || echo ok)"
    check_user_local_shadow
  else
    log "Repository configured. Install with: sudo ${PKG_MGR} install -y ${PKG_NAME}"
  fi
elif command -v zypper >/dev/null 2>&1; then
  rpm --import "$KEY_FILE"
  install -d -m 0755 /etc/zypp/repos.d
  REPO_FILE="/etc/zypp/repos.d/${REPO_ID}.repo"
  cat > "$REPO_FILE" <<EOF
[${REPO_ID}]
name=${DISPLAY_NAME} Repository
baseurl=${BASE_URL}/rpm/\$basearch/
enabled=1
autorefresh=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=${BASE_URL}/gpg.key
EOF
  chmod 0644 "$REPO_FILE"
  zypper --non-interactive --gpg-auto-import-keys refresh "$REPO_ID"
  log "Configured Zypper repository: ${REPO_FILE}"
  if [[ "$REPO_ONLY" != "1" ]]; then
    zypper --non-interactive install "$PKG_NAME"
    log "Installed: $("${PKG_NAME}" --version 2>/dev/null || echo ok)"
    check_user_local_shadow
  else
    log "Repository configured. Install with: sudo zypper install ${PKG_NAME}"
  fi
else
  die "Unsupported Linux distribution (no apt-get, dnf/yum, or zypper found). Use install.sh for rootless tarball installation."
fi
