#!/usr/bin/env bash
# build-linux-repo.sh — Sign RPM/DEB release packages and generate static RPM + APT repositories.
set -euo pipefail

INPUT_DIR=""
OUTPUT_DIR=""
PKG_NAME="dbpp"
DISPLAY_NAME="db++"
VERSION=""
BASE_URL="https://farnaherry.github.io/dbpp"
SETUP_SCRIPT=""
REQUIRE_APT="0"

die() { printf '[build-linux-repo] error: %s\n' "$*" >&2; exit 1; }
log() { printf '[build-linux-repo] %s\n' "$*"; }

usage() {
  cat <<'EOF'
Usage: build-linux-repo.sh --input-dir <dir> --output-dir <dir> [options]

Options:
  --input-dir <dir>      Directory containing *.rpm and/or *.deb files
  --output-dir <dir>     Output site directory
  --pkg-name <name>      Package name (default: dbpp)
  --display-name <name>  Display name (default: db++)
  --version <ver>        Release version string (e.g. 0.4.10)
  --base-url <url>       Published base URL (default: https://farnaherry.github.io/dbpp)
  --setup-script <path>  Optional setup-repo.sh to copy to <output-dir>/setup-repo.sh
  --require-apt          Fail if dpkg-scanpackages / apt-ftparchive are unavailable
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input-dir) INPUT_DIR="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --pkg-name) PKG_NAME="$2"; shift 2 ;;
    --display-name) DISPLAY_NAME="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --base-url) BASE_URL="${2%/}"; shift 2 ;;
    --setup-script) SETUP_SCRIPT="$2"; shift 2 ;;
    --require-apt) REQUIRE_APT="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1" ;;
  esac
done

[[ -n "$INPUT_DIR" && -d "$INPUT_DIR" ]] || die "--input-dir must be an existing directory"
[[ -n "$OUTPUT_DIR" ]] || die "--output-dir is required"
[[ -n "${GPG_PRIVATE_KEY:-}" ]] || die "GPG_PRIVATE_KEY environment variable must be set"

command -v gpg >/dev/null 2>&1 || die "gpg is required"
command -v rpm >/dev/null 2>&1 || die "rpm is required"
command -v rpmsign >/dev/null 2>&1 || die "rpmsign is required"
command -v createrepo_c >/dev/null 2>&1 || die "createrepo_c is required"

TMP_ROOT="$(mktemp -d)"
cleanup() { rm -rf "$TMP_ROOT"; }
trap cleanup EXIT

GPG_HOME="${TMP_ROOT}/gnupg"
RPM_HOME="${TMP_ROOT}/rpmhome"
RPM_VERIFY_DB="${TMP_ROOT}/rpmdb"
mkdir -p "$GPG_HOME" "$RPM_HOME" "$RPM_VERIFY_DB"
chmod 700 "$GPG_HOME"

# Import private key without leaking secret material
(
  set +x
  printf '%s\n' "$GPG_PRIVATE_KEY" | gpg --homedir "$GPG_HOME" --batch --import 2>&1
)

FPR="$(gpg --homedir "$GPG_HOME" --with-colons --list-secret-keys | awk -F: '/^fpr:/ {print $10; exit}')"
[[ -n "$FPR" ]] || die "Failed to extract fingerprint from GPG_PRIVATE_KEY"
log "Imported GPG key fingerprint: $FPR"

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

gpg --homedir "$GPG_HOME" --armor --export "$FPR" > "${OUTPUT_DIR}/gpg.key"
test -s "${OUTPUT_DIR}/gpg.key"
rpm --dbpath "$RPM_VERIFY_DB" --import "${OUTPUT_DIR}/gpg.key"

PASS_FILE=""
if [[ -n "${GPG_PASSPHRASE:-}" ]]; then
  PASS_FILE="${TMP_ROOT}/passphrase"
  (
    set +x
    printf '%s' "$GPG_PASSPHRASE" > "$PASS_FILE"
    chmod 600 "$PASS_FILE"
  )
fi

REAL_GPG="$(command -v gpg)"
GPG_WRAPPER="${TMP_ROOT}/gpg-wrapper.sh"
if [[ -n "$PASS_FILE" ]]; then
  cat > "$GPG_WRAPPER" <<EOF
