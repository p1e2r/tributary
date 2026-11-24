#!/usr/bin/env bash
# History rewriting, squash merges, merge previews, hooks and SSH transport,
# cross-checked with git.
set -euo pipefail
T=${TRIBCLI:-$(pwd)/build/tribcli}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export GIT_CONFIG_GLOBAL="$W/gitconfig"
printf '[user]\n\tname = Test User\n\temail = test@example.com\n[init]\n\tdefaultBranch = main\n' > "$GIT_CONFIG_GLOBAL"
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "ok   - $1"; else fail=$((fail+1)); echo "FAIL - $1"; fi; }
clean() { [ -z "$(git status --porcelain)" ] && [ -z "$(git fsck --strict 2>&1 | grep -E 'error|missing|broken')" ]; }
subjects() { git log --format=%s | tr '\n' '|'; }

cd "$W" && $T init r && cd r
for i in 1 2 3 4; do echo "line $i" > f$i.txt; $T commit "c$i" >/dev/null; done
check "base history" "[ \"\$(subjects)\" = 'c4|c3|c2|c1|' ]"

$T reword HEAD~2 "c2 renamed"
check "reword middle commit" "[ \"\$(subjects)\" = 'c4|c3|c2 renamed|c1|' ] && clean && [ \"\$(git symbolic-ref HEAD)\" = refs/heads/main ]"
check "reword kept the tree" "[ \"\$(git rev-parse HEAD^{tree})\" = \"\$(git rev-parse ORIG_HEAD^{tree})\" ]"

$T reorder HEAD~2 HEAD          # move c4 to just before 'c2 renamed'
check "reorder commits" "[ \"\$(subjects)\" = 'c3|c2 renamed|c4|c1|' ] && clean"

$T squash HEAD~1 "c2 + c3" HEAD  # squash c3 into 'c2 renamed'
check "squash two commits" "[ \"\$(subjects)\" = 'c2 + c3|c4|c1|' ] && clean && [ -e f3.txt ] && [ -e f2.txt ]"

# rebase a feature branch onto main
$T branch feature; $T checkout feature
echo feat > feat.txt; $T commit "feature 1" >/dev/null
echo feat2 >> feat.txt; $T commit "feature 2" >/dev/null
$T checkout main; echo m > main.txt; $T commit "main moves" >/dev/null
$T checkout feature
check "rebase clean" "[ \"\$($T rebase refs/heads/main)\" = done ] && [ \"\$(subjects | cut -d'|' -f1-3)\" = 'feature 2|feature 1|main moves' ] && clean"
check "rebase result linear" "[ \$(git rev-list --merges HEAD | wc -l | tr -d ' ') = 0 ] && git merge-base --is-ancestor main feature"

# rebase with a conflict, then continue
$T checkout main; echo "main side" > shared.txt; $T commit "main shared" >/dev/null
$T checkout feature; echo "feature side" > shared.txt; $T commit "feature shared" >/dev/null
check "rebase stops on conflict" "[ \"\$($T rebase refs/heads/main)\" = conflicts ] && grep -q '<<<<<<<' shared.txt"
echo resolved > shared.txt
check "continue after resolving" "[ \"\$($T continue)\" = done ] && [ \"\$(git log -1 --format=%s)\" = 'feature shared' ] && [ \"\$(cat shared.txt)\" = resolved ] && clean"

# abort restores everything
$T checkout main; echo "main again" > shared.txt; $T commit "main shared 2" >/dev/null
$T checkout feature; before=$(git rev-parse HEAD)
$T rebase refs/heads/main >/dev/null
$T abort
check "abort restores branch" "[ \"\$(git rev-parse HEAD)\" = $before ] && [ \"\$(git symbolic-ref HEAD)\" = refs/heads/feature ] && clean"

