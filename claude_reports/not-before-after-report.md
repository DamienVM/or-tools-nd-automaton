# Report: a `transitions` constraint for CP-SAT

Base: `98c165af62` (v9.15 + 14 commits). Implemented on branch
`forbidden-transitions`. Companion to `nd-report.md` (non-deterministic automata,
plus the build and cluster-deployment recipes) and `landmark-ordering-report.md`
(the follow-up target).

**History.** Started 2026-09-01 from a `NotBefore` / `NotAfter(v, S)` framing.
Analysis, measurement, and the real target model
(`~/Desktop/CCBP/driver/cp_sat/model/`) reshaped the design twice; §11 is the
decision log. Implemented, shipped to CCBP, and evaluated on the full benchmark set
on 2026-09-02. **The evaluation is the most important section: the constraint works
exactly as designed and changed almost nothing.** §10 explains why, and what that
implies for the next attempt.

---

## 0. Summary

**The use case.** In the CCBP planning model, `self._plan[i]` is an `IntVar` over
global actions, one per plan step. Certain ordered action pairs must not (or must)
occur at consecutive steps. This was implemented as one `AddForbiddenAssignments` /
`AddAllowedAssignments` per step, each holding the full pair list. The concern was
model memory, because the pair list is large.

**What the analysis found.**

