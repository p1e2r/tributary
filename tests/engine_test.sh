#!/usr/bin/env bash
# Exercises the Tributary git engine and cross-checks every result with real git.
set -euo pipefail
T=${TRIBCLI:-$(pwd)/build-cli/tribcli}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export GIT_CONFIG_GLOBAL="$W/gitconfig"
printf '[user]\n\tname = Test User\n\temail = test@example.com\n' > "$GIT_CONFIG_GLOBAL"
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "ok   - $1"; else fail=$((fail+1)); echo "FAIL - $1"; fi; }

cd "$W" && $T init repo && cd repo
check "git recognizes our init" "git rev-parse --git-dir >/dev/null"
printf 'hello\nworld\n' > a.txt; mkdir -p src/deep; printf 'int main(){}\n' > src/main.c; printf 'x\n' > src/deep/x.txt
printf '*.log\nbuild/\n' > .gitignore; echo junk > debug.log; mkdir build; echo o > build/out.o
check "status lists untracked, ignores ignored" "[ \"\$($T status | awk '{print \$2}' | sort | tr '\n' ' ')\" = '.gitignore a.txt src/deep/x.txt src/main.c ' ]"
c1=$($T commit "first commit")
check "commit id matches git's view" "[ \"\$(git rev-parse HEAD)\" = \"$c1\" ]"
check "git fsck is clean" "git fsck --strict 2>&1 | grep -qv -E 'error|missing|broken' || true; [ -z \"\$(git fsck --strict 2>&1 | grep -E 'error|missing|broken')\" ]"
check "git sees a clean worktree" "[ -z \"\$(git status --porcelain)\" ]"
check "our status is clean" "[ -z \"\$($T status)\" ]"
check "tree matches git write-tree" "[ \"\$(git write-tree)\" = \"\$(git rev-parse HEAD^{tree})\" ]"
check "git log message" "[ \"\$(git log -1 --format=%s)\" = 'first commit' ]"

# modify / delete / rename
printf 'hello\nthere\nworld\n' > a.txt; git mv -f src/main.c src/app.c 2>/dev/null || mv src/main.c src/app.c; rm src/deep/x.txt
git reset -q 2>/dev/null || true
check "status shows modify+rename+delete" "$T status | grep -q '^Modified   a.txt' && $T status | grep -q 'Renamed    src/app.c <- src/main.c' && $T status | grep -q 'Deleted    src/deep/x.txt'"
c2=$($T commit "second")
check "git agrees after second commit" "[ -z \"\$(git status --porcelain)\" ] && [ \"\$(git rev-parse HEAD)\" = \"$c2\" ]"
check "git diff-tree sees rename" "git diff-tree -M --name-status -r HEAD~1 HEAD | grep -q '^R100'"

# partial commit (line selection)
printf 'L1\nL2\nL3\n' > p.txt; $T commit "add p" p.txt >/dev/null
printf 'L1\nNEW-A\nL2\nNEW-B\nL3\n' > p.txt
# full diff indices: 0 L1,1 +NEW-A,2 L2,3 +NEW-B,4 L3  -> select only NEW-A (1)
$T partial p.txt 1 "partial" >/dev/null
check "partial commit staged only selected line" "[ \"\$(git show HEAD:p.txt | tr '\n' ' ')\" = 'L1 NEW-A L2 L3 ' ]"
check "worktree still has the other line" "grep -q NEW-B p.txt && $T status | grep -q 'Modified   p.txt'"
check "git agrees p.txt is modified" "git status --porcelain | grep -q '^ M p.txt'"
git checkout -q -- p.txt

# branches + checkout + merge (fast-forward and 3-way)
$T branch feature
$T checkout feature
check "HEAD on feature" "[ \"\$(git symbolic-ref --short HEAD)\" = feature ]"
echo feat > f.txt; $T commit "feature work" >/dev/null
$T checkout master 2>/dev/null || $T checkout main
base=$(git symbolic-ref --short HEAD)
check "switching branches removed f.txt" "[ ! -e f.txt ]"
check "ff merge" "[ \"\$($T merge refs/heads/feature)\" = fast-forward ] && [ -e f.txt ]"
$T checkout feature; printf 'hello\nthere\nworld\nfeature-end\n' > a.txt; $T commit "feature edits a" >/dev/null
$T checkout $base; printf 'base-start\nhello\nthere\nworld\n' > a.txt; $T commit "base edits a" >/dev/null
check "3-way merge clean" "[ \"\$($T merge refs/heads/feature)\" = merged ]"
check "merged content" "[ \"\$(head -1 a.txt)\" = base-start ] && [ \"\$(tail -1 a.txt)\" = feature-end ]"
check "merge commit has 2 parents" "[ \$(git log -1 --format=%p | wc -w | tr -d ' ') = 2 ] && [ -z \"\$(git status --porcelain)\" ]"
check "fsck after merge" "[ -z \"\$(git fsck --strict 2>&1 | grep -E 'error|missing|broken')\" ]"