# merge preview + squash merge
$T checkout main
check "preview detects conflicts" "$T preview refs/heads/feature | grep -q '^conflicts'"
$T branch side; $T checkout side; echo s1 > s.txt; $T commit "side 1" >/dev/null; echo s2 >> s.txt; $T commit "side 2" >/dev/null
$T checkout main
check "preview clean merge" "[ \"\$($T preview refs/heads/side)\" = 'clean commits=2 conflicts=0' ] || [ \"\$($T preview refs/heads/side)\" = 'fast-forward commits=2 conflicts=0' ]"
echo x > x.txt; $T commit "main x" >/dev/null
check "squash merge makes one commit" "[ \"\$($T squash-merge refs/heads/side)\" = merged ] && [ \$(git log -1 --format=%p | wc -w | tr -d ' ') = 1 ] && [ \"\$(cat s.txt)\" = \$'s1\ns2' ] && clean"
check "squash message lists commits" "git log -1 --format=%B | grep -q '\* side 2'"

# cherry-pick onto another branch
$T branch other HEAD~1 2>/dev/null || $T branch other
pick=$(git rev-parse HEAD)
$T cherry other $pick >/dev/null
check "cherry-pick switches and applies" "[ \"\$(git symbolic-ref --short HEAD)\" = other ] && [ \"\$(git log -1 --format=%s)\" = \"\$(git log -1 --format=%s $pick)\" ] && clean"

# reset to commit (keeps changes)
$T checkout main
$T reset HEAD~1
check "reset keeps changes as uncommitted" "git status --porcelain | grep -q 's.txt' && [ \"\$(git log -1 --format=%s)\" = 'main x' ]"
git checkout -q -- . ; git clean -fdq

# tags
$T tag v1.0; check "create tag" "git rev-parse -q --verify refs/tags/v1.0 >/dev/null"
$T deltag v1.0; check "delete tag" "! git rev-parse -q --verify refs/tags/v1.0 >/dev/null"

# hooks
mkdir -p .git/hooks
printf '#!/bin/sh\necho "nope from hook" >&2\nexit 1\n' > .git/hooks/pre-commit; chmod +x .git/hooks/pre-commit
echo h > h.txt
check "failing pre-commit blocks commit" "! $T commit 'blocked' 2>/dev/null && [ \"\$(git log -1 --format=%s)\" != blocked ]"
check "hook output is reported" "{ $T commit 'blocked' 2>&1 || true; } | grep -q 'nope from hook'"
rm .git/hooks/pre-commit
printf '#!/bin/sh\necho "Signed-off-by: Hook" >> "$1"\n' > .git/hooks/commit-msg; chmod +x .git/hooks/commit-msg
$T commit "with hook" >/dev/null
check "commit-msg hook edits message" "git log -1 --format=%B | grep -q 'Signed-off-by: Hook'"
rm .git/hooks/commit-msg

# SSH transport through a fake ssh that runs the remote command locally
git init -q --bare "$W/remote.git"
cat > "$W/fake-ssh" <<'SSH'
#!/bin/sh
# drop ssh options, the host, then run the command locally
while [ $# -gt 1 ]; do case "$1" in -o|-p) shift 2;; *) shift;; esac; done
exec sh -c "$1"
SSH
chmod +x "$W/fake-ssh"
export GIT_SSH_COMMAND="$W/fake-ssh"
git remote add origin "git@example.com:$W/remote.git"
check "push over ssh" "$T push origin main && [ \"\$(git -C $W/remote.git rev-parse main)\" = \"\$(git rev-parse main)\" ]"
cd "$W"
check "clone over ssh" "$T clone git@example.com:$W/remote.git viassh 2>/dev/null && cd viassh && clean && [ -e x.txt ]"
cd "$W/viassh"; echo more > more.txt; $T commit "from clone" >/dev/null; $T push
cd "$W/r"; git config branch.main.remote origin; git config branch.main.merge refs/heads/main
check "pull over ssh" "$T pull >/dev/null && [ -e more.txt ]"
unset GIT_SSH_COMMAND

echo; echo "$pass passed, $fail failed"
[ $fail = 0 ]