#!/usr/bin/env bash
exec "${REAL_GPG}" --batch --yes --pinentry-mode loopback --digest-algo sha256 --passphrase-file "${PASS_FILE}" "\$@"
EOF
else
  cat > "$GPG_WRAPPER" <<EOF
#!/usr/bin/env bash
exec "${REAL_GPG}" --batch --yes --pinentry-mode loopback --digest-algo sha256 "\$@"
EOF
fi
chmod 0755 "$GPG_WRAPPER"

cat > "${RPM_HOME}/.rpmmacros" <<EOF
%_signature gpg
%_gpg_name ${FPR}
%_gpg_path ${GPG_HOME}
%_gpg_digest_algo sha256
%__gpg ${GPG_WRAPPER}
%__gpg_check_password_cmd /bin/true
EOF

gpg_detach_sign() {
  local infile="$1" outfile="$2"
  local args=(--homedir "$GPG_HOME" --batch --yes --pinentry-mode loopback --digest-algo SHA256 --detach-sign --armor)
  [[ -n "$PASS_FILE" ]] && args+=(--passphrase-file "$PASS_FILE")
  gpg "${args[@]}" -o "$outfile" "$infile"
}

gpg_clearsign() {
  local infile="$1" outfile="$2"
  local args=(--homedir "$GPG_HOME" --batch --yes --pinentry-mode loopback --digest-algo SHA256 --clearsign)
  [[ -n "$PASS_FILE" ]] && args+=(--passphrase-file "$PASS_FILE")
  gpg "${args[@]}" -o "$outfile" "$infile"
}

