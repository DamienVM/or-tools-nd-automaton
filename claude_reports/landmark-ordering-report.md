# Report: landmark ordering constraints in CCBP, and whether CP-SAT should have a dedicated constraint for them

Written 2026-09-02. Companion to `not-before-after-report.md` (the
`transitions` constraint) and `nd-report.md` (non-deterministic automata).
CCBP at `~/Desktop/CCBP`, or-tools at `~/Desktop/or-tools` branch
`forbidden-transitions`.

**Why this report exists.** The `transitions` constraint shipped and moved almost
nothing (§0.1). The post-mortem said: before building anything, measure what fraction
of the model the encoding is, and check whether the term you shrink is the term that is
binding. This report does that first, for the landmark ordering constraints, and only
then asks the design question.

---

## 0. Summary

**The encoding.** All three landmark-ordering variants
(`strongLess`, `strongBeforeAfter`, `strongBeforeAfterDifferent`) share one core, copied
verbatim three times: for each landmark `L`, compute
`first_L = min{ i : plan[i] ∈ A_L }`, then post `first_L < first_M` per ordering edge.
The `first_L` computation costs, per landmark, `2n+1` variables and `3n+2` constraints —
including `n` two-column tables of `d` tuples each (§1).

**It is expensive, and unlike the consecutive-action constraints it is also useful.**
Measured over 1714 paired tasks of `2026-08-31-landmark-graph-eval`:

| | vs `00-base` |
|---|---|
| `cp_n_variables_init` | 749 → 1834 (**×2.45**) |
| `cp_n_constraints_init` | 788 → 2437 (**×3.09**) |
| coverage, `23-strongBeforeAfterDifferent` | 741 → **768 (+27)** |
| coverage, `21-strongBeforeAfter` | 741 → 701 (−40) |
| `oom-sigsegv-building_model` | 71 → 97 |

So the best variant pays for itself, and the encoding is ~2.5-3× of the whole model —
not the ~2% the consecutive-action constraints turned out to be (§0.1). **This is a
worthwhile target.**

**The headline measurement.** At the measured parameters (`n=16` plan steps, `L=27`
landmarks, `d=758` actions, `|A_L|≈8`), replacing the membership tables with enforced
domain constraints trades one resource against another:

| encoding | input proto | presolved model | peak RSS |
|---|---|---|---|
| current (tables) | 1,032,326 B | **1,052 v / 813 c** | **274 MB** |
| domain constraints | 144,102 B | 9,486 v / 18,969 c | **91 MB** |

The table form presolves down to a *tiny* model but needs 274 MB to get there. The domain
form never builds that transient structure and peaks at 91 MB, but leaves a 9× larger
model for search. **Neither dominates.** Both encodings are semantically exact
(verified, §3.3).

**Conclusion.** There is a real case for a dedicated constraint here, and it is much
stronger than the one for `transitions` was:

1. It is a large fraction of a model that is genuinely helped by the constraints.
2. The two obvious pure-modelling fixes each win on one axis and lose on the other, so
   there is headroom no modelling change can reach (§4).
3. Unlike `transitions`, the constraint graph is **not** a tree, so there is genuine
   filtering to gain (§5.1).
4. `first_L` is consumed **only through bound comparisons** (`first_L < first_M`), which
   is precisely the case where a CP-SAT propagator does not need value encoding (§5.2).

**Recommended next step is still a measurement, not code** (§7): instrument one
mid-size instance to confirm the 274 MB transient is what `oom-sigsegv-building_model`
is actually hitting. That is a half-day and it decides between §4 and §6.

### 0.1 What went wrong last time, in one paragraph

The `transitions` constraint reduced the input proto by 99× and peak RSS by 4.6%, and
changed coverage by +1 of 1836. Two reasons: the consecutive-action constraints were
~15 of ~850 constraints (~2%), and the term reduced (input proto duplication) was not
the binding one (expansion output was). The 88 tasks rescued from `oom-building_model`
all died in `oom-solving` instead. The synthetic benchmark that justified the work used
`n=100` in a model containing nothing else, which overstated the effect by an order of
magnitude. Every number in this report is therefore either measured on the real
benchmark set, or measured on a micro-benchmark whose parameters were taken from it —
and §3.2 documents one place where making the micro-benchmark representative reversed
the conclusion.

