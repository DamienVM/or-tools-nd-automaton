# Report: weak landmark orderings — cost of a `first`/`last` decomposition in CP-SAT, and whether that changes the case for a dedicated constraint

Written 2026-09-12. Companion to `landmark-ordering-report.md` (strong orderings and
`FirstOccurrenceConstraintProto`), `not-before-after-report.md` (the `transitions`
constraint) and `nd-report.md` (non-deterministic automata). CCBP at `~/Desktop/CCBP`,
or-tools at `~/Desktop/or-tools` branch `stable`.

**The question.** The landmark graph carries two kinds of ordering edge, *strong* and
*weak*. Only strong edges are posted today. MiniCP-BP handles weak edges with a dedicated
propagator, `SetFirstLastOccurrence`, that computes the first *and* last occurrence of a
landmark's action set in one constraint. What would it cost to decompose that in CP-SAT,
and does it strengthen the case for a new CP-SAT constraint?

---

## 0. Summary

**Three findings, in decreasing order of how much they should change your plans.**

1. **The pipeline never produces a weak edge.** `lm_rhw()` only emits `NATURAL` and
   `GREEDY_NECESSARY` orderings; weak edges require `REASONABLE`, which only
   `lm_reasonable_orders_hps` creates. All four landmark graphs in `out/` have exactly
   zero weak edges. Until the Scorpion invocation changes, `weakStrongLess` is provably
   identical to `strongLess`, in MiniCP-BP as well as in any CP-SAT port (§1).

2. **The decomposition is nearly free.** `last_L` reuses the `membership` Booleans
   already built for `first_L`, so the `n` membership tables of `d` tuples each — which
   are the entire cost of the encoding — are not duplicated. Measured at the real
   parameters, with *every* landmark needing a `last`: input proto +4.4%, **presolved
   model +2.4% variables, peak RSS unchanged** (§3).

3. **Therefore this does not move the dedicated-constraint decision at all.** Everything
   in `landmark-ordering-report.md` §5-§7 stands unchanged; weak orderings add ~0 to the
   term that is binding. The one amendment is to the *signature*: if
   `FirstOccurrenceConstraintProto` ever gets built, it should compute both endpoints
   (§5).

**Two bugs found along the way** (§6): `firstLastOcc` is allocated `L` deep instead of 2
in MiniCP-BP (L² variables where 2L are needed), and the Scorpion factory choice above.
Both must be fixed before any `weakStrongLess` measurement means anything.

**Recommended order of work** in §7. The short version: count weak edges across the
benchmark set *before* writing any encoding, because finding 2 is as informative as
this whole report and takes an afternoon.

---

## 1. The blocker: no weak edges are generated

### 1.1 Where weak edges come from

`disj_action_landmark_graph.cc:112-124` maps Fast-Downward ordering types onto the
DALM-graph's two strengths:

```cpp
if (ord >= OrderingType::NATURAL) {
    dalm_node.add_strong_dependency(from_id, num_orderings, num_weak_orderings);
} else if (ord == OrderingType::REASONABLE
           && !lms[from_id].overlaps_with(dalm_node)) {
    dalm_node.add_weak_dependency(from_id, num_orderings, num_weak_orderings);
}
```

So a weak edge requires (a) a `REASONABLE` ordering and (b) non-overlapping action sets.

### 1.2 The factory in use produces neither

`driver/runners/runner_action_landmark.py:13` runs

```
--search "astar(landmark_sum(lm_rhw(),transform=adapt_costs(one)),bound=0)"
```

`lm_rhw()` is `LandmarkFactoryRpgSasp`. Every `OrderingType` it emits is `NATURAL` or
`GREEDY_NECESSARY` (`landmark_factory_rpg_sasp.cc:209, 240, 518, 536, 664, 724`), and
both satisfy `ord >= OrderingType::NATURAL`. It never emits `REASONABLE`. Reasonable
orderings come only from the `lm_reasonable_orders_hps(...)` wrapper, which is not used.