# ---- Build & sign RPM repositories ----------------------------------------
shopt -s nullglob
rpm_inputs=("$INPUT_DIR"/*.rpm)
[[ "${#rpm_inputs[@]}" -gt 0 ]] || die "No .rpm files found in $INPUT_DIR"

declare -A rpm_arches=()
for rpm_src in "${rpm_inputs[@]}"; do
  arch="$(rpm -qp --qf '%{ARCH}' "$rpm_src" 2>/dev/null)"
  [[ -n "$arch" ]] || die "Failed to inspect architecture of $rpm_src"
  dest_dir="${OUTPUT_DIR}/rpm/${arch}"
  mkdir -p "$dest_dir"
  dest_file="${dest_dir}/$(basename "$rpm_src")"
  cp -f "$rpm_src" "$dest_file"
  HOME="$RPM_HOME" GNUPGHOME="$GPG_HOME" rpmsign --addsign "$dest_file" < /dev/null
  rpm --dbpath "$RPM_VERIFY_DB" --checksig "$dest_file"
  rpm_arches["$arch"]=1
done

for arch in "${!rpm_arches[@]}"; do
  repo_dir="${OUTPUT_DIR}/rpm/${arch}"
  log "Generating RPM metadata in ${repo_dir}..."
  createrepo_c --no-database "$repo_dir"
  gpg_detach_sign "${repo_dir}/repodata/repomd.xml" "${repo_dir}/repodata/repomd.xml.asc"
  gpg --homedir "$GPG_HOME" --verify "${repo_dir}/repodata/repomd.xml.asc" "${repo_dir}/repodata/repomd.xml"
done

# ---- Build & sign APT repository ------------------------------------------
deb_inputs=("$INPUT_DIR"/*.deb)
APT_STATUS="skipped"
if [[ "${#deb_inputs[@]}" -gt 0 ]]; then
  if command -v dpkg-scanpackages >/dev/null 2>&1 && command -v apt-ftparchive >/dev/null 2>&1; then
    APT_ROOT="${OUTPUT_DIR}/apt"
    PREFIX_CHAR="${PKG_NAME:0:1}"
    POOL_DIR="${APT_ROOT}/pool/main/${PREFIX_CHAR}/${PKG_NAME}"
    mkdir -p "$POOL_DIR"

    declare -A deb_arches=()
    for deb_src in "${deb_inputs[@]}"; do
      deb_arch="$(dpkg-deb -f "$deb_src" Architecture 2>/dev/null)"
      [[ -n "$deb_arch" ]] || die "Failed to inspect architecture of $deb_src"
      cp -f "$deb_src" "${POOL_DIR}/$(basename "$deb_src")"
      deb_arches["$deb_arch"]=1
    done

    ARCH_LIST=""
    for deb_arch in $(printf '%s\n' "${!deb_arches[@]}" | sort); do
      ARCH_LIST="${ARCH_LIST:+${ARCH_LIST} }${deb_arch}"
      rel_bin_dir="dists/stable/main/binary-${deb_arch}"
      mkdir -p "${APT_ROOT}/${rel_bin_dir}"
      (
        cd "$APT_ROOT"
        dpkg-scanpackages --multiversion --arch "$deb_arch" pool/main > "${rel_bin_dir}/Packages"
        gzip -9nc "${rel_bin_dir}/Packages" > "${rel_bin_dir}/Packages.gz"
      )
    done

    DIST_DIR="${APT_ROOT}/dists/stable"
    (
      cd "$DIST_DIR"
      apt-ftparchive \
        -o "APT::FTPArchive::Release::Origin=FarnaHerry" \
        -o "APT::FTPArchive::Release::Label=${DISPLAY_NAME}" \
        -o "APT::FTPArchive::Release::Suite=stable" \
        -o "APT::FTPArchive::Release::Codename=stable" \
        -o "APT::FTPArchive::Release::Architectures=${ARCH_LIST}" \
        -o "APT::FTPArchive::Release::Components=main" \
        -o "APT::FTPArchive::Release::Description=${DISPLAY_NAME} APT Repository" \
        release . > Release
    )
    gpg_detach_sign "${DIST_DIR}/Release" "${DIST_DIR}/Release.gpg"
    gpg_clearsign "${DIST_DIR}/Release" "${DIST_DIR}/InRelease"
    gpg --homedir "$GPG_HOME" --verify "${DIST_DIR}/Release.gpg" "${DIST_DIR}/Release"
    gpg --homedir "$GPG_HOME" --verify "${DIST_DIR}/InRelease"
    APT_STATUS="generated (${ARCH_LIST})"
    log "APT repository ${APT_STATUS}"
  else
    [[ "$REQUIRE_APT" == "1" ]] && die "dpkg-scanpackages / apt-ftparchive missing"
    log "Skipping APT metadata locally (dpkg-scanpackages / apt-ftparchive not installed)."
  fi
fi

if [[ -n "$SETUP_SCRIPT" ]]; then
  [[ -f "$SETUP_SCRIPT" ]] || die "Setup script not found: $SETUP_SCRIPT"
  cp -f "$SETUP_SCRIPT" "${OUTPUT_DIR}/setup-repo.sh"
  chmod 0755 "${OUTPUT_DIR}/setup-repo.sh"
fi

cat > "${OUTPUT_DIR}/index.html" <<EOF
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>${DISPLAY_NAME} Linux Package Repository</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; max-width: 780px; margin: 2.5rem auto; padding: 0 1rem; line-height: 1.6; color: #1f2328; }
    pre { background: #f6f8fa; padding: 0.9rem 1rem; border-radius: 6px; overflow-x: auto; }
    code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  </style>
</head>
<body>
  <h1>${DISPLAY_NAME} Linux Package Repository</h1>
  <p>Published version: <code>${VERSION:-latest}</code> &middot; GPG key: <a href="gpg.key">gpg.key</a> (<code>${FPR}</code>)</p>
  <h2>Quick Install (Fedora / RHEL / openSUSE / Debian / Ubuntu)</h2>
  <pre><code>curl -fsSL ${BASE_URL}/setup-repo.sh | sudo bash</code></pre>
  <h2>Fedora / RHEL (dnf)</h2>
  <pre><code>sudo rpm --import ${BASE_URL}/gpg.key
sudo tee /etc/yum.repos.d/${PKG_NAME}.repo &lt;&lt;'REPO'
[${PKG_NAME}]
name=${DISPLAY_NAME} Repository
baseurl=${BASE_URL}/rpm/\$basearch/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=${BASE_URL}/gpg.key
REPO
sudo dnf install -y ${PKG_NAME}</code></pre>
  <h2>Debian / Ubuntu (apt)</h2>
  <pre><code>curl -fsSL ${BASE_URL}/gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/${PKG_NAME}.gpg
echo "deb [arch=\$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/${PKG_NAME}.gpg] ${BASE_URL}/apt stable main" | sudo tee /etc/apt/sources.list.d/${PKG_NAME}.list
sudo apt update &amp;&amp; sudo apt install -y ${PKG_NAME}</code></pre>
</body>
</html>
EOF

log "Repository site built at ${OUTPUT_DIR} (RPM arches: ${!rpm_arches[*]}; APT: ${APT_STATUS})"