---

## 1. What is posted today

Three methods in `driver/cp_sat/model/planning_model_interface.py`:

| method | line | selected by |
|---|---|---|
| `add_strongless_landmark_ordering_constraints` | 173 | `--action-landmark-graph-constraints strongLess` / `weakStrongLess` |
| `add_strong_before_after_landmark_ordering_constraints` | 213 | `strongBeforeAfter` |
| `add_strong_before_after_different_landmark_ordering_constraints` | 284 | `strongBeforeAfterDifferent` |

dispatched at `driver/cp_sat/ai_planning.py:71-76`.

### 1.1 The shared core: `first_L`

All three begin with the same ~20 lines, copied verbatim:

```python
first = self.NewIntVar(0, horizon - 1, f"lm_{landmark_id}_first")
allowed_table = [(v, 1) if v in actions else (v, 0)
                 for v in range(self._domain_min, self._domain_max + 1)]   # d tuples

for step_idx, action_var in enumerate(self._plan):
    membership = self.NewBoolVar(...)
    self.AddAllowedAssignments([action_var, membership], allowed_table)     # d tuples
    indicator_vars.append(membership)

    min_candidate = self.NewIntVar(0, horizon, ...)
    self.Add(min_candidate == step_idx).OnlyEnforceIf(membership)
    self.Add(min_candidate == horizon).OnlyEnforceIf(membership.Not())
    min_candidates.append(min_candidate)

self.Add(sum(indicator_vars) >= 1)
self.AddMinEquality(first, min_candidates)
```

Per landmark, with `n` plan steps and `d` actions:

| | count |
|---|---|
| Booleans (`membership`) | `n` |
| IntVars over `[0, n]` (`min_candidate`) | `n` |
| IntVars over `[0, n-1]` (`first`) | `1` |
| 2-column allowed tables | `n`, each of `d` tuples → `n·d` tuples |
| enforced `linear1` | `2n` |
| `lin_max` (`AddMinEquality`) | `1`, over `n` expressions |
| `linear` (`sum ≥ 1`) | `1` |

so `L(2n+1)` variables and `≈ L(3n+2)` constraints in total. Confirmed against the real
model: `miconic/s5-0` goes from 222 v / 216 c to 817 v / 1132 c under `strongLess`,
i.e. +595 v and +916 c; with `n=17`, `L=17` that predicts +595 and +901. The
constraint-to-variable ratio `(3n+2)/(2n+1) → 1.5` matches the benchmark-wide medians
(+1085 v, +1649 c, ratio 1.52).

The `CpModelProto` breakdown at the measured parameters, straight from the log:

```
#Variables: 923 (896 primary)
  - 432 Booleans in [0,1]      <- membership,      L*n = 27*16
  - 432 in [0,16]              <- min_candidate,   L*n
  -  27 in [0,15]              <- first_L
  -  16 in [0,757]             <- plan
#kTable:   432 (each with d = 758 tuples)
#kLinear1: 864 (#enforced: 864)
#kLinMax:   27 (#expressions: 432)
#kLinearN:  27 (#terms: 432)
#kLinear2:  13               <- the ordering edges
```

Note the shape: **the ordering constraints themselves are 13 of 1379 constraints.** Over
99% of the cost is computing `first_L`, not ordering it.

### 1.2 What the variants add on top

`strongLess` adds only `first_L < first_M` for `get_min_strong_successors`.

`strongBeforeAfter` and `strongBeforeAfterDifferent` additionally build, per landmark,
min/max over all strong predecessors and successors (`AddMinEquality` /
`AddMaxEquality`), three extra IntVars, and offset bounds using
`get_greedy_max_num_non_overlapping_{pred,succ}essors`. That is `O(L)` extra, negligible
beside `O(L·n)`.

`strongBeforeAfterDifferent` further adds one `AddAllDifferent` over the greedy
disjunctive landmarks. Also cheap, and it is the variant that wins (+27 coverage), so
whatever else changes, keep it.

### 1.3 Incidental finding: the core is triplicated

The ~20-line `first_L` block appears three times verbatim (lines 180-203, 217-244,
288-315), as does the trailing `min_strong_successors` loop. Any change to the encoding
has to be made three times, and the two `strongBeforeAfter*` variants differ only by the
`AllDifferent` at the end. Worth extracting into one `_add_landmark_first_vars()`
regardless of the outcome of this report — it is the difference between a one-line and a
three-line experiment next time.