### 1.3 Confirmed on the graphs

```
floortile-opt11-strips_opt-p01-001   L=17  strong=25  weak=0  |A_L| med=2 max=8
miconic_s5-0                         L=17  strong=20  weak=0  |A_L| med=1 max=9
miconic_s5-1                         L=16  strong=16  weak=0  |A_L| med=1 max=1
woodworking-opt08-strips_p01         L=16  strong=19  weak=0  |A_L| med=4 max=16
```

Four graphs is a small sample, but combined with §1.2 it is not a sample — it is a
proof by the factory's source. **The fix is one line**: wrap the factory as
`lm_reasonable_orders_hps(lm_rhw())`.

### 1.4 What this means for the existing measurement

`landmark-ordering-report.md` §2 reports `22-weakStrongLess` at cov=706 against
`21-strongBeforeAfter` at 701 and base at 741. Given §1.2, that configuration posted
*exactly* the strong constraints and nothing else — the 706 is a measurement of
`strongLess` under a different code path, not of weak orderings. It should not be cited
as evidence about weak orderings in either direction.

---

## 2. What the constraint is, and what the weak edge asserts

### 2.1 `SetFirstLastOccurrence`

`tools/minicpbp/src/main/java/minicpbp/engine/constraints/SetFirstLastOccurrence.java`
posts, over a plan array `x[0..n-1]`, a value set `V`, and two position variables:

```
firstOcc = min { i : x[i] in V }
lastOcc  = max { i : x[i] in V }        (at least one occurrence required)
```

Its filtering, read off `propagate()`:

- bounds channelling `firstOcc <= lastOcc`;
- for positions strictly before `firstOcc.min()` and strictly after `lastOcc.max()`,
  remove all of `V` from `x[p]` (`removeAllValuesAtPos`);
- if `firstOcc` or `lastOcc` is bound to `p`, remove everything *not* in `V` from `x[p]`
  (`forceValuesAtPos`);
- for each candidate position `p`, a three-way test `isDecided(x[p], p)` — using two
  backtrackable witnesses per position, one inside `V` and one outside — that pushes
  `firstOcc`/`lastOcc` bounds when `D(x[p]) ⊆ V`, and removes the hole `p` from both
  when `D(x[p]) ∩ V = ∅`;
- incremental scan anchored on `prevMinPos`/`prevMaxPos` so each position is re-examined
  only as the bounds move.

It declares `setExactWCounting(false)`, so in BP mode its marginals are approximate. In
SP/SBP mode the filtering above is what you get: **bound reasoning on the two endpoints
plus value removal at the extremal positions.** It is not a counting propagator and it
does not reason about positions strictly between `first` and `last`.

### 2.2 The weak edge

`PlanningModelNoOp.java:270-281`:

```java
if (edge.isStrong) { cp.post(less(firstLastOcc[edge.from][0], firstLastOcc[edge.to][0])); }
else               { cp.post(less(firstLastOcc[edge.from][0], firstLastOcc[edge.to][1])); }
```

- strong `A → B`: `first_A < first_B` — *every* occurrence structure of B starts after A
  starts.
- weak `A ⇝ B`: `first_A < last_B` — *some* occurrence of B comes after the first A.

So `last_L` is needed only for landmarks that are the **target** of at least one weak
edge. That asymmetry is what makes the decomposition cheap.

---

## 3. Cost of the decomposition in CP-SAT

### 3.1 The structural point

The current CP-SAT encoding (`planning_model_interface.py:173-203`) already builds, per
landmark and per plan step, a reified membership Boolean via a two-column allowed table
over all `d` action values:

```python
membership = self.NewBoolVar(...)
self.AddAllowedAssignments([action_var, membership], allowed_table)   # d tuples
```

