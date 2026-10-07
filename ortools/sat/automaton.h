// Copyright 2010-2025 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ORTOOLS_SAT_AUTOMATON_H_
#define ORTOOLS_SAT_AUTOMATON_H_

#include <vector>

#include "absl/types/span.h"
#include "ortools/sat/enforcement.h"
#include "ortools/sat/enforcement_helper.h"
#include "ortools/sat/integer.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_base.h"

namespace operations_research {
namespace sat {

// Propagates an automaton constraint, deterministic or not, directly on the
// literals (exprs[t] == label), without any auxiliary variable: the constraint
// is satisfied iff the label sequence has at least one accepting run.
//
// Each call does a forward pass (states reachable from the start) and a
// backward pass (states from which a final state is reachable) on the layered
// graph of the automaton, ignoring the transitions whose label literal is
// false. A label literal at time t is supported iff one of its transitions
// goes from a forward-reachable state to a backward-reachable one; all the
// unsupported ones are set to false. This achieves domain consistency on the
// labels (Pesant, "A Regular Language Membership Constraint for Finite
// Sequences of Variables", CP 2004).
//
// Explanations are cuts of the layered graph: the false label literals of the
// transitions leaving the forward-reachable states before time t, and of the
// transitions entering the backward-reachable states after time t.
class AutomatonPropagator : PropagatorInterface {
 public:
  // States and labels must be reindexed in [0, num_states) and
  // [0, num_labels). label_literals[t * num_labels + l] must be the literal
  // (exprs[t] == label l), it can be a fixed literal.
  AutomatonPropagator(int num_states, int starting_state,
                      absl::Span<const int> final_states,
                      absl::Span<const int> tails, absl::Span<const int> labels,
                      absl::Span<const int> heads, int num_labels,
                      absl::Span<const Literal> label_literals,
                      absl::Span<const Literal> enforcement_literals,
                      Model* model);

  // This type is neither copyable nor movable.
  AutomatonPropagator(const AutomatonPropagator&) = delete;
  AutomatonPropagator& operator=(const AutomatonPropagator&) = delete;

  bool Propagate() final;

 private:
  int RegisterWith(GenericLiteralWatcher* watcher);

  Literal LabelLiteral(int time, int label) const {
    return label_literals_[time * num_labels_ + label];
  }
  bool TransitionIsRemoved(int time, int transition) const {
    return assignment_.LiteralIsFalse(
        LabelLiteral(time, transition_labels_[transition]));
  }

  // Appends to reason_ the literals of the forward (resp. backward) cut at
  // the given time step.
  void AppendForwardCut(int time);
  void AppendBackwardCut(int time);

  const int num_states_;
  const int num_labels_;
  const int num_steps_;
  const int starting_state_;
  std::vector<bool> is_final_;
  std::vector<int> transition_tails_;
  std::vector<int> transition_labels_;
  std::vector<int> transition_heads_;
  std::vector<Literal> label_literals_;

  const VariablesAssignment& assignment_;
  EnforcementHelper& enforcement_helper_;
  EnforcementId enforcement_id_;

  // forward_[t * num_states_ + s] is true iff s is reachable at time t from the
  // starting state. backward_[t * num_states_ + s] is true iff a final state is
  // reachable at time n from s at time t.
  std::vector<bool> forward_;
  std::vector<bool> backward_;
  std::vector<bool> supported_;
  std::vector<Literal> reason_;
};

}  // namespace sat
}  // namespace operations_research

#endif  // ORTOOLS_SAT_AUTOMATON_H_
