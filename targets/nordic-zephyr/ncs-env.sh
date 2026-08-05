# Source this file to put west + the matching NCS toolchain on PATH:
#   source ncs-env.sh
[[ -n "${BASH_VERSION:-}" ]] || return 0 2>/dev/null || exit 0

NCS_ROOT="${NCS_ROOT:-${NCS_DIR:-}}"

# THE NCS VERSION IS PINNED, NOT DISCOVERED.
#
# This used to glob `$HOME/ncs/v3.* $HOME/ncs/v2.*` and take the first hit. That is not
# reproducible: it silently selects whatever the developer happens to have installed, in shell
# glob order, so two machines with different SDKs build different firmware from the same commit
# and neither is told. It also quietly accepted a v2.x SDK for a target that needs v3.x.
#
# OD_NCS_VERSION is the single pin (docs/TOOLCHAINS.md). Override NCS_ROOT explicitly to build
# against something else -- deliberately, and visibly in your shell history, rather than by
# accident of what is on disk.
OD_NCS_VERSION="${OD_NCS_VERSION:-v3.3.1}"

find_ncs_root() {
  local d="$HOME/ncs/${OD_NCS_VERSION}"
  if [[ -d "$d/nrf" && -d "$d/zephyr" ]]; then
    echo "$d"
    return 0
  fi
  return 1
}

if [[ -z "${NCS_ROOT}" ]]; then
  if NCS_ROOT="$(find_ncs_root)"; then
    echo "Auto-detected NCS_ROOT=${NCS_ROOT}"
  fi
fi

if [[ -n "${NCS_ROOT}" && "${NCS_ROOT}" == *"/toolchains/"* ]]; then
  echo "NCS_ROOT points at the toolchain, not the SDK source tree." >&2
  NCS_ROOT=""
  if NCS_ROOT="$(find_ncs_root)"; then
    echo "Auto-detected NCS_ROOT=${NCS_ROOT}"
  fi
fi

if [[ -z "${NCS_ROOT}" ]]; then
  echo "NCS ${OD_NCS_VERSION} not found at ~/ncs/${OD_NCS_VERSION}." >&2
  echo "Install it via nRF Connect, or set NCS_ROOT / OD_NCS_VERSION explicitly." >&2
  return 1 2>/dev/null || exit 1
fi

NCS_ROOT="${NCS_ROOT/#\~/$HOME}"

_activate_toolchain() {
  local tc="$1"
  local env_json="${tc}/environment.json"
  if [[ ! -f "${env_json}" ]]; then
    echo "Missing ${env_json}" >&2
    return 1 2>/dev/null || exit 1
  fi
  eval "$(python3 - "${tc}" "${env_json}" <<'PY'
import json, os, shlex, sys

tc, env_json = sys.argv[1], sys.argv[2]
with open(env_json) as f:
    data = json.load(f)

for var in data["env_vars"]:
    key = var["key"]
    treatment = var.get("existing_value_treatment", "overwrite")
    if var["type"] == "string":
        print(f"export {key}={shlex.quote(var['value'])}")
    elif var["type"] == "relative_paths":
        paths = ":".join(os.path.join(tc, p) for p in var["values"])
        if treatment == "prepend_to":
            print(f'export {key}="{paths}:${{{key}:-}}"')
        else:
            print(f"export {key}={shlex.quote(paths)}")
PY
)"
}

_activate_ncs_env() {
  local zephyr_env="${NCS_ROOT}/zephyr/zephyr-env.sh"
  if [[ ! -f "${zephyr_env}" ]]; then
    echo "Missing ${zephyr_env}" >&2
    return 1 2>/dev/null || exit 1
  fi
  # shellcheck source=/dev/null
  source "${zephyr_env}"

  local tc_id tc_root
  tc_id="$("${NCS_ROOT}/nrf/scripts/print_toolchain_checksum.sh")"
  tc_root="${HOME}/ncs/toolchains/${tc_id}"
  if [[ ! -d "${tc_root}" ]]; then
    if command -v west >/dev/null 2>&1; then
      return 0
    fi
    echo "Matching toolchain not found: ${tc_root}" >&2
    return 1 2>/dev/null || exit 1
  fi
  echo "Activating toolchain ${tc_id}"
  _activate_toolchain "${tc_root}"
  if ! command -v west >/dev/null 2>&1; then
    echo "west still not found after activating ${tc_root}" >&2
    return 1 2>/dev/null || exit 1
  fi
}

_activate_ncs_env