---

## 2. Is it worth optimising? Yes.

From `2026-08-31-landmark-graph-eval`, 1836 tasks per configuration:

```
00-base                        cov=741   oot=604  oom-solving=370  oom-sigsegv-building=71
21-strongBeforeAfter           cov=701   oot=551  oom-solving=442  oom-sigsegv-building=97
22-weakStrongLess              cov=706   oot=547  oom-solving=442  oom-sigsegv-building=95
23-strongBeforeAfterDifferent  cov=768   oot=465  oom-solving=461  oom-sigsegv-building=97
```

Three things to read off this:

1. **`strongBeforeAfterDifferent` is worth +27 coverage.** That is the whole reason this
   report is not "leave it alone". Contrast the consecutive-action constraints, where
   every configuration was *below* base and the posting mechanism was therefore
   irrelevant.
2. **The encoding cost is showing up as failures.** `oom-sigsegv-building_model` rises
   71 → 97 across all three variants; `oom-solving` rises 370 → 442-461. So both the
   build side and the solve side are being pushed over the limit by the extra ~1085
   variables and ~1649 constraints.
3. **`oot-solving` *falls* 604 → 465** for the winning variant. The constraints are
   doing real pruning work; they are simply expensive to state.

Parameters, medians over the 1714 paired tasks (used for every micro-benchmark below):

```
plan length  n = 16          landmarks  L ≈ 27          actions  d = 758
membership tables: L*n = 432 constraints, 758 tuples each = 323k tuples ≈ 5.2 MB int64
min_candidate IntVars: L*n = 432, each over [0, 16]
```

---

## 3. What the two obvious modelling fixes actually do

Both are pure Python, no C++, no new constraint. Both are semantically exact (§3.3).

### 3.1 Fix A — replace the membership tables with domain constraints

`AddAllowedAssignments([action_var, membership], allowed_table)` is a reified set
membership, `membership ⇔ plan[step] ∈ A_L`, written as a two-column table enumerating
**all `d` action values**. The direct form:

```python
dom = Domain.from_values(sorted(actions))
self.add_linear_expression_in_domain(action_var, dom).only_enforce_if(membership)
self.add_linear_expression_in_domain(action_var, dom.complement()).only_enforce_if(~membership)
```

Proto cost drops from `d` tuples to `O(|A_L|)` domain intervals per (landmark, step).

### 3.2 Fix B — replace `min_candidate` + `AddMinEquality` with prefix Booleans

`first_L` is the index of the first occurrence, so with monotone prefix Booleans
`seen[i] = ⋁_{j≤i} membership[j]` it is just a sum:

```python
seen = [NewBoolVar() for _ in range(n)]
# seen[0] <=> membership[0]; seen[i] <=> seen[i-1] or membership[i]
first_L == sum(1 - seen[i] for i in range(n))
```

This replaces `n` IntVars over `[0, n]` and `2n` enforced linear constraints and one
`lin_max` with `n` Booleans, `~3n` clauses and one linear constraint. Same variable
*count*, but `n` of them move from integer to Boolean per landmark.

### 3.3 Both are exactly equivalent

`scratchpad/lmcheck.py` enumerates all `(plan, first[])` assignments for 40 random
configurations (`n ≤ 4`, `d ≤ 4`, `L ≤ 3`, with and without ordering edges) under all
three encodings and against a pure-Python brute force:

```
40/40 random configurations agree with brute force
```

This checks the `first` values, not just feasibility — which is the part that could
silently drift.

### 3.4 And they do not help the way I expected

`scratchpad/lmbench.py` at `n=16, L=27, d=758, |A_L|=8`, exported and presolved with
`sat_runner --params="stop_after_presolve:true,num_workers:1"`:

| encoding | input proto | input v/c | presolved v/c | presolved terms | peak RSS |
|---|---|---|---|---|---|
| current (tables) | 1,032,326 B | 923 / 1379 | **1,052 / 813** | 19,022 | **274 MB** |
| fix A | 144,102 B | 923 / 1811 | 9,486 / 18,969 | 47,674 | **91 MB** |
| fix A + B | 142,388 B | 923 / 2216 | 9,797 / 19,618 | 49,633 | 92 MB |