`landmark-ordering-report.md` §1.1 establishes that these `L·n` tables are the whole
cost: 323k tuples ≈ 5.2 MB, and the `Probe`/`MaxClique` work on them is what produces the
274 MB presolve transient. **`last_L` does not need a single new table.** It reuses the
same `membership` literals and adds only the max-side channelling:

| per landmark that is a weak-edge target | count |
|---|---|
| IntVars over `[-1, n-1]` (`max_candidate`) | `n` |
| IntVar over `[0, n-1]` (`last`) | `1` |
| enforced `linear1` | `2n` |
| `lin_max` (`AddMaxEquality`) | `1` |
| new tables | **0** |

That is `n+1` variables and `2n+1` constraints, against the `2n+1` variables and `3n+2`
constraints the `first` side already costs — and none of the added ones are the
expensive kind.

### 3.2 Benchmark

`scratchpad/wkbench.py`, at the parameters `landmark-ordering-report.md` §2 measured over
the 1714 paired tasks: `n=16` plan steps, `L=27` landmarks, `d=758` actions, `|A_L|=8`,
13 strong edges. Following that report's §3.4 caveat, the model **contains the element
constraints and the objective** — without them presolve deletes the plan variables as
singletons and every conclusion inverts.

Three encodings:

- `table-first` — exactly what CCBP posts today, strong edges only. The control.
- `table-firstlast` — same, plus `AddMaxEquality` over `max_candidate` for every weak
  target, plus the `first_A < last_B` edges.
- `prefix-firstlast` — fix A+B of `landmark-ordering-report.md` §3, extended: reified
  domain membership instead of tables, monotone prefix chain `seen[i]` giving
  `first = Σ(1 - seen[i])`, mirror suffix chain `rem[i]` giving `last = Σ rem[i] - 1`.

Solved with `stop_after_presolve:true, num_workers:1`, or-tools 9.15.6769.

### 3.3 Results

| encoding | weak edges | targets | input proto | input v/c | presolved v/c | peak RSS |
|---|---|---|---|---|---|---|
| `table-first` (today) | 0 | 0 | 1,030,409 B | 923 / 1379 | **1,164 / 870** | 340 MB |
| `table-firstlast` | 13 | 10 of 27 | 1,046,614 B | 1093 / 1722 | **1,192 / 903** | 335 MB |
| `table-firstlast` | 100 | 26 of 27 | 1,075,484 B | 1365 / 2337 | **1,192 / 1001** | 341 MB |
| `prefix-firstlast` | 13 | 10 of 27 | 144,195 B | 1093 / 2672 | 11,703 / 23,473 | 166 MB |
| `prefix-firstlast` | 100 | 26 of 27 | 166,160 B | 1365 / 3511 | 11,971 / 24,228 | 168 MB |

**Read three things off this.**

1. **Adding weak orderings to the current encoding is free.** Saturating the graph with
   weak edges — 26 of 27 landmarks needing a `last` — grows the input proto by 4.4% and
   the input variable count by 48%, but the **presolved model gains 28 variables (+2.4%)
   and peak RSS does not move at all**. CP-SAT's presolve absorbs the min/max channelling
   completely. What it cannot absorb is the tables, and those were already paid for.

2. **The table-vs-domain trade-off from `landmark-ordering-report.md` §3.4 is
   unchanged by weak edges.** The prefix form is still 7× smaller in the proto and half
   the RSS, and still leaves a 10× larger presolved model. Adding `last` does not tip it
   either way: the ratios are the same with 13 weak edges as with 100.

3. **Going from 13 to 100 weak edges is almost invisible** in the table form (identical
   presolved variable count, +98 constraints). Whatever the real weak-edge density turns
   out to be in §7.1, it will not be the thing that decides anything.

**One caveat on the absolute RSS.** These runs go through the Python solver wrapper, so
peak RSS includes the interpreter and Python-side model construction; the
`landmark-ordering-report.md` §3.4 figure of 274 MB for the same control was measured
with `sat_runner` on an exported proto. The 340 MB here and the 274 MB there measure the
same model. Only the **deltas within this table** are meaningful, and the proto size and
variable counts, which do agree across the two harnesses to within 0.2%.

