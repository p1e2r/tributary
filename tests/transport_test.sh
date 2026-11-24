#!/usr/bin/env bash
# Clone / push / pull / non-fast-forward rejection over git's smart HTTP
# protocol, against a local `git http-backend`.
set -euo pipefail
T=${TRIBCLI:-$(pwd)/build-cli/tribcli}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
export GIT_CONFIG_GLOBAL="$W/gitconfig"
printf '[user]\n\tname = Test User\n\temail = test@example.com\n' > "$GIT_CONFIG_GLOBAL"
git init -q --bare "$W/srv/remote.git"
git -C "$W/srv/remote.git" config http.receivepack true
python3 "$HERE/http_backend.py" "$W/srv" $PORT & SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
sleep 1
URL=http://127.0.0.1:$PORT/remote.git
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "ok   - $1"; else fail=$((fail+1)); echo "FAIL - $1"; fi; }

cd "$W" && $T init a && cd a
echo one > one.txt; mkdir d; seq 1 2000 > d/n.txt
$T commit c1 >/dev/null
git remote add origin $URL
$T push origin main
check "push to empty remote" "git -C $W/srv/remote.git rev-parse refs/heads/main >/dev/null"
check "remote passes fsck" "[ -z \"\$(git -C $W/srv/remote.git fsck --strict refs/heads/main 2>&1 | grep -E 'error|missing|broken')\" ]"
cd "$W" && $T clone $URL b 2>/dev/null && cd b
check "clone has content" "[ \"\$(cat one.txt)\" = one ] && [ \"\$(wc -l < d/n.txt)\" = 2000 ]"
check "clone tracks origin" "[ \"\$(git rev-parse --abbrev-ref main@{upstream})\" = origin/main ]"
echo two >> one.txt; $T commit c2 >/dev/null; $T push
cd "$W/a"; git config branch.main.remote origin; git config branch.main.merge refs/heads/main
$T pull >/dev/null
check "fast-forward pull" "[ \"\$(tail -1 one.txt)\" = two ]"
echo x > x.txt; $T commit "a only" >/dev/null; $T push
cd "$W/b"; echo y > y.txt; $T commit "b only" >/dev/null
check "non-fast-forward push is rejected" "! $T push 2>/dev/null"
$T pull >/dev/null
check "pull merges divergent history" "[ \$(git log -1 --format=%p | wc -w) = 2 ] && [ -e x.txt ] && [ -e y.txt ]"
$T push
check "push after merge" "[ \"\$(git -C $W/srv/remote.git rev-parse refs/heads/main)\" = \"\$(git rev-parse HEAD)\" ]"
check "remote fsck after merge push" "[ -z \"\$(git -C $W/srv/remote.git fsck --strict refs/heads/main 2>&1 | grep -E 'error|missing|broken')\" ]"
# repack remote so the next clone receives deltas
git -C "$W/srv/remote.git" repack -adq -f --depth=50 --window=50
cd "$W" && $T clone $URL c 2>/dev/null && cd c
check "clone of delta-packed repo is valid" "[ -z \"\$(git fsck --strict 2>&1 | grep -E 'error|missing|broken')\" ] && [ -z \"\$(git status --porcelain)\" ]"
echo; echo "$pass passed, $fail failed"
[ $fail = 0 ]