**The table encoding is presolve-friendly and memory-hostile; the domain encoding is the
reverse.** The table form ends at 1,052 variables and 813 constraints — remarkably small,
CP-SAT's presolve is very good at this shape (the log shows `Probe` generating 74,045 new
binary clauses and `MaxClique` merging 316,188 constraints with 632,376 literals into
293,765). That work is exactly what costs 274 MB. Fix A never creates that structure, so
it peaks at 91 MB, but its enforced multi-interval domain constraints get expanded
per value and leave a 9× larger model for search.

**A caveat on this table, and the reason to trust it.** My first version of this
benchmark had no `AddElement` and no objective, so presolve deleted the cost variables as
singletons and the plan variables were never really encoded. It reported fix A shrinking
the presolved model 1047 → 462 variables — the opposite of the truth. Adding the element
constraints *and* the objective (both of which the real model has, and the objective is
what stops presolve from deleting them) flipped it to 1,052 → 9,486. This is the same
class of error as §0.1 and it was caught only by asking "does this benchmark contain what
the real model contains".

---

## 4. So what should change in CCBP, with no new constraint

Given §3.4, there is no free win, but there are three defensible moves:

1. **Extract the triplicated core** (§1.3). Zero behaviour change, and it makes
   everything below a one-line experiment.
2. **Make the membership encoding a flag**, not a decision. The two forms trade 274 MB
   of transient presolve memory against a 9× larger search model, and which one wins is
   an empirical question per instance family — exactly the kind of thing your experiment
   harness is built to answer. Given `oom-sigsegv-building_model` is 97 tasks and
   `oom-solving` is 461, my guess is the table form is right for most instances and the
   domain form rescues the big ones, but that is a guess.
3. **Do not bother with fix B alone.** It moves `L·n` variables from integer to Boolean
   and changes nothing measurable in §3.4 (91 → 92 MB, 9,486 → 9,797 variables). Its only
   real argument is that it removes the `lin_max` over `n` expressions per landmark, and
   the numbers say that is not where the cost is.

---

## 5. The case for a dedicated CP-SAT constraint

The constraint wanted is *index of first occurrence of a value from a set*:

```proto
// target = min { i : exprs(i) in values }.  Infeasible if no exprs(i) is in values.
message FirstOccurrenceConstraintProto {
  repeated LinearExpressionProto exprs = 1;
  LinearExpressionProto target = 2;
  repeated int64 values = 3;          // Domain-encoded
}
```

One constraint per landmark, replacing `3n+2` constraints and `2n` auxiliary variables.
This is a better candidate than `transitions` was, for two specific reasons.

### 5.1 The tree argument does not apply, so there is filtering to gain

`transitions` was a chain of binary relations on consecutive pairs — a path, hence a
tree, hence arc consistency on the decomposition already equalled GAC on the global
constraint, hence a propagator could not prune anything new
(`not-before-after-report.md` §2). That was the single strongest argument against it.

Here the structure is different: `first_L` is linked to **all `n`** plan variables at
once, and the `first_L` variables are linked to each other by the ordering edges. The
constraint graph contains cycles. GAC on `first_L = min{i : plan[i] ∈ A_L}` is strictly
stronger than arc consistency on the `membership`/`min_candidate` decomposition — for
instance, knowing `first_L ≥ 5` from an ordering edge should remove `A_L` from
`plan[0..4]`, which the decomposition only derives through the `lin_max` and the
per-step enforced equalities.

### 5.2 `first_L` is consumed purely through bounds

Every consumer of `first_L` is a bound comparison: `first_L < first_M` for the edges,
plus min/max/offset comparisons in the `strongBeforeAfter*` variants. Nothing ever asks
"is `first_L` equal to 7". This matters because CP-SAT cannot remove an interior domain
value without a Boolean literal for it (`not-before-after-report.md` §3.2) — but it
propagates *bounds* natively and cheaply. A propagator here would:

- push `lb(target)` = first index `i` with `A_L ∩ D(plan[i]) ≠ ∅` — a domain
  intersection test, no literals;
