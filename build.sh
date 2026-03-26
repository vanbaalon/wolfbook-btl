#!/usr/bin/env bash
# ==============================================================
# build.sh  —  Wolfbook build script
#
# Usage:
#   ./build.sh              full build (native + TypeScript)
#   ./build.sh native       C++ addon only
#   ./build.sh ts           TypeScript only
#   ./build.sh rebuild      force full native rebuild + TS
#   ./build.sh clean        remove build/ and out/ artefacts
#   ./build.sh test         run WL unit tests (needs wolframscript)
#   ./build.sh smoke        quick Node.js smoke test of the addon
#   ./build.sh generate     regenerate special_chars.cpp from Mathematica
#                           (requires wolframscript; output is then committed)
# ==============================================================
set -euo pipefail

# ---- colours (skip when not a TTY) ----
if [ -t 1 ]; then
  RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
  CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

step()  { echo -e "\n${CYAN}${BOLD}▶ $*${RESET}"; }
ok()    { echo -e "${GREEN}✓ $*${RESET}"; }
warn()  { echo -e "${YELLOW}⚠ $*${RESET}"; }
fail()  { echo -e "${RED}✗ $*${RESET}"; exit 1; }

# ---- helpers ----

check_node() {
  command -v node &>/dev/null || fail "node not found — install Node.js >= 18"
  NODE_VER=$(node -e "process.stdout.write(process.version)")
  ok "Node $NODE_VER"
}

check_npm_deps() {
  if [ ! -d node_modules ]; then
    step "Installing npm dependencies"
    npm install --ignore-scripts   # skip auto-rebuild; we control that below
    ok "npm install done"
  else
    ok "node_modules present"
  fi
}

build_native() {
  local mode="${1:-incremental}"   # incremental | rebuild
  step "Building C++ native addon (mode=$mode)"
  if [ "$mode" = "rebuild" ]; then
    node_modules/.bin/node-gyp rebuild 2>&1 | grep -v '^gyp info' | sed 's/^/  /'
  else
    node_modules/.bin/node-gyp configure 2>&1 | grep -v '^gyp info' | sed 's/^/  /'
    node_modules/.bin/node-gyp build     2>&1 | grep -v '^gyp info' | sed 's/^/  /'
  fi
  ADDON="build/Release/wolfbook_btl.node"
  [ -f "$ADDON" ] || fail "native addon not found at $ADDON after build"
  SIZE=$(du -sh "$ADDON" | cut -f1)
  ok "native addon built — $ADDON ($SIZE)"
}

build_ts() {
  step "Compiling TypeScript (src/)"
  node_modules/.bin/tsc -p tsconfig.json 2>&1 | sed 's/^/  /'
  ok "TypeScript compiled → out/"
}

smoke_test() {
  step "Smoke-testing native addon"
  node - <<'EOF'
const addon = require('./build/Release/wolfbook_btl.node');

const cases = [
  ['FractionBox["1","2"]',                   '\\frac{1}{2}'],
  ['SqrtBox["x"]',                           '\\sqrt{x}'],
  ['SuperscriptBox["x","2"]',                'x^2'],
  ['SubscriptBox["x","i"]',                  'x_i'],
  ['RowBox[{"x","+","y"}]',                  'x+y'],
  ['UnderoverscriptBox["\\\\[Sum]",RowBox[{"n","=","1"}],"\\\\[Infinity]"]',
                                             '\\sum_{n=1}^{\\infty}'],
  ['"\\\\[Alpha]"',                          '\\alpha'],
  ['"\\\\[Infinity]"',                       '\\infty'],
];

let passed = 0, failed = 0;
for (const [input, expected] of cases) {
  const result = addon.boxToLatex(input);
  const latex  = result.latex;   // result is { latex, error }
  if (result.error) {
    console.error(`  FAIL  ${input.slice(0,50)}`);
    console.error(`        error: ${result.error}`);
    failed++;
  } else if (latex === expected) {
    console.log(`  PASS  ${input.slice(0,50)}`);
    passed++;
  } else {
    console.error(`  FAIL  ${input.slice(0,50)}`);
    console.error(`        got:      ${latex}`);
    console.error(`        expected: ${expected}`);
    failed++;
  }
}
console.log(`\n${passed} passed / ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
EOF
  ok "smoke tests passed"
}

run_wl_tests() {
  if ! command -v wolframscript &>/dev/null; then
    warn "wolframscript not found — skipping WL unit tests"
    return
  fi
  step "Running WL unit tests"
  wolframscript -file renderer/Tests.wl
  ok "WL tests done"
}

copy_to_ext() {
  local ADDON="$ROOT/build/Release/wolfbook_btl.node"
  local EXT_DEV="$ROOT/../VSCodeWolframExtension/Extension Development/wllatex-addon/wolfbook_btl.node"
  if [ -f "$ADDON" ] && [ -d "$(dirname "$EXT_DEV")" ]; then
    cp "$ADDON" "$EXT_DEV"
    ok "Copied wolfbook_btl.node → Extension Development/wllatex-addon/"
  else
    warn "Extension dev folder not found — skipping auto-copy (expected: $EXT_DEV)"
  fi
}

clean() {
  step "Cleaning build artefacts"
  rm -rf build/ out/
  ok "build/ and out/ removed"
}

# ---- main dispatch ----
TARGET="${1:-all}"

case "$TARGET" in
  all | "")
    check_node
    check_npm_deps
    build_native incremental
    build_ts
    smoke_test
    copy_to_ext
    echo -e "\n${GREEN}${BOLD}Build complete.${RESET}"
    ;;
  native)
    check_node
    check_npm_deps
    build_native incremental
    smoke_test
    copy_to_ext
    ;;
  ts)
    check_node
    check_npm_deps
    build_ts
    ;;
  rebuild)
    check_node
    check_npm_deps
    build_native rebuild
    build_ts
    smoke_test
    copy_to_ext
    echo -e "\n${GREEN}${BOLD}Full rebuild complete.${RESET}"
    ;;
  clean)
    clean
    ;;
  test)
    run_wl_tests
    ;;
  smoke)
    smoke_test
    ;;
  generate)
    step "Regenerating special_chars.cpp from Mathematica character table"
    if ! command -v wolframscript &>/dev/null; then
      fail "wolframscript not found — install Wolfram Engine or Mathematica"
    fi
    wolframscript -file "$ROOT/renderer/GenerateCharTable.wls"
    ok "special_chars.cpp regenerated"
    echo -e "  Run ${CYAN}./build.sh native${RESET} to recompile with the updated table."
    ;;
  *)
    echo "Usage: $0 [all | native | ts | rebuild | clean | test | smoke | generate]"
    exit 1
    ;;
esac
