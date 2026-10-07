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

#include "ortools/sat/automaton.h"

#include <vector>

#include "absl/log/check.h"
#include "absl/types/span.h"
#include "ortools/base/stl_util.h"
#include "ortools/sat/enforcement.h"
#include "ortools/sat/enforcement_helper.h"
#include "ortools/sat/integer.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_base.h"

namespace operations_research {
namespace sat {

AutomatonPropagator::AutomatonPropagator(
    int num_states, int starting_state, absl::Span<const int> final_states,
    absl::Span<const int> tails, absl::Span<const int> labels,
    absl::Span<const int> heads, int num_labels,
    absl::Span<const Literal> label_literals,
    absl::Span<const Literal> enforcement_literals, Model* model)
    : num_states_(num_states),
      num_labels_(num_labels),
      num_steps_(label_literals.size() / num_labels),
      starting_state_(starting_state),
      is_final_(num_states, false),
      transition_tails_(tails.begin(), tails.end()),
      transition_labels_(labels.begin(), labels.end()),
      transition_heads_(heads.begin(), heads.end()),
      label_literals_(label_literals.begin(), label_literals.end()),
      assignment_(model->GetOrCreate<Trail>()->Assignment()),
      enforcement_helper_(*model->GetOrCreate<EnforcementHelper>()) {
  CHECK_GT(num_labels, 0);
  CHECK_EQ(label_literals.size() % num_labels, 0);
  CHECK_EQ(tails.size(), labels.size());
  CHECK_EQ(tails.size(), heads.size());
  for (const int f : final_states) is_final_[f] = true;
  forward_.resize((num_steps_ + 1) * num_states_);
  backward_.resize((num_steps_ + 1) * num_states_);
  supported_.resize(num_labels_);

  GenericLiteralWatcher* watcher = model->GetOrCreate<GenericLiteralWatcher>();
  enforcement_id_ = enforcement_helper_.Register(enforcement_literals, watcher,
                                                 RegisterWith(watcher));
}

int AutomatonPropagator::RegisterWith(GenericLiteralWatcher* watcher) {
  const int id = watcher->Register(this);
  // We only care about label literals becoming false.
  std::vector<Literal> watched = label_literals_;
  gtl::STLSortAndRemoveDuplicates(&watched);
  for (const Literal literal : watched) {
    watcher->WatchLiteral(literal.Negated(), id);
  }
  // When the same variable appears at several time steps, removing a label at
  // one step can remove more labels at another one.
  watcher->NotifyThatPropagatorMayNotReachFixedPointInOnePass(id);
  return id;
}

void AutomatonPropagator::AppendForwardCut(int time) {
  const int offset = time * num_states_;
  for (int i = 0; i < transition_tails_.size(); ++i) {
    if (forward_[offset + transition_tails_[i]] &&
        !forward_[offset + num_states_ + transition_heads_[i]]) {
      DCHECK(TransitionIsRemoved(time, i));
      reason_.push_back(LabelLiteral(time, transition_labels_[i]));
    }
  }
}

void AutomatonPropagator::AppendBackwardCut(int time) {
  const int offset = time * num_states_;
  for (int i = 0; i < transition_tails_.size(); ++i) {
    if (backward_[offset + num_states_ + transition_heads_[i]] &&
        !backward_[offset + transition_tails_[i]]) {
      DCHECK(TransitionIsRemoved(time, i));
      reason_.push_back(LabelLiteral(time, transition_labels_[i]));
    }
  }
}

bool AutomatonPropagator::Propagate() {
  const EnforcementStatus status = enforcement_helper_.Status(enforcement_id_);
  if (status != EnforcementStatus::CAN_PROPAGATE_ENFORCEMENT &&
      status != EnforcementStatus::IS_ENFORCED) {
    return true;
  }

  const int num_transitions = transition_tails_.size();

  // Forward pass.
  forward_.assign(forward_.size(), false);
  forward_[starting_state_] = true;
  for (int t = 0; t < num_steps_; ++t) {
    const int offset = t * num_states_;
    for (int i = 0; i < num_transitions; ++i) {
      if (forward_[offset + transition_tails_[i]] &&
          !TransitionIsRemoved(t, i)) {
        forward_[offset + num_states_ + transition_heads_[i]] = true;
      }
    }
  }

  bool accepted = false;
  for (int s = 0; s < num_states_; ++s) {
    if (is_final_[s] && forward_[num_steps_ * num_states_ + s]) {
      accepted = true;
      break;
    }
  }
  if (!accepted) {
    reason_.clear();
    for (int t = 0; t < num_steps_; ++t) AppendForwardCut(t);
    gtl::STLSortAndRemoveDuplicates(&reason_);
    if (status == EnforcementStatus::IS_ENFORCED) {
      return enforcement_helper_.ReportConflict(enforcement_id_, reason_,
                                                /*integer_reason=*/{});
    }
    return enforcement_helper_.PropagateWhenFalse(enforcement_id_, reason_,
                                                  /*integer_reason=*/{});
  }
  if (status != EnforcementStatus::IS_ENFORCED) return true;

  // Backward pass.
  backward_.assign(backward_.size(), false);
  for (int s = 0; s < num_states_; ++s) {
    backward_[num_steps_ * num_states_ + s] = is_final_[s];
  }
  for (int t = num_steps_ - 1; t >= 0; --t) {
    const int offset = t * num_states_;
    for (int i = 0; i < num_transitions; ++i) {
      if (backward_[offset + num_states_ + transition_heads_[i]] &&
          !TransitionIsRemoved(t, i)) {
        backward_[offset + transition_tails_[i]] = true;
      }
    }
  }

  // Remove the unsupported labels.
  for (int t = 0; t < num_steps_; ++t) {
    const int offset = t * num_states_;
    supported_.assign(num_labels_, false);
    for (int i = 0; i < num_transitions; ++i) {
      if (forward_[offset + transition_tails_[i]] &&
          backward_[offset + num_states_ + transition_heads_[i]]) {
        supported_[transition_labels_[i]] = true;
      }
    }

    bool reason_is_computed = false;
    for (int l = 0; l < num_labels_; ++l) {
      if (supported_[l]) continue;
      const Literal literal = LabelLiteral(t, l);
      if (assignment_.LiteralIsFalse(literal)) continue;

      // The reason is the same for all the labels of this time step.
      if (!reason_is_computed) {
        reason_is_computed = true;
        reason_.clear();
        for (int before = 0; before < t; ++before) AppendForwardCut(before);
        for (int after = t + 1; after < num_steps_; ++after) {
          AppendBackwardCut(after);
        }
        gtl::STLSortAndRemoveDuplicates(&reason_);
      }
      if (!enforcement_helper_.EnqueueLiteral(enforcement_id_,
                                              literal.Negated(), reason_,
                                              /*integer_reason=*/{})) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace sat
}  // namespace operations_research