- push `ub(target)` = first index `i` with `D(plan[i]) ⊆ A_L` — likewise;
- feed `RootLevelLinear2Bounds` / `ConditionalLinear2Bounds`
  ([precedences.h:548](ortools/sat/precedences.h#L548)), which is where the `first_L <
  first_M` chain already lives, so the ordering edges would propagate through the
  existing precedence machinery rather than through `lin_max` expansions.

Only the reverse direction (`target` fixed ⇒ remove `A_L` from earlier steps) needs
literals, and only when `A_L` is not an interval.

That is the combination `not-before-after-report.md` §3.3 identified as the one place a
CP-SAT propagator genuinely earns its keep, and unlike there, it is the common case here
rather than a corner.

### 5.3 What it would cost, honestly

- Proto, validator, presolve, expansion, the seven compiler-forced sites in
  `cp_model_utils.cc`, the `LOG(FATAL)` feasibility checker, Python wrapper — the same
  checklist as `not-before-after-report.md` §4.4, ~400 lines plus tests.
- Plus, if the propagator of §5.2 is wanted, a `cp_model_loader.cc` case and a
  `first_occurrence.cc/.h`. That breaks the "always expanded" invariant and therefore
  needs the symmetry-detection and LS-evaluator cases that expansion-only constraints get
  away with skipping (`not-before-after-report.md` §6.2).
- The expansion alone (no propagator) would emit the §3.2 prefix form directly — `2n`
  Booleans, `~3n` clauses, one linear per landmark — without the table detour, which is
  the shape that should give both a small proto *and* a small presolved model. **That is
  the specific hypothesis worth testing, and §3.4 says neither existing encoding achieves
  it.**

---

## 6. Recommendation

1. **Extract the triplicated core** (§1.3, §4.1). Do this regardless.
2. **Confirm the 274 MB transient is what is actually killing the 97
   `oom-sigsegv-building_model` tasks** (§7). If it is not, stop — the encoding is not
   your bottleneck and §5 is premature.
3. If it is: add the membership encoding as a flag (§4.2) and run it across the domain
   set. This costs no C++ and gives a real answer about which axis matters.
4. Only then build `FirstOccurrenceConstraintProto`, expansion-only, emitting the prefix
   form (§5.3). Expected win: proto ≈ fix A, presolved model ≈ current, RSS ≈ fix A.
   Test the hypothesis on the micro-benchmark *before* touching the proto — it is 30
   lines of Python to hand-write the prefix encoding and measure it.
5. The propagator (§5.2) is a separate, later project. It is the first one in this
   series with a sound theoretical case, but it should be justified by a measurement of
   how often ordering bounds actually prune, not by the argument alone.

**What I am explicitly not claiming:** that any of this improves coverage. §2 shows the
constraints already pay for themselves at their current cost; a cheaper encoding lets
more instances build and search, which *should* convert some of the 97 build-OOMs and
461 solve-OOMs. The `transitions` experience is that such conversions can be exactly
zero-sum (`not-before-after-report.md` §0.1, and the 88 tasks that moved from
`oom-building_model` to `oom-solving`). Assume nothing until it is on the plot.

---

## 7. The measurement that decides this

One instance from the `oom-sigsegv-building_model` set under
`23-strongBeforeAfterDifferent`, run locally with a raised memory limit:

```sh
cd ~/Desktop/CCBP
/usr/bin/time -v uv run ./ccbp.py <that instance> \
    --action-landmark-graph-constraints strongBeforeAfterDifferent 2>&1 \
  | grep -E "Maximum resident|Model has|presolve"
```

and the same with `--action-landmark-graph-constraints none`. What to look for:

- **Peak RSS attributable to the landmark encoding.** If the delta is on the order of
  the 274 → 91 MB seen in §3.4, per model, and models are rebuilt per plan length
  (`cp_n_models_built`), the encoding is the build-side bottleneck and §6.3-6.4 follow.
- **Whether the peak occurs during presolve or during search.** `MaxClique` and `Probe`
  are presolve; if the peak is in search instead, the encoding is not the problem and
  the 9× larger presolved model of fix A would make things worse, not better.

Scripts used for this report, in the session scratchpad: `lm.py` (benchmark-set
analysis), `lmbench.py` (encoding micro-benchmark), `lmcheck.py` (equivalence fuzz),
`lmexport.py` (proto export for `sat_runner`), `lmrules.py` (presolve rule breakdown).
Copy them into the repo if any of this gets picked up.