1. **The decomposition wastes no variables.** `ExpandNegativeTable` emits only
   clauses over *shared* value-encoding literals
   ([cp_model_expand.cc:1673](ortools/sat/cp_model_expand.cc#L1673)). In this model
   especially: `AddElement` and `AddAutomaton` on `self._plan` already force a
   Boolean for every `(step, action)` pair, so these constraints add **zero**
   variables (§3.1).
2. **The waste is repetition** — the same pair list written once per step (§3.4).
3. **A propagator is the wrong first move.** CP-SAT cannot remove a value from the
   interior of a domain without a Boolean literal for it (§3.2), so a propagator
   cannot avoid the encoding either.
4. **There is no filtering to gain.** A chain of binary relations on consecutive
   pairs is a path — a tree — so arc consistency on the decomposition already equals
   GAC on the global constraint (§2).

**The design.** One constraint holding the expression array once and the pair list
once, in either direction:

```proto
message ValuePairsProto {
  repeated int64 pairs = 1;          // flattened (first, second), even size
}

message TransitionsConstraintProto {
  repeated LinearExpressionProto exprs = 1;
  oneof pairs {
    ValuePairsProto forbidden = 2;
    ValuePairsProto allowed   = 3;
  }
}
```

Expanded in presolve into exactly the clauses the per-step tables produce. Same
semantics, same solver behaviour, `O(|F| + n)` proto instead of `O(n·|F|)`.

**What it delivered.** Semantically exact (§7). Input model 99× smaller on the pair
data, presolve 9% faster, peak RSS 4.6% lower (§9). On the real benchmark set:
**coverage 729 → 732 out of 1836** on the forbidden branch and **657 → 658** on the
allowed branch (§10). For the commuting configuration it halved model-building OOMs
(203 → 115) — and all 88 rescued tasks then died in `oom-solving` instead.

**The lesson, stated up front** (§10.3): the constraint amortises the *input proto*,
which is ~5% of peak memory and, in these models, ~2% of the constraint count. The
binding term is the expansion output, which this constraint reproduces exactly. Any
future work of this kind must first measure what fraction of the model the target is,
and whether the term being shrunk is the term that is binding.

---

## 1. What the constraint is

### 1.1 Semantics and proto

For every `i` in `[0, exprs_size() - 2]`, writing `p = (exprs(i), exprs(i+1))`:

- with `forbidden`: `p` must **not** be one of the listed pairs;
- with `allowed`: `p` must **be** one of the listed pairs.

Fewer than two expressions ⇒ trivially true. In the `oneof` at
[cp_model.proto:331](ortools/sat/cp_model.proto#L331):

```proto
    TransitionsConstraintProto transitions = 31;
```

Field 31 was the next free id (the `// Next id:` comment at
[cp_model.proto:309](ortools/sat/cp_model.proto#L309) is bumped to 32).

Design notes:

- **A `oneof` of submessages, not two plain repeated fields.** Repeated fields have
  no presence in proto3, so "both empty" would be indistinguishable — and reading
  that as trivially-true is **unsound** for an empty allowed-list, which must be
  infeasible. The `oneof` gives real presence. The
  `TransitionsExpandTest.NoAllowedPairsIsInfeasible` test pins this.
- **A flat pair list, not two domains.** An earlier draft used
  `from_domain`/`to_domain`, i.e. `exprs(i) ∈ A ⇒ exprs(i+1) ∈ B`. That expresses a
  single *rectangle* `A × ¬B`. The CCBP pair sets are scattered singletons, so a
  rectangle form would need one constraint per pair, each carrying its own copy of
  `exprs` — worse than what it replaces. See §1.4 for when that flips.
- **`LinearExpressionProto`, not `int32` vars**, matching `table` and `automaton`
  ([cp_model.proto:301](ortools/sat/cp_model.proto#L301)).
- **Enforcement literals** come free from `ConstraintProto.enforcement_literal`. See
  §2.1 and §4.3.
- **Pairs, not arbitrary arity.** The arity-`k` generalisation is `table`, which
  already exists. This constraint exists to amortise one *fixed* relation over a
  *sliding window of size 2*.

### 1.2 The four user-facing forms all lower to a pair set

The original request was four constraints. They are the same thing once the client
API flattens them:

| user-facing form | meaning | lowering |
|---|---|---|
| `NotAfter(v, S)` | `x[i]=v ⇒ x[i+1] ∉ S` | `forbidden {(v, s) : s ∈ S}` |
| `NotBefore(v, S)` | `x[i]=v ⇒ x[i-1] ∉ S` | `forbidden {(s, v) : s ∈ S}` |
| `AllowedAfter(v, S)` | `x[i]=v ⇒ x[i+1] ∈ S` | `forbidden {(v, w) : w ∈ D \ S}` |
| `AllowedBefore(v, S)` | `x[i]=v ⇒ x[i-1] ∈ S` | `forbidden {(w, v) : w ∈ D \ S}` |

One implementation, one expansion, four names in the wrappers. `add_not_after` and
`add_not_before` ship; the two `Allowed*` rows need the domain at build time to
complement `S`, and when `S` is large the `allowed` field of §1.1 is the better
statement anyway.

### 1.3 Edge cases the validator and expansion agree on

- `exprs_size() <= 1` → trivially true, remove.
- `forbidden` empty → trivially true, remove.
- `allowed` empty → **infeasible** when there are ≥ 2 expressions (this is why §1.1
  uses a `oneof`).
- neither set → invalid model, rejected.
- odd pair-list size → invalid model, rejected.
- A pair `(a, b)` with `a ∉ D[i]` or `b ∉ D[i+1]` → inert at position `i`; silently
  dropped there, kept elsewhere.
- Values unreachable through an affine expression (e.g. `1` under `2x`) → dropped.
- Duplicate pairs → harmless, ignored.

### 1.4 Extensions, if the data changes

- **Block / rectangle form.** If the pairs become block-structured — "no two moves in
  a row", "these 50 actions are mutually exclusive" — a rectangle list compresses
  enormously *and* survives to expansion, emitting one clause per step per block
  instead of `|A|·|B|`. A 20-action mutex group is 400 pairs flat and one entry here.
  **This is the extension §10.4 now recommends looking at**, because the commuting
  matrices are exactly block-structured; the "scattered singletons" finding came from
  the opposite-action data.
- **Window / offset** (`(x[i], x[i+k]) ∉ F` for `k ∈ [1, w]`). The one extension that
  changes the §2 conclusion: with `w ≥ 2` the constraint graph has triangles, is no
  longer a tree, and GAC becomes strictly stronger than AC on the pairs. This is where
  a propagator would earn its keep.
- **Cyclic** (`x[n-1] → x[0]`): a flag; trivial in expansion.

---

## 2. There is no filtering to gain

Let `R(a,b)` be the pair relation. The constraint is
`∧_{i=0}^{n-2} R(x[i], x[i+1])`, whose constraint graph is the path
`x[0] — x[1] — … — x[n-1]`: a tree.

On tree-structured binary networks, enforcing arc consistency leaves every remaining
domain value participating in at least one solution (Freuder 1982). Therefore

> **GAC on the global constraint = AC on the pairwise decomposition.**

In CP-SAT the clause encoding *is* AC on each pair, given that the values are encoded
and the `exactly_one` from full encoding is present — which the CCBP model already
guarantees (§3.1). So the per-step tables were already achieving the strongest
possible filtering, and a native version cannot improve on it. This is the same
reasoning behind `table`, `automaton` and `element` having no propagators at all (no
case for them in [cp_model_loader.cc:1695](ortools/sat/cp_model_loader.cc#L1695)).

### 2.1 The one exception: enforcement literals

With an enforcement literal `e`, adding `e` connects one variable to every pair, so
the graph stops being a tree. If the chain is infeasible only *globally*, root-level
AC on the enforced pairs will not infer `¬e`, whereas a propagator reasoning under
the hypothesis `e` would. CDCL recovers this on the first branch on `e`, so the gap
is narrow — but it is the only honest "yes" to "can a propagator ever help at `w=1`".

---

## 3. Where the memory actually is

### 3.1 The decomposition adds no variables — and in this model, provably none

`ExpandNegativeTable`
([cp_model_expand.cc:1673-1721](ortools/sat/cp_model_expand.cc#L1673)) emits one
`bool_or` per tuple over `NegatedRef(GetOrCreateVarValueEncoding(var, value))`. No
tuple-selection literals, no new integer variables; the encoding literals are cached
per `(var, value)` and shared between adjacent positions.

In CCBP the encoding literals exist regardless. All three of these force
`GetOrCreateVarValueEncoding` on every `(step, action)` pair:

- `AddElement(self._plan[idx], automata.global_action_cost, self.action_cost[idx])`;
- `AddAutomaton(automaton_plan, …)` where `automaton_plan is self._plan`;
- `AddAllowedAssignments([automaton_plan[idx], self._plan[idx]], allowed_pairs)`.

So `n · |actions|` Booleans are already paid for and this constraint contributes
none, whether decomposed, native, or propagated.

Corollary, acted on: `add_consecutive_actions_constraints_booleans` was pure
overhead — its explicit `NewBoolVar`s duplicate literals that already exist, and its
forbid branch emitted **each pair twice** (the two `OnlyEnforceIf` lines produce the
same clause).

### 3.2 CP-SAT cannot remove an interior value without a literal

`IntegerTrail` stores bounds only
([integer.h:595-598](ortools/sat/integer.h#L595)):

> the min/max are updated with level zero propagation, **but not holes**.

Interior removal requires
`IntegerEncoder::GetOrCreateLiteralAssociatedToEquality`
([integer.h:194](ortools/sat/integer.h#L194)). `all_different` documents the same
trade-off ([all_different.h:44-69](ortools/sat/all_different.h#L44)): bounds-only
propagation "will not remove already taken values from inside a domain", while the AC
version "will fully encode variables", pruning by negating a cached literal
([all_different.cc:407](ortools/sat/all_different.cc#L407)).

**This is CP-SAT-specific.** In a bitset-domain CP solver (Gecode, Choco, OR-Tools'
legacy `Solver`) a global propagator *would* beat `n-1` pair propagators on memory —
that intuition motivated the original request. It does not transfer.

### 3.3 The bound fast path — not applicable here

When the forbidden successors of a value form a prefix or suffix of the domain, the
constraint becomes `[x[i]=v] ⇒ x[i+1] ≤ c`, which CP-SAT stores and propagates
without encoding `x[i+1]` at all
([precedences.h:548](ortools/sat/precedences.h#L548), `ConditionalLinear2Bounds`).
Measured at 6× fewer variables (§9.1). It needs contiguous forbidden sets, which the
CCBP pairs are not. Recorded because renumbering actions so common forbidden sets
become intervals would unlock it — a consecutive-ones-property problem, out of scope,
and the only known route to reducing the Boolean count.

### 3.4 The cost model

`n` = plan length, `|F|` = pairs, `d` = actions.

| term | per-step tables | `transitions` |
|---|---|---|
| proto pairs | `(n-1)·|F|`, held in the input model *and* the presolve working copy | `|F|` |
| proto exprs | `2(n-1)` | `n` |
| expanded clauses | `(n-1)·|F|` binary | **unchanged** |
| Booleans added | **0** (§3.1) | **0** |

Both the proto term and the clause term scale as `(n-1)·|F|`, at roughly 32 bytes and
~600 bytes per pair-position respectively. **The ratio is scale-invariant**: this
constraint saves ~5% of peak memory for any `n` and `|F|`, never more. §9.1 measured
it; §9's original estimate had the clause side low by more than an order of
magnitude, because during presolve each clause is a full `ConstraintProto`, not the
8-16 bytes it eventually costs in `BinaryImplicationGraph`.

---

## 4. Implementation

Validated, presolved, then fully expanded into clauses before loading. **No
`cp_model_loader.cc` case and no propagator.**

### 4.1 Validation

`ValidateTransitionsConstraint`, cased into the switch at
[cp_model_checker.cc:1130](ortools/sat/cp_model_checker.cc#L1130) (its `default:` is
a silent `break`): rejects `PAIRS_NOT_SET` and odd pair-list sizes, and validates
each expression.

`TransitionsConstraintIsFeasible`, cased into
[cp_model_checker.cc:1878](ortools/sat/cp_model_checker.cc#L1878) — whose `default:`
is **`LOG(FATAL)`**, so this one is mandatory. It builds the pair set once and scans
consecutive values, with the polarity taken from which `oneof` arm is set.

### 4.2 Presolve

Not implemented. The planned two-sweep domain filtering (forward: single-valued
`D[i]` prunes `D[i+1]`; backward: unsupported values leave `D[i]`) is optional and
was skipped once §9 showed the win was not where expected. Presolve reaches the same
fixpoint through the expanded clauses anyway.

### 4.3 Expansion

`ExpandTransitions` in `cp_model_expand.cc`, dispatched from `ExpandCpModel`
alongside the `kTable` case at
[cp_model_expand.cc:2863](ortools/sat/cp_model_expand.cc#L2863) — note that case
expands **unconditionally**, with only canonicalization gated on
`params().cp_model_presolve()`; §6.3 explains why that matters.

**Forbidden** — the inner loop of `ExpandNegativeTable` with the position loop hoisted
out. Per position and pair, map both values back through the affine expressions, skip
if unreachable or out of domain, then emit
`bool_or(¬[x_i=a], ¬[x_{i+1}=b])` through `context->AddEnforcedConstraint(ct)` so
outer enforcement literals are carried onto each clause. An empty clause is the
correct outcome when both sides are fixed to a forbidden pair.

**Allowed** — per position this is exactly a positive table on two variables, so it
reuses the existing `AddSizeTwoTable`
([cp_model_expand.cc:1822](ortools/sat/cp_model_expand.cc#L1822)), which emits
per-value *support* clauses (~`2d` per position) rather than one clause per pair.
That is far smaller than complementing the relation would be, and it is why §8's
answer to "should the allowed form just be reversed into forbidden?" is no: at
`d=100, A=2000, n=50`, reversing shrinks the proto 12× and grows the clauses 40×.

Per position the allowed path filters the pairs against both current domains,
tightens both domains to the surviving values, and calls the helper. A single forward
sweep is enough for correctness — clauses emitted earlier stay sound if a domain
shrinks later. It falls back to emitting one positive `TableConstraintProto` per
position where `AddSizeTwoTable` does not apply (enforcement literals,
`detect_table_with_cost`, constant expressions); those are appended to the working
model and picked up by the same expansion loop.

**No new Booleans, therefore no hint crushing needed**, and
`enumerate_all_solutions` counts are unaffected — unlike the automaton work
(`nd-report.md` §5.1).

Also required: a case in the `domain_of_var_is_used` bookkeeping switch at
[cp_model_expand.cc:2669](ortools/sat/cp_model_expand.cc#L2669) setting
`domain_is_used = true`, beside `kTable`/`kAutomaton`. Omitting it silently degrades
all-different expansion decisions.

### 4.4 Plumbing checklist

Compiler-forced (exhaustive switches with no `default:`). The report originally
listed five; the compiler found **seven**:

| site | what |
|---|---|
| [cp_model_utils.cc:110](ortools/sat/cp_model_utils.cc#L110) `GetReferencesUsedByConstraint` | indices of each `exprs(k).vars()` |
| [cp_model_utils.cc:254](ortools/sat/cp_model_utils.cc#L254) `ApplyToAllLiteralIndices` | nothing |
| [cp_model_utils.cc:317](ortools/sat/cp_model_utils.cc#L317) `ApplyToAllVariableIndices` | remap `exprs` vars |
| [cp_model_utils.cc:438](ortools/sat/cp_model_utils.cc#L438) `ApplyToAllIntervalIndices` | nothing |
| `cp_model_utils.cc` `UsedIntervals` | nothing |
| [cp_model_utils.cc:499](ortools/sat/cp_model_utils.cc#L499) `ConstraintCaseName` | `"kTransitions"` |
| `cp_model_utils.cc` `FingerprintModel` | exprs + `pairs_case()` + the pair list |

Silent, and therefore the ones that bite:

| site | `default:` | if skipped |
|---|---|---|
| [cp_model_checker.cc:1199](ortools/sat/cp_model_checker.cc#L1199) | `break` | malformed models accepted |
| [cp_model_checker.cc:1878](ortools/sat/cp_model_checker.cc#L1878) | **`LOG(FATAL)`** | crash in every test that verifies a solution |
| [cp_model_loader.cc:1752](ortools/sat/cp_model_loader.cc#L1752) | `return false` | → [cp_model_solver_helpers.cc:1399](ortools/sat/cp_model_solver_helpers.cc#L1399), `"BUG: We will wrongly report INFEASIBLE now."` (§6.3) |
| [cp_model_symmetries.cc:576](ortools/sat/cp_model_symmetries.cc#L576) | `return nullptr` | **symmetry detection off model-wide** (§6.2) |
| [constraint_violation.cc:1880](ortools/sat/constraint_violation.cc#L1880) | `VLOG(1)` | silently absent from the LS model (§6.2) |
| [cp_model_copy.cc:221](ortools/sat/cp_model_copy.cc#L221) | verbatim copy | **works as-is** |
| `var_domination.cc`, `primary_variables.cc`, `linear_relaxation.cc` | conservative / no-op | safe once expanded |

`cp_model_lns.cc` needs nothing — it uses the generic `UsedVariables()`.

### 4.5 Language APIs

Python `CpModel` methods are **not** proto builders; they call pybind entry points on
`CpBaseModel`. So the Python API needed two changes:

- `python/cp_model_helper.cc` — `CpBaseModel::AddTransitionsInternal(exprs, pairs,
  forbidden)` plus the `.def("_add_transitions", …)` binding, next to `_add_table`;
- `python/cp_model.py` — `add_forbidden_transitions`, `add_allowed_transitions`,
  `add_not_after`, `add_not_before`.

C++/Java/C#/Go wrappers were not added; only Python is needed for CCBP.

---

## 5. Alternatives, ranked

**A. Keep the per-step tables, fix the cheap things.** Delete
`add_consecutive_actions_constraints_booleans` (§3.1); prefer the `Not*` form when
`S` is large. No solver change. *In hindsight this was within ~2% of the outcome
(§10).*

**B. Expand to an `automaton`.** The relation is regular, so no new constraint type.
Measured 3-8× *worse* in variables and up to 5× worse in clause literals (§9.1).
Dead; kept only as a third oracle in the §7 fuzz.

**C. New proto constraint, presolve-expanded (§4).** *What was built.*

**D. New propagator.** No filtering gain at `w=1` (§2), no variable saving (§3.2),
and it costs the CDCL learning and `BinaryImplicationGraph` reductions the clauses
feed (§6.5). §10 now argues it is the only remaining lever on the clause term — and
also that the clause term may not be worth removing.

---

## 6. Implications and risks

### 6.1 Reviewability

Any upstream reviewer asks §2 first: *what does this prune that per-pair tables do
not?* Nothing. The case rests on model size, and §10 shows that case is weak in
practice. Not upstreamable as it stands.

### 6.2 Two silent-degradation sites, both saved by ordering

`DetectAndAddSymmetryToProto` runs at
[cp_model_presolve.cc:13987](ortools/sat/cp_model_presolve.cc#L13987), *after*
`ExpandCpModelAndCanonicalizeConstraints()` at
[:13964](ortools/sat/cp_model_presolve.cc#L13964) — so an always-expanded constraint
never reaches [cp_model_symmetries.cc:576](ortools/sat/cp_model_symmetries.cc#L576).
And `constraint_violation.cc` has no `kTable`/`kAutomaton`/`kElement` case at all,
confirming expanded constraints never reach the `LsEvaluator`.

Both reduce to one invariant: **the constraint is always expanded.** Which is why
option D is a materially *larger* change than C, not a smaller one.

### 6.3 `disable_constraint_expansion`

Under that parameter `ExpandCpModel` returns immediately
([cp_model_expand.cc:2809](ortools/sat/cp_model_expand.cc#L2809)) and the loader has
no case, so the solver reports INFEASIBLE with a `"BUG:"` log line. Pre-existing for
`table` and `automaton`; do not widen it by gating expansion on
`params().cp_model_presolve()`.

### 6.4 Cross-version proto hazard

An **older** solver parsing a model with field 31 set puts it in the unknown-field
set, `constraint_case()` returns `CONSTRAINT_NOT_SET`, and `ConstraintIsFeasible`
treats it as always feasible. The old binary then silently returns plans that violate
the constraint — no error, no warning. This is recorded in
`CCBP/tools/or-tools/README.md` because CCBP now depends on a locally built wheel.

### 6.5 Why the clauses may be worth keeping (the case against option D)

The `(n-1)·|F|` binary clauses live in `BinaryImplicationGraph`, which stores the
2-SAT part separately, never deletes it, and runs equivalence detection and
transitive reduction over it
([clause.h:575-587](ortools/sat/clause.h#L575)). On a planning model the implication
structure between actions at consecutive steps is exactly what drives propagation and
gets learned from. Replacing it with a propagator trades memory for a plausible
solve-time regression. Measure solve time, not just RSS.

---

## 7. Tests

`cp_model_expand_test.cc`, 10 tests, all passing; 79 in the file overall.

1. **`MatchesBruteForceAndTables` — the equivalence fuzz.** For both directions,
   `n ∈ [0,4]`, `d ∈ [1,3]`, random pair sets, 6 trials each — 180 model pairs. Each
   is solved natively *and* as per-step tables with `enumerate_all_solutions`, and
   both are compared against a pure-C++ brute force over the whole cube. Solution
   *sets* over the original variables, not just counts.
2. Direction and edge cases: `NoForbiddenPairsIsTriviallyTrue`,
   `NoAllowedPairsIsInfeasible`, `SingleExpressionIsTriviallyTrue`,
   `AffineExpressions` (including a value unreachable under `2x`),
   `AllowedWithEnforcementLiteral` (exercises the table fallback),
   `ForbiddenWithEnforcementLiteral`, `FixedVariablesMakeItInfeasible`,
   `AllowedChainPropagates`.

Regression at the time of writing: expand 79, checker 40, utils 22, table 16,
presolve 200 — all green.

**Two bugs the harness caught, both mine:**

- **Inverted polarity in the feasibility checker** — `contains(pair) != use_forbidden`
  where it should be `==`. The solver produced correct solutions and the checker
  rejected them. Caught by the `SolutionIsFeasible` assertion inside the fuzz
  harness, which is exactly why that assertion earns its place.
- **`result.insert({})` in the brute-force reference** binds to the
  `initializer_list` overload and inserts *zero* elements, so the expected set was
  empty for `n = 0`. A test bug that masqueraded as a product bug.

---

## 8. Should the allowed form be reversed into the forbidden one?

No. With `d` actions, `A` allowed pairs, `n` steps:

| | proto pairs | expanded clauses |
|---|---|---|
| reverse (complement → forbidden) | `d² − A`, once | `(n−1)·(d² − A)` **binary** |
| keep allowed (`AddSizeTwoTable`) | `A`, once | `(n−1)·~2d` support clauses |

`AddSizeTwoTable` emits one support clause *per value* — `~2d` per position
regardless of pair count — while the forbidden form emits one binary clause *per
pair*. At `d=100, A=2000, n=50`: reversing gives 392,000 binary clauses against
~9,800. Since §9.1 measured the expansion outweighing the proto by ~20×, reversing
trades a 12× proto win for a 40× clause loss.

Hence both directions are native, and the CCBP caller's existing "post whichever set
is smaller" heuristic ([ai_planning.py:86](/home/damienvm/Desktop/CCBP/driver/cp_sat/ai_planning.py#L86))
is respected rather than overridden.

---

## 9. Synthetic measurements (2026-09-01)

### 9.1 Encoding comparison

Measured with the wheel from `nd-report.md` §6 on a synthetic `NotAfter(v, S)` chain,
`n = 365`, with `cp_model_presolve:false, stop_after_presolve:true` so the expansion
output is visible.

| encoding | input proto | expanded vars | expanded constraints |
|---|---|---|---|
| **d=5, \|S\|=2** | | | |
| per-pair `add_forbidden_assignments` | 15,299 B | 1,457 (1,092 Bool) | 728 `bool_or` + 2,184 `linear1` |
| bound form (§3.3) | 41,633 B | **729** (364 Bool) | **1,092 `linear1`, zero clauses** |
| automaton (§5 B) | 7,468 B | 4,738 | 2,912 `exactly_one` (13,468 lits) |
| **d=30, \|S\|=10** | | | |
| per-pair | 21,123 B | 4,369 (4,004 Bool) | 3,640 `bool_or` + 8,008 `linear1` |
| bound form | 41,633 B | **729** (364 Bool) | **1,092 `linear1`** |
| automaton | 7,594 B | 11,719 | 18,150 `bool_or` (54,450 lits) |

Established: **input proto size is not the memory that matters** (the bound form has
the largest proto and the smallest model; the automaton the reverse); expansion
output is identical between native and decomposed, arithmetically
(`1,092 = 3·364` encoding literals, `728 = 364·|S|` clauses); the *encoding choice*
dominates, but the bound form needs contiguous sets, which CCBP lacks; and option B
is dead.

### 9.2 After implementation

`n = 100`, `d = 200`, `|F| = 8,892` scattered pairs, built to match the CCBP shape.

With `cp_model_presolve:false` both formulations produce **identically**
`20,100` variables, `880,308` `bool_or`, `1,760,616` literals; with presolve on both
reduce to the same `19,982` constraints / `920,190` terms. Strong evidence that the
constraint is a pure representation change, on top of the fuzz.

| | tables | native | delta |
|---|---|---|---|
| input constraints | 99 | 1 | |
| pair int64s in the input model | 1,760,616 | 17,784 | **99×** |
| peak RSS, `cp_model_presolve:false` | 283 MB | 249 MB | −12% |
| presolve wall, `cp_model_presolve:false` | 1.01 s | 0.44 s | **−2.3×** |
| peak RSS, presolve on | 625 MB | 596 MB | **−4.6%** |
| presolve wall, presolve on | 6.87 s | 6.26 s | −9% |

The absolute saving matches the predicted proto duplication exactly
(`2·8892·8 B·2 copies·99 positions ≈ 28 MB`). The disappointment is the denominator.

### 9.3 Caveat that turned out to matter

A lone transitions chain is trivially satisfiable, so with presolve *enabled* these
models collapse to `#Variables: 0`. Hence the presolve-disabled figures. These
numbers isolate encoding cost and say nothing about interaction with the rest of the
CCBP model — which §10 then measured, and which changed the conclusion.

---

## 10. Real evaluation (2026-09-02) — the constraint changed almost nothing

`experiment-data/2026-09-02-symmetry-constraints-eval`, 1836 tasks per configuration,
revision `bbca0ba47`.

### 10.1 Coverage

```
00-base                        cov=735   oot=629  oom-solving=402  oom-building=44
A1-table-opposite              cov=729   oot=631  oom-solving=397  oom-building=48
A2-transitions-opposite        cov=732   oot=625  oom-solving=403  oom-building=44
B1-table-commuting             cov=657   oot=404  oom-solving=473  oom-building=203
B2-transitions-commuting       cov=658   oot=423  oom-solving=540  oom-building=115
C2-transitions-all             cov=654   oot=415  oom-solving=540  oom-building=116
```

`transitions` over `table`: **+3** on the opposite branch, **+1** on the commuting
branch.

### 10.2 Why: the target was ~2% of the model

Paired medians over the tasks where both ran:

| | opposite | commuting |
|---|---|---|
| `cp_n_constraints_init` | 856 → **841** | 758 → **738** |
| `cp_n_variables_init` | 793 → 793 | 716 → 716 |
| `runlim_space_MB` | 74 → 74 | 76 → 76 |
| `cp_build_time_max_s` (sum ratio) | 1.012 | **0.624** |

The constraint collapses `plan_length − 1` constraints into 1, but plan lengths are
~16-24 and the model has ~850 constraints from the automata and element constraints.
So it removes ~15-20 of ~850. §9.2's synthetic benchmark used `n = 100` in a model
containing *nothing but* the transitions constraint, which overstated the effect by
an order of magnitude.

### 10.3 Where it did work, the memory moved rather than vanished

Model-building OOMs on the commuting branch fell **203 → 115**, and build time
dropped 38%. Following those tasks:

```
B1-table-commuting 'oom-building_model' tasks, outcome under B2-transitions-commuting:
     115  oom-building_model
      88  oom-solving
```

All 88 died in `oom-solving` instead (`oom-solving` +67, `oot-solving` +19). The
failure moved from build to solve. Nothing new was solved.

This is exactly what §3.4 predicts: the proto duplication is ~5% of peak, the
expansion output is the rest, and this constraint emits the same clauses. Removing
the input-model copy just lets the process survive long enough to run out of memory
on the clauses.

### 10.4 The finding that is not about this constraint

**Every consecutive-action configuration is worse than posting nothing** — `opposite`
costs 3-6 coverage, `commuting` costs ~77. The posting mechanism is a rounding error
beside that. If coverage is the goal, the question is why these constraints hurt, not
how to post them more compactly.

Where the effort went next: `landmark-ordering-report.md`, which applies this
report's lesson *before* building anything — and finds a target that is 2.5-3× of the
model and whose constraints actually gain coverage.

---

## 11. Decision log

| decision | why |
|---|---|
| one constraint, not four | all four user forms flatten to a pair set (§1.2) |
| flat pair list, not `from_domain`/`to_domain` | the domain form encodes a single rectangle; CCBP pairs are scattered (§1.1) |
| `oneof` of submessages, not two repeated fields | repeated fields have no presence; "both empty" would be unsound for an empty allowed-list (§1.1) |
| both directions native | complementing allowed→forbidden trades a 12× proto win for a 40× clause loss (§8) |
| allowed reuses `AddSizeTwoTable` | support clauses per value, not per pair; no new encoding logic (§4.3) |
| expansion, not a propagator | no filtering gain (§2), no variable saving (§3.2), clauses feed CDCL (§6.5) |
| no `cp_model_loader.cc` case | keeps the "always expanded" invariant that saves symmetry detection and the LS workers (§6.2) |
| no presolve function | optional; the expanded clauses reach the same fixpoint (§4.2) |
| no hint crushing | the expansion creates no new Booleans (§4.3) |
| rectangle form deferred | judged inapplicable — **§10.4 revisits this**: commuting matrices *are* block-structured |
| windowed version deferred | the one variant where a propagator genuinely earns its keep (§1.4) |

---

## 12. Shipping it

### 12.1 Python package

The wheel is built per `nd-report.md` §6, which now also carries the two pitfalls
found here — both silently produce a wheel without your changes:

- `ortools/sat/python/cp_model.py` is installed by `file(COPY …)` at
  `cmake/python.cmake:410`, which runs at **configure** time. A Python-source edit
  needs `cmake -S. -Bbuild` before rebuilding.
- `python_package` is stamp-gated; `rm -f build/python/{dist,ortools}_timestamp`
  forces it. Do **not** delete `build/python/dist` — the target's last step installs
  from it and falls through to PyPI.

And, mandatory before the wheel leaves this machine (`nd-report.md` §7):

- **Bundle libstdc++ under a mangled soname.** `python_package` always writes an
  unpatched wheel, which imports fine on Ubuntu 22.04 and fails on Arrhenius
  (AlmaLinux 9) with `GLIBCXX_3.4.30 not found`. Scripted as
  `CCBP/tools/or-tools/patch_wheel_libstdcxx.sh`; it self-verifies. Watch for the
  `set -o pipefail` + `grep -q` SIGPIPE trap that silently skipped `libhighs.so.1`
  (88 of 89 libraries patched) — the script documents it.

### 12.2 CCBP integration

- `add_consecutive_actions_constraints_transitions` in
  `driver/cp_sat/model/planning_model_interface.py`, native in both directions.
- `--consecutive-actions-posting {table, boolean, transitions}` in
  `driver/arguments.py`; default still `table`.
- The wheel is vendored at `CCBP/tools/or-tools/` with a README covering provenance,
  the rebuild recipe, and the caveats. `uv.lock` pins the wheel hash, so a rebuild
  needs `uv lock --upgrade-package ortools` before `uv sync`.
- `pyproject.toml` `requires-python` is pinned to `>=3.10,<3.11` because the wheel is
  cp310-only.

Verified end-to-end across 6 domains × both branches: identical variable counts,
identical optimal costs, all plans validate, `plan_length − 1 → 1` constraints.

### 12.3 Unrelated fix made along the way

`AIPlanningModel.__init__` now sets `self._plan = []` *before* `super().__init__()`.
Since ortools 9.15, `CpModel.__init__` calls `_add_pre_pep8_methods()`, which
`getattr`s every name in `dir(self)` — firing the `horizon` property getter before any
subclass has set `_plan`. A 9.14 → 9.15 incompatibility, independent of this
constraint, that would have hit on any upgrade. 
