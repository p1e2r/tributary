#!/usr/bin/env bash
# Assistant providers (Claude, GPT, Gemini, Ollama) against a mock of each streaming API,
# the MCP server over stdio, and the updater's archive reader.
set -u
TRIBCLI=${TRIBCLI:-$PWD/build/tribcli}
APP=${APP:-$PWD/build/Tributary}
HERE=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3 || command -v python)
T=$(mktemp -d)
pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1"; fail=$((fail+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

PORT=$((20000 + RANDOM % 20000))
"$PY" "$HERE/ai_mock.py" $PORT "$T/ai.log" & MOCK=$!
trap 'kill $MOCK 2>/dev/null; rm -rf "$T"' EXIT
for i in $(seq 50); do "$PY" -c "import socket;socket.create_connection(('127.0.0.1',$PORT),1)" 2>/dev/null && break; sleep 0.1; done

mkdir -p "$T/w/src"
printf '#pragma once\n\n/* Adds two numbers. */\ninline int add(int a, int b) {\n    return a + b;\n}\n' > "$T/util.orig"
for p in anthropic openai gemini ollama; do
  cp "$T/util.orig" "$T/w/src/util.hpp"
  out=$(cd "$T/w" && TRIBUTARY_AI_KEY=k123 "$TRIBCLI" ai $p m1 "make add constexpr" http://127.0.0.1:$PORT 2>&1)
  check "$p: tool loop edits the file" 'grep -q "constexpr int add" "$T/w/src/util.hpp"'
  check "$p: streams the final answer" '[[ "$out" == *"ready for your pull request"* ]]'
  models=$(TRIBUTARY_AI_KEY=k123 "$TRIBCLI" ai-models $p http://127.0.0.1:$PORT 2>&1 | tr -d '\r' | tr '\n' ' ')
  check "$p: lists chat models only" '[[ -n "$models" && "$models" != *embedding* ]]'
done
check "anthropic: sends x-api-key and version" 'grep "/v1/messages" "$T/ai.log" | grep -q "\"x-api-key\": \"k123\""'
check "gemini: echoes thought signatures" 'grep "streamGenerateContent" "$T/ai.log" | tail -1 | grep -q c2lnbmF0dXJl'
check "openai: sends tool results with call ids" 'grep "/chat/completions" "$T/ai.log" | tail -1 | grep -q "\"tool_call_id\": \"call_1\""'

# ---- MCP server
R="$T/repo"; mkdir -p "$R"
( cd "$R" && git init -q -b main && git config user.name T && git config user.email t@e.com && echo hello > a.txt && git add . && git commit -qm init && echo world >> a.txt )
cat > "$T/mcp.in" <<'J'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"status","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"create_branch","arguments":{"name":"feature"}}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"commit","arguments":{"message":"Add world"}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"read_file","arguments":{"path":"a.txt","revision":"main"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"checkout","arguments":{"branch":"nope"}}}
{"jsonrpc":"2.0","id":8,"method":"bogus"}
J
HOME="$T/home" APPDATA="$T/home" "$APP" --mcp --repo "$R" < "$T/mcp.in" > "$T/mcp.out" 2>"$T/mcp.err"
tr -d '\r' < "$T/mcp.out" > "$T/mcp.txt"
check "mcp: initialize" 'grep -q "\"protocolVersion\":\"2025-06-18\"" "$T/mcp.txt"'
check "mcp: no reply to notifications" '[ "$(wc -l < "$T/mcp.txt" | tr -d " ")" = 8 ]'
check "mcp: lists tools" 'grep "\"id\":2" "$T/mcp.txt" | grep -q create_pull_request'
check "mcp: status" 'grep "\"id\":3" "$T/mcp.txt" | grep -q "Modified  a.txt"'
check "mcp: commit lands in git" '[ "$(git -C "$R" log -1 --format=%s feature)" = "Add world" ]'
check "mcp: read_file at a revision" 'grep "\"id\":6" "$T/mcp.txt" | grep -q "\"text\":\"hello\\\\n\""'
check "mcp: tool errors are isError" 'grep "\"id\":7" "$T/mcp.txt" | grep -q "\"isError\":true"'
check "mcp: unknown method" 'grep "\"id\":8" "$T/mcp.txt" | grep -q -- "-32601"'

# ---- archives (self-update)
A="$T/arc/src/pkg"; mkdir -p "$A/sub/deeper"
printf '#!/bin/sh\necho hi\n' > "$A/run"; chmod 755 "$A/run"
seq 1 20000 > "$A/sub/big.txt"; echo x > "$A/sub/deeper/$(printf 'n%.0s' {1..120}).txt"
(cd "$T/arc/src" && tar -czf ../a.tar.gz pkg)
"$PY" - "$T/arc" <<'P'
import os, sys, zipfile
root = sys.argv[1]
with zipfile.ZipFile(os.path.join(root, "a.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, fs in os.walk(os.path.join(root, "src")):
        for f in fs:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, os.path.join(root, "src")))
P
for a in a.tar.gz a.zip; do
  rm -rf "$T/arc/out"
  "$TRIBCLI" extract "$T/arc/$a" "$T/arc/out" >/dev/null 2>&1
  check "extract $a" 'diff -r "$T/arc/src" "$T/arc/out" >/dev/null'
done
"$PY" -c "
import zipfile; z=zipfile.ZipFile('$T/arc/evil.zip','w'); z.writestr('../evil.txt','x'); z.close()"
check "extract refuses ../ paths" '! "$TRIBCLI" extract "$T/arc/evil.zip" "$T/arc/out2" >/dev/null 2>&1 && [ ! -e "$T/arc/evil.txt" ]'

echo "$pass passed, $fail failed"
[ $fail = 0 ]
