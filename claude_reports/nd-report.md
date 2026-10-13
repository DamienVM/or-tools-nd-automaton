# Report: Non-deterministic automata for `AddAutomaton` in CP-SAT

Repo state: `98c165af62` (v9.15 + 14 commits), branch `stable`. (Written before any local
build existed; build status is tracked in §4 and §6-7.)

## 1. Supporting non-deterministic automata

### 1.1 Key insight up front

CP-SAT has **no automaton propagator**. The constraint is *always* expanded in presolve
(`ExpandAutomaton`, `ortools/sat/cp_model_expand.cc:1318-1627`) into per-time-step
Boolean encodings; if an unexpanded automaton ever reaches the constraint loader it is
a hard failure (`cp_model_loader.cc` has no `kAutomaton` case; the solver then logs
`"BUG: We will wrongly report INFEASIBLE now."`, `cp_model_solver_helpers.cc:1400-1417`).

The good news: **most of the pipeline is already relation-based and NFA-tolerant.**
Determinism is load-bearing in exactly four places, listed in §1.3. The semantics of an
NFA here is the natural one: the constraint is satisfied iff *there exists* an accepting
run of the label sequence.

### 1.2 Where determinism currently lives

**The proto** (`ortools/sat/cp_model.proto:283-302`, `AutomatonConstraintProto`) stores
the transition relation as three parallel arrays `transition_tail/head/label`. Nothing
structural forces determinism — only the doc comment at lines 291-292: *"No two outgoing
transitions from the same state can have the same label."* So **no proto change is
needed**; NFAs are already representable.

**None of the language APIs enforce determinism** — C++ `AddAutomaton`
(`cp_model.h:979-1020`, `cp_model.cc:1076-1118`), Python `add_automaton`
(`ortools/sat/python/cp_model.py:699-764`), Java, C#, Go are all pure proto builders.
The single hard gate is the model validator.

### 1.3 The four sites that must change

1. **The validator — the hard gate.** `ValidateAutomatonConstraint`
   (`ortools/sat/cp_model_checker.cc:556-603`) builds a
   `flat_hash_map<{tail,label}, head>` and rejects any collision:
   `"automaton: incompatible transitions ..."` for genuine non-determinism, and
   `"automaton: duplicate transition ..."` even for harmless exact duplicates.
   **Change:** drop the incompatible-transitions rejection (keep — or silently
   dedup — exact duplicates). This is a ~10-line change.

2. **The "light" expansion branch — the correctness break.** In `ExpandAutomaton`,
   each time step builds a 3-column table of surviving `(in_state, label, out_state)`
   triples, then picks one of two encodings by a pure size heuristic
   (`use_light_encoding = num_tuples > num_involved_variables`,
   `cp_model_expand.cc:1521-1531`):
   - The **light** encoding (`cp_model_expand.cc:1530-1563`) emits the functional-
     consistency 3-clause `in[s] ∧ label[a] ⇒ out[h]` for every triple. With an NFA
     where `(s,a)→h1` and `(s,a)→h2`, both clauses fire and force the run to be in two
     states at once — combined with the exactly-one structure of the out-state
     encoding this makes every NFA branch point **infeasible**. Unsound for NFAs.
   - The **heavy** tuple-literal encoding (`cp_model_expand.cc:1566-1616`) creates one
     Boolean per table row inside an `exactly_one`, then channels the chosen row to
     the in-state/label/out-state encodings via `LinkLiteralsAndValues`
     (`cp_model_expand.cc:1242-1282`). A row is *chosen*, so two rows sharing `(s,a)`
     is just a disjunctive choice — **this is exactly NFA semantics and needs no
     structural change.** (It is morally a table encoding, and
     `TableConstraintProto` has no functional-dependency requirement at all.)

   **Change:** per time step, detect whether the surviving triple set is functional
   (no duplicate `(in_state, label)` pair — cheap hash-set pass over the tuples).
   If functional, keep everything as today; if not, force the heavy encoding.
   Optionally later: generalize the light branch's Part 2 to
   `in[s] ∧ label[a] ⇒ (out[h1] ∨ out[h2] ∨ …)`, which stays sound but propagates
   more weakly than the current 3-clause form.

   Two details to re-audit in the heavy branch (both currently justified by
   determinism but survive relaxation): the comment at `cp_model_expand.cc:1573-1575`
   claims the `exactly_one` over row literals is implied — with an NFA it becomes a
   genuine witness-selection constraint (it is added explicitly anyway, so this is a
   comment fix); and the 2-tuple shortcut using `v` / `¬v` (`:1567-1571`) forces
   exactly one of the two rows, which remains correct for existential run semantics.