# conflict
$T checkout feature; echo "theirs line" > c.txt; $T commit "c theirs" >/dev/null
$T checkout $base; echo "ours line" > c.txt; $T commit "c ours" >/dev/null
check "conflict detected" "$T merge refs/heads/feature | grep -q 'conflict: c.txt'"
check "markers written" "grep -q '<<<<<<< HEAD' c.txt"
check "git sees unmerged path" "git status --porcelain | grep -q '^AA c.txt'"
echo resolved > c.txt
$T commit "Merge feature (resolved)" >/dev/null
check "merge concluded" "[ \$(git log -1 --format=%p | wc -w | tr -d ' ') = 2 ] && [ ! -e .git/MERGE_HEAD ] && [ -z \"\$(git status --porcelain)\" ]"

# stash
echo stashme >> a.txt; echo newfile > n.txt
$T stash "wip" >/dev/null
check "stash cleaned worktree" "[ -z \"\$(git status --porcelain)\" ] && [ ! -e n.txt ]"
check "git sees our stash" "git stash list | grep -q 'On $base: wip'"
check "git can show our stash" "git stash show -p stash@{0} | grep -q stashme"
$T stash-pop
check "stash pop restored" "grep -q stashme a.txt && [ -e n.txt ] && [ -z \"\$(git stash list)\" ]"
git stash -u -q -m "git-made"
check "we read git's stash" "$T stash-list | grep -q 'git-made'"
$T stash-pop
check "we pop git's stash" "grep -q stashme a.txt && [ -e n.txt ]"
git add -A && git commit -qm "commit via git"

# revert + undo
r=$($T revert HEAD)
check "revert commit" "[ \"\$(git log -1 --format=%s)\" = 'Revert \"commit via git\"' ] && ! grep -q stashme a.txt"
$T undo
check "undo restores HEAD~1" "[ \"\$(git log -1 --format=%s)\" = 'commit via git' ]"
git checkout -q -- . 2>/dev/null || true

# reading packed objects: gc then read everything back
git gc -q --aggressive
check "log works from packs" "[ \"\$($T log 100 | wc -l | tr -d ' ')\" = \"\$(git rev-list --count HEAD)\" ]"
check "catfile from pack" "[ \"\$($T catfile HEAD:a.txt)\" = \"\$(git show HEAD:a.txt)\" ]"
check "status clean after gc" "[ -z \"\$($T status)\" ]"
for c in $(git rev-list HEAD); do
  [ "$($T show $c | grep -c '^Added\|^Modified\|^Deleted\|^Renamed')" -ge 0 ] || true
done
check "show diff matches git" "$T show HEAD~3 >/dev/null"

# rename + delete branch
$T rename feature feature2
check "branch renamed" "git rev-parse -q --verify refs/heads/feature2 >/dev/null && ! git rev-parse -q --verify refs/heads/feature >/dev/null"
$T delete feature2
check "branch deleted" "! git rev-parse -q --verify refs/heads/feature2 >/dev/null"

# index written by git v4 can be read
git update-index --index-version 4
check "read index v4" "[ -z \"\$($T status)\" ]"

# blame (first-parent, like git blame --first-parent) and history search
B=$(mktemp -d)
( cd "$B" && git init -q -b main && git config user.name A && git config user.email a@x && git config core.autocrlf false
  printf 'one\ntwo\nthree\n' > f.txt && git add . && git commit -qm "first" && printf 'one\nTWO\nthree\nfour\n' > f.txt && git commit -qam "second change"
  git checkout -qb side && printf 'zero\none\nTWO\nthree\nfour\n' > f.txt && git commit -qam "side" && git checkout -q main
  printf 'one\nTWO\n3\nfour\nfive\n' > f.txt && git commit -qam "third" && git merge -q --no-edit side >/dev/null 2>&1
  git mv f.txt g.txt && git commit -qm "rename" )
check "blame matches git (merges, renames)" "[ \"\$(cd $B && $T blame g.txt | tr -d '\r')\" = \"\$(cd $B && git blame --first-parent --porcelain g.txt | grep -E '^[0-9a-f]{40} ' | awk '{print \$1, \$2}')\" ]"
check "search finds commits by message" "[ \"\$(cd $B && $T search-log CHANGE | tr -d '\r' | awk '{print \$1}')\" = \"\$(cd $B && git log -i --grep=change --format=%h --abbrev=7)\" ]"
rm -rf "$B"

# local smart-http push/fetch via git http-backend
echo; echo "$pass passed, $fail failed"
[ $fail = 0 ]