---

## 4. Is the decomposition weaker than the propagator?

Two separate questions: does it filter less, and does it matter.

### 4.1 Filtering

Mapping §2.1's five filtering rules onto the decomposition:

| `SetFirstLastOccurrence` does | the decomposition does |
|---|---|
| `removeAllValuesAtPos(p)` outside the bounds | `membership[p] = false` propagates through the reified table, removing `A_L` from `plan[p]` |
| `forceValuesAtPos(p)` at bound endpoints | `membership[p] = true` removes the complement |
| `isDecided → 1` when `D(x[p]) ⊆ V` | the table's reification fixes `membership[p] = true` |
| `isDecided → -1` when `D(x[p]) ∩ V = ∅` | fixes `membership[p] = false` |
| `firstOcc.remove(p)` (hole) | `lin_max`/`lin_min` over the `min_candidate`s; and in the prefix form, unit propagation on `seen[p] ⇔ seen[p-1] ∨ membership[p]` |

I do not find filtering the Java propagator has and the decomposition lacks. The prefix
form is arguably *stronger*: `first_L ≤ i` is literally the literal `seen[i]`, so it is a
full order encoding of the endpoint, and holes fall out of unit propagation rather than
from a `lin_max` bound computation.

What the propagator has is **incrementality** — `prevMinPos`/`prevMaxPos` and the
backtrackable witnesses mean each position is scanned `O(1)` amortised as bounds move,
where the decomposition re-propagates clauses. At `n=16` that is not a consideration.

### 4.2 Whether it matters

Independently of encoding, **`first_A < last_B` is a weak constraint.** `lb(last_B)`
stays at 0 until some position is forced into `A_B`, and `ub(first_A)` stays at `n-1`
until some position is forced out. So the edge is dormant through most of search and
wakes only near the end. This is a structural property of the relation, not of how it is
written, and it is the main reason to expect weak orderings to buy little coverage even
once §1 is fixed.

Contrast the strong edge `first_A < first_B`, which feeds CP-SAT's precedence machinery
(`RootLevelLinear2Bounds`, `ortools/sat/precedences.h:180`; `ConditionalLinear2Bounds`, `:552`) and chains transitively
across the whole landmark DAG.

---

## 5. Consequences for `FirstOccurrenceConstraintProto`

`landmark-ordering-report.md` §5 made the case for a dedicated CP-SAT constraint on four
grounds: the encoding is 2.5-3× of the whole model; the two pure-modelling fixes each win
on one axis and lose on the other; the constraint graph is not a tree so there is real
filtering to gain; and `first_L` is consumed purely through bound comparisons. **Weak
orderings change none of these**, because §3.3 shows they add ~0 to the term that is
binding. The §7 measurement in that report — confirming the presolve transient is what
kills the 97 `oom-sigsegv-building_model` tasks — remains the thing that decides it.

Two amendments:

1. **The signature should compute both endpoints.**

   ```proto
   // first  = min { i : exprs(i) in values }
   // last   = max { i : exprs(i) in values }
   // Infeasible if no exprs(i) is in values.  Either target may be omitted.
   message FirstLastOccurrenceConstraintProto {
     repeated LinearExpressionProto exprs = 1;
     LinearExpressionProto first = 2;        // optional
     LinearExpressionProto last = 3;         // optional
     repeated int64 values = 4;              // Domain-encoded
   }
   ```

   It costs nothing extra in the proto — one repeated field and one branch in expansion —
   and it saves revisiting the design later. Making `last` optional keeps the common
   strong-only case at exactly the cost §5 of the previous report estimated.

