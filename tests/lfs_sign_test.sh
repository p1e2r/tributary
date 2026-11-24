#!/usr/bin/env bash
# Git LFS (against a local LFS server, cross-checked with the real git-lfs) and
# commit signing (verified with `git verify-commit`).
set -uo pipefail
T=${TRIBCLI:-$(pwd)/build-cli/tribcli}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
export GIT_CONFIG_GLOBAL="$W/gitconfig"
printf '[user]\n\tname = Test User\n\temail = test@example.com\n' > "$GIT_CONFIG_GLOBAL"
git init -q --bare -b main "$W/srv/remote.git"
git -C "$W/srv/remote.git" config http.receivepack true
python3 "$HERE/http_backend.py" "$W/srv" $PORT & SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
sleep 1
URL=http://127.0.0.1:$PORT/remote.git
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "ok   - $1"; else fail=$((fail+1)); echo "FAIL - $1"; fi; }
HAS_LFS=0; command -v git-lfs >/dev/null && HAS_LFS=1
[ $HAS_LFS = 1 ] && git lfs install --skip-repo >/dev/null

# ---------------------------------------------------------------- LFS: Tributary writes
cd "$W" && $T init a && cd a
git remote add origin $URL
$T lfs-track '*.bin'
head -c 300000 /dev/urandom > big.bin
echo "hello" > small.txt
check "*.bin is tracked by LFS" "[ \"\$($T is-lfs big.bin)\" = yes ] && [ \"\$($T is-lfs small.txt)\" = no ]"
$T commit "add big file" >/dev/null
BLOB=$(git rev-parse HEAD:big.bin)
check "commit stores a pointer, not the content" "git cat-file -p $BLOB | head -1 | grep -q 'version https://git-lfs.github.com/spec/v1'"
OID=$(sha256sum big.bin | cut -d' ' -f1)
check "pointer has the right oid and size" "git cat-file -p $BLOB | grep -q \"oid sha256:$OID\" && git cat-file -p $BLOB | grep -q 'size 300000'"
check "object stored in .git/lfs/objects" "[ -f .git/lfs/objects/${OID:0:2}/${OID:2:2}/$OID ]"
check "worktree keeps the real file and status is clean" "cmp -s big.bin .git/lfs/objects/${OID:0:2}/${OID:2:2}/$OID && [ -z \"\$($T status | grep -v '^branch')\" ]"
$T push origin main >/dev/null
check "push uploads the LFS object" "[ -f $W/srv/.lfs-objects/$OID ]"
check "the pushed blob is the pointer" "[ \"\$(git -C $W/srv/remote.git rev-parse main:big.bin)\" = $BLOB ]"

# ---------------------------------------------------------------- LFS: Tributary reads
cd "$W" && $T clone $URL b >/dev/null 2>&1 && cd b
check "clone downloads and smudges LFS files" "cmp -s big.bin $W/a/big.bin"
check "cloned repo status is clean" "[ -z \"\$($T status | grep -v '^branch')\" ]"
echo more >> big.bin
check "editing an LFS file shows as modified" "$T status | grep -q big.bin"
$T commit "grow big file" >/dev/null
$T push >/dev/null
OID2=$(sha256sum big.bin | cut -d' ' -f1)
check "second version uploaded" "[ -f $W/srv/.lfs-objects/$OID2 ]"
cd "$W/a"; git config branch.main.remote origin; git config branch.main.merge refs/heads/main
$T pull >/dev/null
check "pull fetches the new LFS version" "cmp -s big.bin $W/b/big.bin"

# ---------------------------------------------------------------- interop with git-lfs
if [ $HAS_LFS = 1 ]; then
    cd "$W" && GIT_TERMINAL_PROMPT=0 git -c lfs.url=$URL/info/lfs clone -q $URL real 2>$W/lfs-clone.err || cat $W/lfs-clone.err && cd real
    check "git-lfs reads what Tributary pushed" "cmp -s big.bin $W/b/big.bin"
    git config lfs.url $URL/info/lfs
    head -c 50000 /dev/urandom > other.bin
    git add other.bin && git commit -qm "git-lfs adds a file" && git push -q origin main 2>/dev/null
    cd "$W/a" && $T pull >/dev/null
    check "Tributary reads what git-lfs pushed" "cmp -s other.bin $W/real/other.bin"
    check "and status stays clean" "[ -z \"\$($T status | grep -v '^branch')\" ]"
fi

# ---------------------------------------------------------------- signing (OpenPGP)
if command -v gpg >/dev/null; then
    export GNUPGHOME="$W/gnupg"; mkdir -m 700 "$GNUPGHOME"
    gpg --batch --quiet --passphrase '' --quick-gen-key "Test User <test@example.com>" ed25519 sign never 2>/dev/null
    KEY=$(gpg --list-keys --with-colons test@example.com | awk -F: '/^fpr/ {print $10; exit}')
    cd "$W" && $T init s && cd s
    git config commit.gpgsign true; git config user.signingkey "$KEY"
    echo r > r.txt; $T commit "root" >/dev/null
    echo a > a.txt; $T commit "signed commit" >/dev/null
    check "commit carries a gpgsig header" "git cat-file -p HEAD | grep -q '^gpgsig -----BEGIN PGP SIGNATURE-----'"
    check "git verify-commit accepts it" "git verify-commit HEAD 2>/dev/null"
    echo b > b.txt; $T commit "second" >/dev/null
    $T reword HEAD~1 "signed commit, reworded" >/dev/null
    check "rewritten commits are re-signed" "git verify-commit HEAD 2>/dev/null && git verify-commit HEAD~1 2>/dev/null"
    check "reword kept history intact" "[ \"\$(git log -1 --format=%s HEAD~1)\" = 'signed commit, reworded' ] && [ -z \"\$(git fsck --strict 2>&1 | grep -E 'error|broken')\" ]"
    git config user.signingkey DEADBEEFNOTAKEY
    echo c > c.txt
    check "a broken signing key fails the commit loudly" "! $T commit 'should fail' 2>/dev/null"
    git config commit.gpgsign false
    check "unsigned when signing is off" "$T commit 'unsigned' >/dev/null && ! git cat-file -p HEAD | grep -q '^gpgsig'"
fi

echo; echo "$pass passed, $fail failed"
[ $fail = 0 ]
