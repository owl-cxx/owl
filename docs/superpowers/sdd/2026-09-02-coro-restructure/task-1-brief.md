## Task 1: Dissolve `util/`

`util/` holds a driver, a combinator, two algorithms and the error model. Split it three ways.

**Files:**
- Move: `coro/include/coro/util/fmap.h` → `coro/include/coro/algo/fmap.h`
- Move: `coro/include/coro/util/combine.h` → `coro/include/coro/algo/combine.h`
- Move: `coro/include/coro/util/wait_all.h` → `coro/include/coro/algo/wait_all.h`
- Move: `coro/include/coro/util/result.h` → `coro/include/coro/result/result.h`
- Move: `coro/include/coro/util/as_result.h` → `coro/include/coro/result/as_result.h`
- Move: `coro/include/coro/util/sync_wait.h` → `coro/include/coro/run/sync_wait.h`
- Modify: `coro/include/coro/coro.h` (6 include lines)
- Modify: `coro/CMakeLists.txt` (FILE_SET)
- Modify: `owl/include/owl/http/response.h:14`
- Modify: 8 files under `coro/tests/`

**Interfaces:**
- Consumes: nothing
- Produces: the include paths `coro/algo/{fmap,combine,wait_all}.h`, `coro/result/{result,as_result}.h`, `coro/run/sync_wait.h`. Every later task uses these spellings. No symbol names change: `coro::fmap`, `coro::combine`, `coro::wait_all`, `coro::result`, `coro::as_result`, `coro::sync_wait` are all untouched.

- [ ] **Step 1: Create the directories and move the files with git**

```bash
cd /Users/professor/CLionProjects/owl/coro/include/coro
mkdir -p algo result run
git mv util/fmap.h      algo/fmap.h
git mv util/combine.h   algo/combine.h
git mv util/wait_all.h  algo/wait_all.h
git mv util/result.h    result/result.h
git mv util/as_result.h result/as_result.h
git mv util/sync_wait.h run/sync_wait.h
rmdir util
```

- [ ] **Step 2: Rewrite every reference to the old paths**

```bash
cd /Users/professor/CLionProjects/owl
rewrite() {
  grep -rl -- "$1" coro owl main.cpp \
      --include='*.h' --include='*.cpp' --include='CMakeLists.txt' \
    | xargs sed -i '' "s|$1|$2|g"
}
rewrite 'coro/util/fmap.h'      'coro/algo/fmap.h'
rewrite 'coro/util/combine.h'   'coro/algo/combine.h'
rewrite 'coro/util/wait_all.h'  'coro/algo/wait_all.h'
rewrite 'coro/util/result.h'    'coro/result/result.h'
rewrite 'coro/util/as_result.h' 'coro/result/as_result.h'
rewrite 'coro/util/sync_wait.h' 'coro/run/sync_wait.h'
```

- [ ] **Step 3: Verify no reference to `util/` survives**

```bash
grep -rn 'coro/util' coro owl main.cpp --include='*.h' --include='*.cpp' --include='CMakeLists.txt'
```

Expected: no output.

- [ ] **Step 4: Reorder the FILE_SET block to match the new tree**

In `coro/CMakeLists.txt`, the six rewritten `include/coro/...` lines are now scattered under the old `# util` grouping. Regroup them so the manifest reads in tree order — root, `concepts/` (added later), `algo/`, `result/`, `run/`, `executors/`, `io/`, `sync/`. Only line order changes; the set of files is identical.

- [ ] **Step 5: Prove nothing but includes changed**

```bash
cd /Users/professor/CLionProjects/owl
for pair in "util/fmap.h algo/fmap.h" "util/combine.h algo/combine.h" \
            "util/wait_all.h algo/wait_all.h" "util/result.h result/result.h" \
            "util/as_result.h result/as_result.h" "util/sync_wait.h run/sync_wait.h"; do
  set -- $pair
  old=$(git show HEAD:coro/include/coro/$1 | grep -v '^#include')
  new=$(grep -v '^#include' coro/include/coro/$2)
  [ "$old" = "$new" ] && echo "OK   $2" || echo "DIFF $2"
done
```

Expected: six `OK` lines. Any `DIFF` means content changed and must be investigated before continuing.

- [ ] **Step 6: Build and test**

```bash
cmake --preset Debug && cmake --build --preset Debug && ctest --preset Debug
```

Expected: same tests as `/tmp/coro-baseline.txt`, all passing.

- [ ] **Step 7: Commit**

```bash
git add -A coro owl
git commit -m "Dissolve coro/util into algo, result and run

util/ held a driver (sync_wait), a combinator (wait_all), two algorithms
(fmap, combine) and the error model (result, as_result). The name told a
reader nothing about which. Each header now sits under the role it serves.

Pure relocation: verified that every moved file differs from its original
only in #include lines."
```

---

