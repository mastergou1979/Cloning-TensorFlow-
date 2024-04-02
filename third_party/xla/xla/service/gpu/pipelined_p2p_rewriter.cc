
/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/pipelined_p2p_rewriter.h"

#include <stdbool.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "xla/hlo/ir/dfs_hlo_visitor.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_query.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/util.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

namespace {
// Maps a computation to a boolean that indicates whether the computation
// invokes collective operations directly or indirectly.
using CollectiveInComputation =
    absl::flat_hash_map<const HloComputation*, bool>;

using InstructionVector = HloInstruction::InstructionVector;

// Returns whether the instruction is a collective operation.
bool IsCollectiveOp(const HloInstruction* op) {
  HloOpcode opcode = op->opcode();
  // TODO(b/309639264): we need to avoid custom-calls to overlap with Send/Recv
  // to workaround the bug. Remove custom-calls here when the bug is fixed.
  if (opcode == HloOpcode::kCustomCall) {
    return true;
  }

  return hlo_query::IsCollectiveCommunicationOp(opcode) ||
         opcode == HloOpcode::kSend || opcode == HloOpcode::kRecv;
}

// Returns whether the instruction may invoke collective operations directly
// or indirectly.
bool MayInvokeCollectiveOp(
    const HloInstruction* hlo,
    const CollectiveInComputation& collective_in_computation) {
  if (IsCollectiveOp(hlo)) {
    return true;
  }
  for (auto callee : hlo->called_computations()) {
    auto collective_in_comp = collective_in_computation.find(callee);
    if (collective_in_comp != collective_in_computation.end() &&
        collective_in_comp->second) {
      return true;
    }
  }
  return false;
}

// Returns the unique get-tuple-element user with the given idx or nullptr if
// there isn't such a unique user.
HloInstruction* FindUniqueGTEUserWithIndex(const HloInstruction* op,
                                           int64_t idx) {
  CHECK(op->shape().IsTuple());

  HloInstruction* gte = nullptr;
  for (auto user : op->users()) {
    if (user->opcode() != HloOpcode::kGetTupleElement) {
      continue;
    }
    if (user->tuple_index() == idx) {
      if (gte == nullptr) {
        gte = user;
      } else {
        return nullptr;
      }
    }
  }
  return gte;
}

// Returns whether there is any get-tuple-element user with the given idx.
bool HasGTEUserWithIndex(const HloInstruction* op, int64_t idx) {
  CHECK(op->shape().IsTuple());

  for (auto user : op->users()) {
    if (user->opcode() != HloOpcode::kGetTupleElement) {
      continue;
    }
    if (user->tuple_index() == idx) {
      return true;
    }
  }
  return false;
}

// Checks whether the while-op, its while-body and while-condition have a
// recognized pipelined pattern. If a pipelined pattern is found, returns the
// first and last indices for the pipelined instruction in the while-init tuple.
// Otherwise, return (0, 0).
std::pair<int64_t, int64_t> FindPipelinedP2P(const HloInstruction* while_op) {
  VLOG(10) << "while_op: " << while_op->ToString();
  const HloInstruction* while_init = while_op->while_init();
  if (while_init->opcode() != HloOpcode::kTuple) return std::make_pair(0, 0);

  // The while-body and while-condition should have one parameter of a tuple
  // shape.
  const HloComputation* while_body = while_op->while_body();
  const HloComputation* while_condition = while_op->while_condition();
  if (while_body->num_parameters() != 1 ||
      while_condition->num_parameters() != 1) {
    return std::make_pair(0, 0);
  }

  int64_t opnd_start = 0;
  int64_t opnd_tot = 0;
  enum State { kNotStarted, kStarted, kEnded };
  State state = kNotStarted;
  // If SendDone/RecvDone exists in a consecutive block in the while-init
  // tuple, find such block.
  for (int64_t i = 0; i < while_init->operand_count(); ++i) {
    const HloInstruction* op = while_init->operand(i);
    if (op->opcode() == HloOpcode::kRecvDone ||
        op->opcode() == HloOpcode::kSendDone) {
      if (state == kNotStarted) {
        state = kStarted;
        opnd_start = i;
        opnd_tot = 1;
      } else if (state == kStarted) {
        opnd_tot++;
      } else if (state == kEnded) {
        opnd_tot = 0;
      }
    } else {
      if (state == kStarted) {
        state = kEnded;
      }
    }
  }
  if (opnd_tot == 0) {
    VLOG(10) << "No SendDone/RecvDone in while-init or SendDone/RecvDone "
                "aren't in a consecutive block";
    return std::make_pair(0, 0);
  }
  int opnd_end = opnd_start + opnd_tot;

  VLOG(10) << "opnd_start " << opnd_start << " opnd_end " << opnd_end;

  // The number of SendDone and RecvDone in the blocks are the same. In the
  // while-result or while-body parameter, the index for RecvDone should
  // correspond one get-tuple-element user and the index for SendDone should
  // not correspond to any get-tuple-element user.
  int64_t difference = 0;
  for (int64_t i = opnd_start; i < opnd_end; ++i) {
    const HloInstruction* op = while_init->operand(i);
    if (op->opcode() == HloOpcode::kRecvDone) {
      difference++;
      HloInstruction* gte = FindUniqueGTEUserWithIndex(while_op, i);
      if (gte == nullptr) {
        VLOG(10) << "While result get-tuple-element user with index " << i
                 << " not unique";
        return std::make_pair(0, 0);
      }
      gte = FindUniqueGTEUserWithIndex(while_body->parameter_instruction(0), i);
      if (gte == nullptr) {
        VLOG(10) << "While-body parameter get-tuple-element user with index "
                 << i << " not unique";
        return std::make_pair(0, 0);
      }
    } else {
      CHECK(op->opcode() == HloOpcode::kSendDone);
      difference--;
      if (HasGTEUserWithIndex(while_op, i) ||
          HasGTEUserWithIndex(while_body->parameter_instruction(0), i)) {
        VLOG(10) << "SendDone with index " << i << " has unexpected users";
        return std::make_pair(0, 0);
      }
    }
  }

  if (difference != 0) {
    VLOG(10) << "Mismatch number of SendDone and RecvDone: " << difference;
    return std::make_pair(0, 0);
  }

  // The element in the while-body result tuple corresponding to the pipelined
  // SendDone/RecvDone in the while-init have the same opcode.
  const HloInstruction* root = while_body->root_instruction();
  for (int64_t i = opnd_start; i < opnd_end; ++i) {
    const HloInstruction* op_init = while_init->operand(i);
    const HloInstruction* op_root = root->operand(i);
    if (op_init->opcode() != op_root->opcode()) {
      VLOG(10) << "Mismatching opcode, op_init: " << op_init->ToString()
               << " op_root: " << op_root->ToString();
      return std::make_pair(0, 0);
    }
  }

  VLOG(10) << "opnd_start: " << opnd_start << " opnd_end: " << opnd_end;
  return std::make_pair(opnd_start, opnd_end);
}

absl::Status ReplaceOpInSequence(HloInstruction* old_op, HloInstruction* new_op,
                                 HloInstructionSequence& instruction_sequence) {
  VLOG(10) << "old_op: " << old_op->ToString();
  VLOG(10) << "new_op: " << new_op->ToString();
  instruction_sequence.replace_instruction(old_op, new_op);
  TF_RETURN_IF_ERROR(old_op->DropAllControlDeps());
  TF_RETURN_IF_ERROR(old_op->parent()->RemoveInstruction(old_op));
  return absl::OkStatus();
}

absl::Status ReplaceUsesAndUpdateSequence(
    HloInstruction* old_op, HloInstruction* new_op,
    HloInstructionSequence& instruction_sequence, bool diff_shape = false) {
  VLOG(10) << "old_op: " << old_op->ToString();
  VLOG(10) << "new_op: " << new_op->ToString();
  if (diff_shape) {
    TF_RETURN_IF_ERROR(old_op->ReplaceAllUsesWithDifferentShape(new_op));
  } else {
    TF_RETURN_IF_ERROR(old_op->ReplaceAllUsesWith(new_op));
  }
  return ReplaceOpInSequence(old_op, new_op, instruction_sequence);
}

absl::Status ReplaceUsesAndUpdateSequence(
    const InstructionVector& old_ops, const InstructionVector& new_ops,
    HloInstructionSequence& instruction_sequence) {
  CHECK(old_ops.size() == new_ops.size());
  for (int64_t i = 0; i < old_ops.size(); ++i) {
    TF_RETURN_IF_ERROR(ReplaceUsesAndUpdateSequence(old_ops[i], new_ops[i],
                                                    instruction_sequence));
  }
  return absl::OkStatus();
}

absl::Status RemoveOpsAndUpdateSequence(
    const InstructionVector& ops,
    HloInstructionSequence& instruction_sequence) {
  for (auto op : ops) {
    VLOG(10) << "op: " << op->ToString();
    TF_RETURN_IF_ERROR(op->DropAllControlDeps());
    TF_RETURN_IF_ERROR(op->parent()->RemoveInstruction(op));
    instruction_sequence.remove_instruction(op);
  }
  return absl::OkStatus();
}

bool InsertBeforeFirstCollectiveOp(
    const InstructionVector& ops,
    const CollectiveInComputation& collective_in_computation,
    HloInstructionSequence& instruction_sequence, int64_t& idx,
    int64_t& idx_tot) {
  bool inserted = false;
  while (idx < idx_tot) {
    HloInstruction* hlo = instruction_sequence.instructions()[idx];
    if (MayInvokeCollectiveOp(hlo, collective_in_computation)) {
      for (auto op : ops) {
        instruction_sequence.insert_instruction(op, idx);
        idx++;
        idx_tot++;
      }
      inserted = true;
      break;
    }
    idx++;
  }
  return inserted;
}

void CopyInstructionInfo(const HloInstruction* old_op, HloInstruction* new_op) {
  new_op->set_metadata(old_op->metadata());
  new_op->add_frontend_attributes(old_op->frontend_attributes());
  new_op->CopyBackendConfigFrom(old_op);
}

absl::Status RewritePipelinedP2PWhileBody(
    const CollectiveInComputation& collective_in_computation,
    const std::vector<Shape>& new_parameter_shapes, HloInstruction* while_op,
    int64_t opnd_start, int64_t opnd_end) {
  HloComputation* computation = while_op->while_body();
  HloInstruction* while_init = while_op->while_init();
  HloInstruction* root = computation->root_instruction();
  HloInstructionSequence& instruction_sequence =
      computation->parent()->schedule().GetOrCreateSequence(computation);

  HloInstruction* param = computation->parameter_instruction(0);
  *param->mutable_shape() = ShapeUtil::MakeTupleShape(new_parameter_shapes);

  InstructionVector recv_dones;
  InstructionVector new_recv_dones;
  InstructionVector new_send_dones;
  for (int64_t i = opnd_start; i < opnd_end; ++i) {
    const HloInstruction* op = root->operand(i);
    if (op->opcode() == HloOpcode::kRecvDone) {
      HloInstruction* gte = FindUniqueGTEUserWithIndex(param, i);
      CHECK(gte != nullptr);
      recv_dones.push_back(gte);

      // Create the new RecvDone using the new while-body parameter.
      HloInstruction* recv = computation->AddInstruction(
          HloInstruction::CreateGetTupleElement(param, i));
      HloInstruction* recv_done = computation->AddInstruction(
          HloInstruction::CreateRecvDone(recv, op->channel_id().value()));
      new_recv_dones.push_back(recv_done);
      CopyInstructionInfo(op, recv_done);
      continue;
    }
    CHECK(op->opcode() == HloOpcode::kSendDone);
    //  Create the new SendDone using the new while-op result.
    HloInstruction* send = computation->AddInstruction(
        HloInstruction::CreateGetTupleElement(param, i));
    HloInstruction* send_done = computation->AddInstruction(
        HloInstruction::CreateSendDone(send, op->channel_id().value()));
    new_send_dones.push_back(send_done);
    CopyInstructionInfo(op, send_done);
  }
  TF_RETURN_IF_ERROR(ReplaceUsesAndUpdateSequence(recv_dones, new_recv_dones,
                                                  instruction_sequence));

  // Create a new root tuple.
  InstructionVector done_ops;
  InstructionVector new_opnds;
  for (int64_t i = 0; i < while_init->operand_count(); ++i) {
    HloInstruction* op = root->mutable_operand(i);
    if (i >= opnd_start && i < opnd_end) {
      new_opnds.push_back(op->mutable_operand(0));
      done_ops.push_back(op);
    } else {
      new_opnds.push_back(op);
    }
  }
  HloInstruction* new_root =
      computation->AddInstruction(HloInstruction::CreateTuple(new_opnds));
  computation->set_root_instruction(new_root, /*accept_different_shape=*/true);
  TF_RETURN_IF_ERROR(computation->RemoveInstruction(root));
  instruction_sequence.replace_instruction(root, new_root);

  TF_RETURN_IF_ERROR(
      RemoveOpsAndUpdateSequence(done_ops, instruction_sequence));

  // Find a place to put the new SendDone. It will be either the first
  // may-invoke-collective ops that is not in the pipelined Send/Recv chain or
  // the first op in the pipelined Send/Recv chain.
  int64_t idx = 0;
  int64_t idx_end = instruction_sequence.size();
  bool inserted =
      InsertBeforeFirstCollectiveOp(new_send_dones, collective_in_computation,
                                    instruction_sequence, idx, idx_end);
  CHECK(inserted);  // There are Send/Recv in the while-body, expect inserted.
  CHECK(idx_end == instruction_sequence.size());

  TF_RETURN_IF_ERROR(computation->parent()->schedule().Update());
  return absl::OkStatus();
}

void RewritePipelinedP2PWhileCond(
    const std::vector<Shape>& new_parameter_shapes, HloInstruction* while_op) {
  HloComputation* computation = while_op->while_condition();
  HloInstruction* param = computation->parameter_instruction(0);
  *param->mutable_shape() = ShapeUtil::MakeTupleShape(new_parameter_shapes);
  VLOG(10) << computation->ToString();
}

}  // namespace