2. **In the prefix form, `last` is the mirror suffix chain**, so the expansion emits
   `2n` Booleans, `~6n` clauses and two linear constraints for both endpoints together.
   `first_A < last_B` then relates two order encodings that already exist, and propagates
   by unit propagation rather than through `lin_max` expansions. If the expansion-only
   route of `landmark-ordering-report.md` §5.3 is taken, this is the shape to emit.

The propagator of that report's §5.2 would extend naturally: `ub(last)` = last index `i`
with `A_L ∩ D(plan[i]) ≠ ∅`, `lb(last)` = last index `i` with `D(plan[i]) ⊆ A_L` — the
exact mirror of the `first` rules, same domain-intersection tests, no new machinery.

---

## 6. Bugs found

### 6.1 `firstLastOcc` is allocated `L` deep instead of 2

`PlanningModelNoOp.java:259-268`:

```java
IntVar[][] firstLastOcc = new IntVar[landmarkGraph.nLandmarks()][];
for (int i = 0; i < landmarkGraph.nLandmarks(); i++) {
    firstLastOcc[i] = makeIntVarArray(getSolver(), landmarkGraph.nLandmarks(), upperBoundLength());
}
```

Only indices `[i][0]` and `[i][1]` are ever used. This allocates `L²` variables where
`2L` are needed — 729 instead of 54 at `L=27`, and quadratic in the landmark count, so
it gets worse exactly on the instances that matter. The unused `L²-2L` variables are
unconstrained, which also means they enter the solver's variable ordering.

Fix: `makeIntVarArray(getSolver(), 2, upperBoundLength())`.

**Any MiniCP-BP comparison of `weakStrongLess` against `strongLess` run before this fix
is measuring the allocation bug, not the orderings.**

### 6.2 The factory choice

§1.2. `runner_action_landmark.py:13` must become `lm_reasonable_orders_hps(lm_rhw())` for
weak edges to exist at all. Note this changes the *strong* edges too — the wrapper only
adds orderings, but `add_strong_dependency` upgrades an existing weak edge to strong
(`disj_action_landmark_graph.cc:30-33`), so the graph is not a superset in a trivial
sense. The strong-only configurations must be re-run on the new graphs before they can
be compared to the weak ones.

### 6.3 Pre-existing, from the previous report

`landmark-ordering-report.md` §1.3: the ~20-line `first_L` block is triplicated across
`add_strongless_...`, `add_strong_before_after_...` and
`add_strong_before_after_different_...`. Adding `last` support would make it four copies
of a longer block. Extract first.

---

## 7. Recommendation

In order, and each step can kill the next.

1. **Count the weak edges** (§7.1 below). One afternoon. If the density is negligible on
   the domains you care about, stop — everything else here is moot.
2. **Fix §6.1 and §6.2.** Neither is optional if any weak-ordering measurement is to
   mean anything.
3. **Extract the triplicated `first_L` core** (§6.3), as `landmark-ordering-report.md`
   §4.1 already recommended. Zero behaviour change.
4. **Add `last` to the CP-SAT encoding** — appendix A, ~12 lines once step 3 is done.
   §3.3 says it is free, so there is no reason to gate this on further measurement once
   steps 1-3 are done.
5. **Do not build a CP-SAT constraint for this.** The decision stays where
   `landmark-ordering-report.md` §7 left it. If it is ever taken, use the two-endpoint
   signature of §5.1.

**What I am explicitly not claiming:** that weak orderings improve coverage. §4.2 gives a
structural reason to expect them to prune late and little. The `transitions` experience
(`not-before-after-report.md` §0.1) and the `21-strongBeforeAfter` result (−40 coverage
against base) both say that a sound-looking redundant constraint can easily cost more in
search overhead than it recovers in pruning. The merit of step 4 is that it is cheap
enough to be worth trying, not that it is likely to win.

### 7.1 The measurement that decides this

Regenerate the landmark graphs with reasonable orderings and count:

```sh
cd ~/Desktop/CCBP
# after applying the fix of 6.2
for d in <a domain sample>; do
  uv run ./ccbp.py "$d" --action-landmark-graph-constraints none
done
# then, over the produced out/*/action_landmark_graph.txt, run cnt.py (appendix B):
uv run python cnt.py
```