3. **The solution feasibility checker.** `AutomatonConstraintIsFeasible`
   (`cp_model_checker.cc:1601-1631`) walks the automaton with a single
   `{tail,label}→head` map — on an NFA the last duplicate silently wins.
   **Change:** replace the walk with a subset simulation (track a
   `flat_hash_set<int64_t>` of current states; accept iff the final set intersects
   `final_states`). Still linear in `n × transitions`.

4. **Solution-hint crushing.** `SolutionCrush::SetAutomatonExpandedVars`
   (`ortools/sat/solution_crush.cc:620-655`) does the same deterministic walk to
   assign values to the expansion's state/transition Booleans from a user hint.
   **Change:** choose a witness path — forward subset simulation over the hinted
   labels, then backward path extraction (or just bail out of hint crushing when a
   step is non-functional; hints are best-effort).

### 1.4 What needs *no* change

- **Reachability pruning** — `PropagateAutomaton` (`ortools/sat/cp_model_table.cc:334-381`)
  is already a set-of-states forward/backward pass over the transition *relation*;
  works verbatim for NFAs.
- **Presolve** — `CpModelPresolver::PresolveAutomaton`
  (`cp_model_presolve.cc:7752-7796`) is purely reachability-based.
- **Copy / remap / fingerprint plumbing** (`cp_model_copy.cc:649-681`,
  `cp_model_utils.cc:213-219, 406-412, 858-870`) — verbatim on the three arrays.
- **The proto and all language APIs** — only doc comments need updating
  (`cp_model.proto:291-292`, `cp_model.h:979-1003`, `cp_model.py` docstring, etc.).

### 1.5 Epsilon transitions

Not expressible in the current scheme: the expansion loop runs exactly
`n = exprs_size()` steps and every step consumes one label
(`cp_model_expand.cc:1355`); the proto has no "no-op" step notion.
The cheap, standard route: **epsilon-closure preprocessing** — pick a reserved label
value (or a small proto extension), compute the closure of the transition relation
and the effective final-state set before expansion, then proceed with a plain
(possibly non-deterministic) automaton. Variable-length unrolling would be a much
bigger change and is not recommended.

### 1.6 Tests to update / add

- `cp_model_checker_test.cc:606-624` `IncompatibleAutomatonTransitions` currently
  expects rejection — must be inverted (non-determinism now valid);
  `:626-644` `DuplicateAutomatonTransitions` stays or becomes a dedup test.
- `cp_model_expand_test.cc:1085-1497` has ~10 `AutomatonExpandTest` cases that
  enumerate *all* solutions and compare against a hand-written set — the ideal
  harness. Add NFA cases: a branch point `(s,a)→{h1,h2}` where only one branch
  reaches a final state; a case where both do (solution counts must not double —
  the constraint is on the label sequence, and duplicate label sequences are not
  distinct solutions since the row-selection Booleans are internal); a case sized to
  fall into each encoding branch.
- End-to-end: `table_test.cc:397-553` (`AutomatonTest.*`), plus a text-proto model
  derived from `ortools/sat/fuzz_testdata/AutomatonModel` run through `sat_runner`.

### 1.7 Suggested implementation order

1. Relax `ValidateAutomatonConstraint` (keep rejecting nothing but malformed input;
   optionally warn/dedup exact duplicates).