absl::StatusOr<bool> PipelinedP2PRewriter::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  if (!module->has_schedule()) return changed;
  CollectiveInComputation collective_in_computation;
  std::vector<HloComputation*> all_computations =
      module->MakeComputationPostOrder(execution_threads);
  // Visit the computations in the order of callees to callers, so that
  // while-body is processed before while-op.
  for (auto comp_iter = all_computations.begin();
       comp_iter != all_computations.end(); ++comp_iter) {
    HloComputation* computation = *comp_iter;
    if (computation->IsFusionComputation()) {
      collective_in_computation[computation] = false;
      continue;
    }
    VLOG(10) << "Process compuation " << computation->name();
    HloInstructionSequence& instruction_sequence =
        module->schedule().GetOrCreateSequence(computation);
    int64_t idx = 0;
    int64_t idx_end = instruction_sequence.size();
    while (idx < idx_end) {
      HloInstruction* hlo = instruction_sequence.instructions()[idx];

      if (MayInvokeCollectiveOp(hlo, collective_in_computation)) {
        collective_in_computation[computation] = true;
      }

      if (hlo->opcode() != HloOpcode::kWhile) {
        idx++;
        continue;
      }

      int64_t opnd_start;
      int64_t opnd_end;
      std::tie(opnd_start, opnd_end) = FindPipelinedP2P(hlo);
      if (opnd_end == 0) {
        idx++;
        continue;
      }

      // Rewrite the while-op with a recognized pipelined SendDone/RecvDone
      // pattern to pipeline Send/Recv instead.
      VLOG(10) << "Transform pipelined while-op " << hlo->ToString();
      HloInstruction* while_init = hlo->while_init();
      InstructionVector new_while_init_opnds;
      std::vector<Shape> new_parameter_shapes;
      for (int64_t i = 0; i < while_init->operand_count(); ++i) {
        HloInstruction* op = while_init->mutable_operand(i);
        if (i >= opnd_start && i < opnd_end) {
          // Get Send/Recv from SendDone/RecvDone.
          new_while_init_opnds.push_back(op->mutable_operand(0));
        } else {
          new_while_init_opnds.push_back(op);
        }
        new_parameter_shapes.push_back(new_while_init_opnds.back()->shape());
      }

      RewritePipelinedP2PWhileCond(new_parameter_shapes, hlo);
      TF_RETURN_IF_ERROR(RewritePipelinedP2PWhileBody(collective_in_computation,
                                                      new_parameter_shapes, hlo,
                                                      opnd_start, opnd_end));
      HloInstruction* new_while_init = computation->AddInstruction(
          HloInstruction::CreateTuple(new_while_init_opnds), "while-init");
      VLOG(10) << "new_while_init: " << new_while_init->ToString();
      HloInstruction* new_while_op = computation->AddInstruction(
          HloInstruction::CreateWhile(
              hlo->while_body()->root_instruction()->shape(),
              hlo->while_condition(), hlo->while_body(), new_while_init),
          "while-result");
      CopyInstructionInfo(hlo, new_while_op);
      VLOG(10) << "new_while_op: " << new_while_op->ToString();

      InstructionVector recv_dones;
      InstructionVector new_recv_dones;
      InstructionVector new_send_dones;
      InstructionVector done_ops;
      for (int64_t i = opnd_start; i < opnd_end; ++i) {
        HloInstruction* op = while_init->mutable_operand(i);
        done_ops.push_back(op);
        if (op->opcode() == HloOpcode::kRecvDone) {
          HloInstruction* gte = FindUniqueGTEUserWithIndex(hlo, i);
          CHECK(gte != nullptr);
          recv_dones.push_back(gte);

          // Create the new RecvDone using the new while-op result.
          HloInstruction* recv = computation->AddInstruction(
              HloInstruction::CreateGetTupleElement(new_while_op, i));
          HloInstruction* recv_done = computation->AddInstruction(
              HloInstruction::CreateRecvDone(recv, op->channel_id().value()));
          new_recv_dones.push_back(recv_done);
          CopyInstructionInfo(op, recv_done);
          continue;
        }
        CHECK(op->opcode() == HloOpcode::kSendDone);
        //  Create the new SendDone using the new while-op result.
        HloInstruction* send = computation->AddInstruction(
            HloInstruction::CreateGetTupleElement(new_while_op, i));
        HloInstruction* send_done = computation->AddInstruction(
            HloInstruction::CreateSendDone(send, op->channel_id().value()));
        new_send_dones.push_back(send_done);
        CopyInstructionInfo(op, send_done);
      }

      TF_RETURN_IF_ERROR(ReplaceUsesAndUpdateSequence(
          hlo, new_while_op, instruction_sequence, /*diff_shape*/ true));
      TF_RETURN_IF_ERROR(ReplaceOpInSequence(while_init, new_while_init,
                                             instruction_sequence));
      TF_RETURN_IF_ERROR(ReplaceUsesAndUpdateSequence(
          recv_dones, new_recv_dones, instruction_sequence));
      TF_RETURN_IF_ERROR(
          RemoveOpsAndUpdateSequence(done_ops, instruction_sequence));

      int64_t opnd_tot = opnd_end - opnd_start;
      // Verify that the numbers of ops we have removed from the sequence is
      // opnd_tot and they are before the position of the new while-op.
      CHECK(idx_end == instruction_sequence.size() + opnd_tot);
      CHECK(instruction_sequence.instructions()[idx - opnd_tot] ==
            new_while_op);

      // Update idx_end to reflect the current size of the instruction sequence.
      // Update idx to right after the new while-op.
      idx_end -= opnd_tot;
      idx = idx - opnd_tot + 1;
      bool inserted = InsertBeforeFirstCollectiveOp(
          new_send_dones, collective_in_computation, instruction_sequence, idx,
          idx_end);
      CHECK(idx_end == instruction_sequence.size());
      // If there isn't any may-invoke-collective ops after the while-op, add
      // the new SendDone ops before the last instruction in the sequence.
      if (!inserted) {
        CHECK(idx_end == idx);
        idx--;
        for (auto send_done : new_send_dones) {
          instruction_sequence.insert_instruction(send_done, idx++);
        }
      }
      changed = true;
    }
  }

  if (changed) {
    TF_RETURN_IF_ERROR(module->schedule().Update());
  }

  return changed;
}

}  // namespace gpu
}  // namespace xla