What to look for:

- **weak-edge density** — weak / (weak + strong), and the number of *distinct targets*,
  since that is what sets the decomposition cost (§3.1). §3.3 says anything up to
  saturation is free, so this number decides whether the constraints are worth posting at
  all, not how to post them.
- **how the strong edges changed.** §6.2: the wrapper can upgrade weak to strong. If the
  strong-edge count moves materially, `00-base` and `23-strongBeforeAfterDifferent` need
  re-running on the new graphs before any comparison.
- **whether `|A_L|` and `L` moved.** These set the encoding cost, and
  `landmark-ordering-report.md`'s whole cost model is parameterised on them.

---

## Appendix A — the `last` patch

Mirroring the existing `min_candidate` block, inside the per-landmark loop, guarded so it
is only built for weak-edge targets:

```python
if landmark_id in weak_edge_targets:
    max_candidates = []
    for step_idx, membership in enumerate(indicator_vars):     # reuse, do NOT rebuild
        max_candidate = self.NewIntVar(-1, horizon - 1, f"lm_{landmark_id}_last_candidate_{step_idx}")
        self.Add(max_candidate == step_idx).OnlyEnforceIf(membership)
        self.Add(max_candidate == -1).OnlyEnforceIf(membership.Not())
        max_candidates.append(max_candidate)
    last = self.NewIntVar(0, horizon - 1, f"lm_{landmark_id}_last")
    self.AddMaxEquality(last, max_candidates)
    landmark_last_vars[landmark_id] = last
```

`sum(indicator_vars) >= 1` is already posted on the `first` side, so `last >= 0` is
implied and the `-1` sentinel can never be the maximum. The edges:

```python
for landmark_id in range(landmark_graph.n_landmarks()):
    edges = landmark_graph.precedence_edge[landmark_id]
    if edges is None: continue
    for edge in edges:
        if not edge.is_strong:
            self.Add(landmark_first_vars[edge.from_id] < landmark_last_vars[edge.to_id])
```

Note `get_min_strong_successors` must **not** be used for weak edges — the transitive
reduction is only valid for strong edges, since `first_A < last_B` is not transitive.
Iterate the raw `precedence_edge` sets.

---

## Appendix B — scripts

Committed next to this report. Both run from `~/Desktop/CCBP` (they need the CCBP venv
and, for `cnt.py`, the `driver.landmark` package on the path).

- `weak-ordering-cnt.py` — weak/strong edge counts, weak-target counts and `|A_L|`
  statistics over `out/*/action_landmark_graph.txt`. Produces the §1.3 table.

  ```sh
  cd ~/Desktop/CCBP && uv run python ~/Desktop/or-tools/weak-ordering-cnt.py
  ```

- `weak-ordering-bench.py` — the §3 encoding micro-benchmark.

  ```sh
  cd ~/Desktop/CCBP
  uv run python ~/Desktop/or-tools/weak-ordering-bench.py                    # proto sizes, all modes
  WEAK=100 uv run python ~/Desktop/or-tools/weak-ordering-bench.py table-firstlast   # + presolve log
  ```

  Modes: `table-first` (the control — what CCBP posts today), `table-firstlast`,
  `prefix-firstlast`. `WEAK` sets the weak-edge count (default 13); `OUT_DIR` sets where
  the exported `.pb` goes (default `.`). Passing a mode also runs a presolve-only solve
  with `log_search_progress`, which is where the presolved v/c counts in §3.3 come from;
  wrap it in `/usr/bin/time -v` for the RSS column.

  The `N, L, D, ASIZE, STRONG` constants at the top are the §3.2 parameters. Change them
  together with the §3.3 table, and keep the element constraints and the objective —
  `landmark-ordering-report.md` §3.4 documents what happens to the conclusions without
  them.