2. Fix `AutomatonConstraintIsFeasible` → subset simulation (so the debug solution
   checker can't mis-verify).
3. Gate the light encoding on per-step functionality in `ExpandAutomaton`.
4. Handle `solution_crush.cc` (witness path or bail out).
5. Update doc comments in proto + all wrappers; add tests; optionally add an
   automaton sample (none exists today).
6. (Later, separate change) epsilon-closure preprocessing.

Steps 1–4 are each small and independently testable; the whole core change is on the
order of a few hundred lines including tests.

## 2. How to build and run CP-SAT from this checkout

You do **not** need to write a new C++ script just to run CP-SAT — the repo ships
`sat_runner`, a CLI that solves a serialized `CpModelProto`, plus C++/Python samples.
You do need to build once.

### 2.1 State of `hornet` when this was written (Ubuntu 22.04 — OUTDATED, see §4)

- No `build/` directory and no Bazel output trees — nothing has been built here yet.
  (The only untracked artifact, `cmake/samples/java/target/`, is Maven output from the
  Java integration template, unrelated to CP-SAT.)
- Installed: CMake 3.29 (requirement is >= 3.24), `g++`, `make`, `python3`.
- **Bazel is not installed.** So the practical path is CMake; alternatively install
  `bazelisk` if you prefer Bazel's incremental rebuilds (recommended for iterating on
  `ortools/sat/*.cc` since Python/Java wrappers aren't needed for C++ work).

### 2.2 CMake build (works today on this machine)

From `cmake/docs/cpp.md`:

```sh
cd ~/Desktop/or-tools
cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_DEPS=ON
cmake --build build --config Release -j$(nproc)
```

`-DBUILD_DEPS=ON` fetches and builds all dependencies (abseil, protobuf, ...), so the
first build takes a long time; later rebuilds after editing `ortools/sat/*.cc` are
incremental.

Run tests: `ctest --test-dir build -R "cxx_.*"`.

### 2.3 Running CP-SAT on a model without writing any C++: `sat_runner`

`ortools/sat/sat_runner.cc` is the only `main()` under `ortools/sat/` and does exactly
this. `--input` accepts a `CpModelProto` in binary **or text** format (also `.cnf`,
`.opb`). Useful flags: `--params` (text `SatParameters`, e.g.
`--params="log_search_progress:true"`), `--output`, `--hint_file`.

```sh
cmake --build build --target sat_runner
./build/bin/sat_runner \
    --input=ortools/sat/fuzz_testdata/AutomatonModel \
    --params="log_search_progress:true"
```

Note: `ortools/sat/fuzz_testdata/AutomatonModel` is a ready-made **text CpModelProto
containing an `automaton` constraint** — an ideal starting point for testing a
non-determinism change without touching any client code. The rest of
`ortools/sat/fuzz_testdata/` (CircuitModel, ElementModel, ...) is a similar corpus.

So the minimal edit-test loop for this project is:

```sh
# edit ortools/sat/cp_model_expand.cc (or wherever the change lands)
cmake --build build --target sat_runner -j$(nproc)
./build/bin/sat_runner --input=my_nondeterministic_automaton_model.pb.txt
```

where the model file is a hand-written text proto — no C++ needed.

### 2.4 C++ samples (if you do want a small program)

Samples live in `ortools/sat/samples/*.cc` (32 files; e.g. `simple_sat_program.cc`,
`cp_sat_example.cc`, `search_for_all_solutions_sample_sat.cc`). There is currently
**no automaton C++ sample** — writing one (calling `cp_model.AddAutomaton(...)`) would
be a natural companion to this change. Each `.cc` becomes an executable of the same
name via `add_cxx_sample` (`cmake/cpp.cmake:882`), gated on `BUILD_SAMPLES` /
`BUILD_CXX_SAMPLES` (ON by default with `BUILD_DEPS=ON`):

```sh
cmake --build build --target simple_sat_program
./build/bin/simple_sat_program
```

### 2.5 Bazel alternative (needs bazelisk installed)

From `bazel/README.md` (Bazel >= 5.4, bzlmod via `MODULE.bazel`):

```sh
bazel run  -c opt //ortools/sat:sat_runner -- --input=/abs/path/model.pb.txt
bazel test -c opt //ortools/sat/samples:simple_sat_program_test --test_output=all
```

Samples are declared as `cc_test`, hence the `_test` suffix. Bazel does **not** build
the Python/Java/.NET wrappers.

### 2.6 Python bindings from this source tree (optional)

CMake only. From `cmake/docs/python.md`:

```sh
cmake -S. -Bbuild -DBUILD_PYTHON=ON
cmake --build build --target python_package -v
python3 -m pip install --user --find-links=build/python/dist ortools
```

A C++ change under `ortools/sat/` is picked up by rebuilding `python_package`; the
Python CP-SAT layer itself is `ortools/sat/python/cp_model.py`
(`add_automaton` lives there).

## 3. Summary of recommendations

1. Build once with CMake (`-DBUILD_DEPS=ON`); iterate via the `sat_runner` target.
2. Test with hand-written text `CpModelProto` files (start from
   `ortools/sat/fuzz_testdata/AutomatonModel`) — no C++ driver program required.
3. Only build the Python package if you want to exercise the change through
   `cp_model.add_automaton`.

## 4. Build status on `hornet` (Ubuntu 22.04, 2026-08-20)

The build described in §2.2 is **done**. Configured with
`cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_DEPS=ON` and compiled the
`sat_runner` target with `-j12`; both succeeded with no errors. The binary is at
`build/bin/sat_runner`. The bundled dependencies (abseil, protobuf, Boost, ...)
live under `build/_deps/` and never rebuild — do not delete `build/` and do not
re-run the configure step.

### 4.1 Verification

- `ortools/sat/fuzz_testdata/AutomatonModel` (deterministic) solves to
  `OPTIMAL` in ~7 ms.
- `nfa_test_model.pb.txt` (repo root, untracked) is a copy of that model with one
  extra transition making state 0 non-deterministic on label 0
  (`0 --(0)--> 1` and `0 --(0)--> 2`). It currently fails exactly at site #1
  from §1.3:

  ```
  Invalid model: automaton: incompatible transitions 0 --(0)--> 2 and 0 --(0)--> 1
  status: MODEL_INVALID
  ```

  This was the baseline regression check; it now reports `OPTIMAL` (see §5).

### 4.2 Edit-test loop

```sh
# edit e.g. ortools/sat/cp_model_checker.cc or cp_model_expand.cc
cmake --build build --target sat_runner -j12
./build/bin/sat_runner --input=nfa_test_model.pb.txt --params="log_search_progress:true"
```

Incremental rebuild after a `.cc` edit is well under a minute; a header edit
rebuilds its includers; a `cp_model.proto` edit rebuilds most of `ortools/sat/`
(but §1.2 concluded no structural proto change is needed).

The repo's own unit tests also build here, and they are a much better harness
than `sat_runner` for this work:

```sh
cmake --build build --target sat_cp_model_expand_test sat_cp_model_checker_test -j12
./build/bin/sat_cp_model_expand_test --gtest_filter="*Automaton*"
```

## 5. Implementation status (2026-08-21)

Steps 1–5 of §1.7 are **implemented and passing**. Diff: 9 files,
~220 insertions / ~50 deletions.

### 5.1 What was changed

1. **Validator** (`cp_model_checker.cc:580-598`) — the
   `flat_hash_map<{tail,label}, head>` became a
   `flat_hash_set<{tail,label,head}>`. Non-determinism is accepted; exact
   duplicate transitions are still rejected with the same message as before.

2. **Solution feasibility checker** (`cp_model_checker.cc:1593-1631`) —
   the deterministic walk became a subset simulation over
   `{tail,label} -> vector<head>`, accepting iff the final state set meets
   `final_states`.

3. **Expansion** (`cp_model_expand.cc:1531-1546`) — a per-time-step
   `is_functional` check (hash set over `(in_state, label)` pairs) now gates
   `use_light_encoding`. Non-functional steps fall through to the existing
   heavy tuple-literal encoding, which is NFA-correct as-is. The misleading
   "exactly_one is implied" comment (`:1593-1597`) was corrected.

4. **Hint crushing** (`solution_crush.cc:620-687`) — forward subset simulation
   records the reachable state set per step, then a backward pass extracts one
   witness accepting run. Bails out (leaving the expanded variables alone) when
   the hinted labels are not accepted — which the old code did *not* check, it
   assigned values from a walk that need not end in a final state.

5. **`SolutionCrush::TransitionVar` gained a `transition_head` field**
   (`solution_crush.h:270-277`, set at `cp_model_expand.cc:1589` and `:1611`).
   This was **not** in the original plan and is required: the struct keyed
   transition literals by `(time, tail, label)`, which is not unique for an NFA,
   so two distinct tuple literals of the same `exactly_one` would both be hinted
   `true`.

6. **Doc comments** — `cp_model.proto:290-295`, `cp_model.h` `AddAutomaton`,
   `python/cp_model.py` `add_automaton`. No other wrapper documented the
   determinism requirement, and no structural proto change was needed, as
   predicted.

### 5.2 Tests

- `cp_model_checker_test.cc`: `IncompatibleAutomatonTransitions` inverted into
  `NonDeterministicAutomatonTransitionsAreValid` (expects no error).
  `DuplicateAutomatonTransitions` untouched and still passing.
- `cp_model_expand_test.cc`: three new `AutomatonExpandTest` cases, all with
  solution hints so that the harness's
  `debug_crash_if_presolve_breaks_hint` exercises the crush path:
  - `NonDeterministicBranch` — branch point where only one branch reaches a
    final state.
  - `NonDeterministicWithSeveralAcceptingRuns` — the sequence `(0,0)` has two
    distinct accepting runs and must yield exactly one solution.
  - `NonDeterministicDenseTransitions` — 16 transitions over 4 states and
    3 labels, i.e. `num_tuples > num_involved_variables`, so it lands in the
    light-encoding size regime and exercises the new gate.

All green: expand 73/73, checker 40/40, table 12/12, presolve 200/200,
presolve_random 15000/15000, solver 161/161.

**The gate is load-bearing** — verified by temporarily removing
`is_functional &&` and rebuilding: `NonDeterministicDenseTransitions` then dies
with `F cp_model_solver.cc:3026] Presolve broke a feasible hint`, confirming the
light encoding really is unsound for NFAs rather than merely suspect.

### 5.3 Correction to §1.6: solution counts *do* double

§1.6 claimed duplicate label sequences "are not distinct solutions since the
row-selection Booleans are internal". That is **wrong at the raw solver level**.
`enumerate_all_solutions` enumerates assignments over every variable of the
presolved model, expansion-created Booleans included, so an ambiguous NFA
reports one solution *per accepting run*:

```sh
./build/bin/sat_runner --input=nfa_ambiguous_model.pb.txt \
    --params="enumerate_all_solutions:true,log_search_progress:true,num_workers:1"
# Solutions (2)   <- one label sequence, two accepting runs
```

(Confirmed to be the general mechanism: a model with one free auxiliary
variable likewise doubles.) It is not specific to this change — it is inherent
to expanding an existential choice into the same model — and it does **not**
affect the unit tests, which collect solutions into a
`btree_set<vector<int>>` over the *original* variables only
(`cp_model_expand_test.cc:53-64`), so duplicates collapse there.

Still worth deciding before upstreaming: whether counting runs rather than
label sequences is acceptable for `enumerate_all_solutions` on NFAs, or whether
it needs documenting as a caveat. Deduplicating in general would require
determinization, which is exponential.

### 5.4 Not done

Epsilon transitions (§1.5) — still unimplemented, still recommended as a
separate change via epsilon-closure preprocessing. No automaton C++ sample was
added.

## 6. Python package from this checkout 

The wheel `build/python/dist/ortools-9.15.6769-cp310-cp310-linux_x86_64.whl`
builds and, installed in the venv below, solves an NFA through
`cp_model.add_automaton` (pip's stock 9.14 returns `MODEL_INVALID` on the same
model). The working recipe:

```sh
# one-time setup
sudo apt install swig
python3 -m venv ~/.venvs/ortools-dev
~/.venvs/ortools-dev/bin/pip install "protobuf==6.33.6" mypy mypy-protobuf \
    typing-extensions virtualenv wheel setuptools absl-py numpy pandas \
    immutabledict pytest
cmake -S. -Bbuild -DBUILD_PYTHON=ON \
    -DPython3_EXECUTABLE=$HOME/.venvs/ortools-dev/bin/python

# each rebuild — the PATH prefix is required, see below
PATH="$HOME/.venvs/ortools-dev/bin:$PATH" \
    cmake --build build --target python_package -j12
~/.venvs/ortools-dev/bin/pip install --force-reinstall --no-deps \
    build/python/dist/ortools-*.whl
```

Pitfalls hit on the way (both are protobuf gencode-vs-runtime version errors
from `protoc-gen-mypy`):

1. `pip install --user` had put `mypy-protobuf` (gencode 6.32.1) next to
   `protobuf 6.31.1` in `~/.local` — runtime older than gencode is a hard
   error. Fixed by the venv with `protobuf==6.33.6` (matches the checkout's
   protoc 33.1; don't let pip resolve to protobuf 7.x).
2. `-DPython3_EXECUTABLE` does NOT fix the plugin: protoc resolves
   `protoc-gen-mypy` from PATH, and `~/.local/bin` shadowed the venv's copy.
   Hence the `PATH=` prefix on every `python_package` build.

Also: with `BUILD_PYTHON=ON` in the cache, every CMake dep of
`search_python_module` (mypy, mypy-protobuf, virtualenv, ...) must already be
in the venv — the auto-install fallback uses `pip install --user`, which
fails inside a venv.

Two more, found on 2026-09-01 while shipping a Python API change (they do not
bite a C++-only change, which is why they were missed the first time):

3. **Editing `ortools/sat/python/cp_model.py` requires a CMake re-configure.**
   It is installed by `file(COPY ...)` at `cmake/python.cmake:410`, and
   `file(COPY)` runs at *configure* time, not build time. So
   `cmake --build build --target python_package` happily rebuilds the wheel
   from the stale copy in `build/python/ortools/sat/python/cp_model.py`, with
   no warning. Fix:

   ```sh
   cmake -S. -Bbuild          # cached args are reused; deps are NOT refetched
   ```

   This is safe — it does not touch `build/_deps` — and takes ~8 s.

4. **`python_package` is stamp-gated and will silently skip the wheel.** The
   target's steps are guarded by `build/python/{dist,ortools,pybind11,stub}_timestamp`.
   If the C++/Python sources changed but a stamp is newer, the target jumps
   straight to "Create venv and install ortools" and reinstalls the *old*
   wheel. Symptom: `build/python/dist/*.whl` keeps its old mtime. Fix:

   ```sh
   rm -f build/python/dist_timestamp build/python/ortools_timestamp
   PATH="$HOME/.venvs/ortools-dev/bin:$PATH" \
       cmake --build build --target python_package -j12
   ```

   Do **not** just `rm -rf build/python/dist` — the target's last step is
   `pip install --find-links=build/python/dist ortools==<version>`, which then
   fails against PyPI with "Could not find a version that satisfies the
   requirement".

So the reliable full recipe for a change that touches Python:

```sh
cmake -S. -Bbuild
rm -f build/python/dist_timestamp build/python/ortools_timestamp
PATH="$HOME/.venvs/ortools-dev/bin:$PATH" \
    cmake --build build --target python_package -j12
~/.venvs/ortools-dev/bin/pip install --force-reinstall --no-deps \
    build/python/dist/ortools-*.whl
```

## 7. Deploying the wheel to Arrhenius (works, 2026-08-21)

The wheel from §6 runs on the Arrhenius **CPU partition** (login node:
AlmaLinux 9.8, glibc 2.34, x86_64; the GPU partition is GH200/aarch64 and can
never run this wheel — irrelevant anyway, CP-SAT is CPU-only). Verified: import
OK, NFA models solve to `OPTIMAL` in the project venv
`/nobackup/proj/disk/dfsplan/personal/damvan/CCBP/.venv` (a **uv-managed** venv:
it has no pip — install with `uv pip install`, not `pip`; the bare `pip` there
silently falls through to the system Python 3.9 pip, which rejects cp310 wheels
as "not supported on this platform").

### 7.1 The problem

```
ImportError: /lib64/libstdc++.so.6: version `GLIBCXX_3.4.30' not found
             (required by .../ortools/.libs/libortools.so.9)
```

The wheel is built on Ubuntu 22.04 whose libstdc++ is GCC 12's
(`GLIBCXX_3.4.30`); AlmaLinux 9 ships GCC 11's (`GLIBCXX_3.4.29`). glibc itself
is fine (2.34 ≥ the 2.34 the wheel needs) — only the C++ runtime is one notch
too old. `auditwheel show` rates the wheel `manylinux_2_35`; el9 needs
`manylinux_2_34`.

Dead ends, for the record:

- `module load GCC/12.3.0` does not help: this site's EasyBuild is
  rpath-based and sets no `LD_LIBRARY_PATH`; also, the module tree the login
  node suggests first resolves to the `el9_gh200` (ARM!) software stack.
- Pointing `LD_LIBRARY_PATH` at
  `/software/sse2/el9_epyc9005/easybuild/prefix/software/GCCcore/12.3.0/lib64`
  (the x86-64 GCC 12 runtime) *does* work, but ties the venv to a software-tree
  path NSC may reshuffle. Used temporarily, then removed.
- Naively copying `libstdc++.so.6` into the wheel's `ortools/.libs/` does NOT
  work despite `RUNPATH $ORIGIN`: `cp_model.py` imports pandas/numpy before the
  ortools extension, numpy's extensions load the system `libstdc++.so.6` first,
  and the dynamic loader reuses an already-loaded soname for every later
  request — the copy in `.libs` is never consulted.
- `auditwheel repair --plat manylinux_2_34_x86_64` refuses ("too-recent
  versioned symbols"): auditwheel never grafts whitelisted libs like libstdc++.

### 7.2 The fix that works: soname-mangled bundling (patchelf)

Same technique auditwheel uses for non-whitelisted libraries, done manually.
On the build machine, on the unpacked wheel:

```sh
python -m wheel unpack ortools-*.whl -d . && cd ortools-9.15.6769
cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.30 ortools/.libs/libstdcxx-ortnfa.so.6
patchelf --set-soname libstdcxx-ortnfa.so.6 ortools/.libs/libstdcxx-ortnfa.so.6
for so in $(find . -name "*.so*" -type f); do
  patchelf --print-needed "$so" 2>/dev/null | grep -qx "libstdc++.so.6" &&
    patchelf --replace-needed libstdc++.so.6 libstdcxx-ortnfa.so.6 "$so"
done            # 89 libraries patched
cd .. && python -m wheel pack ortools-9.15.6769 -d .   # regenerates RECORD
```

The unique soname makes collision with the system libstdc++ impossible; the
wheel's own RUNPATHs (`$ORIGIN...`/`.libs`) find the bundled copy. ALL 89 libs
must be patched consistently — mixing two C++ runtimes among the wheel's own
libraries would break exception propagation. numpy et al. still load the system
libstdc++; two differently-named copies in one process is fine (no shared
C++ objects cross that boundary).

Verified on Arrhenius with empty `LD_LIBRARY_PATH`: `/proc/self/maps` shows
ortools using `.libs/libstdcxx-ortnfa.so.6` and numpy using
`/usr/lib64/libstdc++.so.6.0.29`.

### 7.3 Where things stand

- `build/python/dist/ortools-9.15.6769-*.whl` on hornet IS the patched,
  self-contained wheel — send it to any x86_64 / Python 3.10 / glibc ≥ 2.34
  machine as-is. **Rebuilding `python_package` overwrites it with an unpatched
  wheel** — re-run §7.2 after every rebuild (patchelf is in
  `~/.venvs/ortools-dev/bin`).
- The old, unpatched wheel still sits on the cluster at
  `.../CCBP/tools/or-tools-nd-auotmata/` — replace it with the patched one
  before ever reinstalling from there.
- The `LD_LIBRARY_PATH` workaround lines added to the venv's `activate` on
  2026-08-21 have been removed again; no environment setup is needed.
