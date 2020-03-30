/*
 * conflict_detection.cpp
 *
 *  Created on: 25.03.2020
 *      Author: Tim Jammer
 */

#include "conflict_detection.h"
#include "function_coverage.h"
#include "implementation_specific.h"
#include "mpi_functions.h"

using namespace llvm;

// do i need to export it into header?
std::vector<CallBase *> get_corresponding_wait(CallBase *call);

// if scope ending.empty: normal send without a scope
bool check_call_for_conflict(CallBase *mpi_call,
                             std::vector<CallBase *> scope_endings) {

  // tuple Instr, scope_ended,in_Ibarrier
  std::set<std::tuple<Instruction *, bool, bool>> to_check;
  std::set<BasicBlock *>
      already_checked; // be aware, that revisiting the current block might be
                       // necessary if in a loop
  std::set<CallBase *> potential_conflicts;

  Instruction *current_inst = dyn_cast<Instruction>(mpi_call);
  assert(current_inst != nullptr);
  Instruction *next_inst = current_inst->getNextNode();

  // what this function does:
  // follow all possible code paths until
  // A a synchronization occurs
  // B a conflicting call is detected
  // C end of program (mpi finalize)
  // for A the analysis will only stop if the scope of an I... call has already
  // ended

  // errs()<< "Start analyzing Codepath\n";
  // mpi_call->dump();

  // if this call is within the ibarrier, We suspect its past the barrier for
  // our analysis, as it may conflict with past calls, preceding calls will
  // detect the conflict with this call nonetheless Theoretically it is allowed
  // to have multiple Ibarriers interleaved. (meaning calling Ibar, Ibar, Wait,
  // Wait) but our analysis will not cover multiple ibarriers, we only care
  // about the first encountered
  bool in_Ibarrier = false;
  std::vector<CallBase *> i_barrier_scope_end;

  bool scope_ended = scope_endings.empty();

  while (next_inst != nullptr) {
    current_inst = next_inst;
    // current_inst->dump();

    if (auto *call = dyn_cast<CallBase>(current_inst)) {
      if (is_mpi_call(call)) {
        // check if this call is conflicting or if search can be stopped as this
        // is a sync point
        // errs() << "need to check call to "
        //		<< call->getCalledFunction()->getName() << "\n";
        // ignore sync if scope has not ended yet
        if (scope_ended &&
            mpi_func->sync_functions.find(call->getCalledFunction()) !=
                mpi_func->sync_functions.end()) {

          if (call->getCalledFunction() == mpi_func->mpi_Ibarrier) {
            if (in_Ibarrier) {
              errs() << "Why do you use multiple interleaved Ibarrier's? I "
                        "don't see a usecase for it.\n";
            } else {

              assert(call->getNumArgOperands() == 2 &&
                     "MPI_Ibarrier has 2 args");
              if (get_communicator(mpi_call) == call->getArgOperand(0)) {
                if (!i_barrier_scope_end.empty()) {
                  errs() << "Warning: parsing too many Ibarriers\n"
                         << "Analysis result is still correct, but false "
                            "positives are more likely";

                } else {
                  in_Ibarrier = true;
                  i_barrier_scope_end = get_corresponding_wait(call);
                }
              }
              // else: could not prove same communicator: pretend barrier isnt
              // there for our analysis
            }
          } else if (call->getCalledFunction() == mpi_func->mpi_barrier) {
            assert(call->getNumArgOperands() == 1 && "MPI_Barrier has 1 args");
            if (get_communicator(mpi_call) == call->getArgOperand(0)) {

              current_inst = nullptr;
              errs() << "call to " << call->getCalledFunction()->getName()
                     << " is a sync point, no overtaking possible beyond it\n";
            }
            // else: could not prove that barrier is in the same communicator:
            // continue analysis

          } else if (call->getCalledFunction() == mpi_func->mpi_finalize) {
            // no mpi beyond this
            current_inst = nullptr;
            errs() << "call to " << call->getCalledFunction()->getName()
                   << " is a sync point, no overtaking possible beyond it\n";
          }

          // no need to analyze this path further, a sync point will stop msg
          // overtaking anyway

        } else if (mpi_func->conflicting_functions.find(
                       call->getCalledFunction()) !=
                   mpi_func->conflicting_functions.end()) {
          potential_conflicts.insert(call);
        } else if (mpi_func->unimportant_functions.find(
                       call->getCalledFunction()) !=
                   mpi_func->unimportant_functions.end()) {

          if (in_Ibarrier &&
              std::find(i_barrier_scope_end.begin(), i_barrier_scope_end.end(),
                        call) != i_barrier_scope_end.end()) {

            assert(scope_ended);
            // no mpi beyond this
            current_inst = nullptr;
            errs() << "Completed Ibarrier, no overtaking possible beyond it\n";
          }

          if (!scope_ended &&
              std::find(scope_endings.begin(), scope_endings.end(), call) !=
                  scope_endings.end()) {
            // found end of scope
            scope_ended = true;
          }

          // errs() << "call to " << call->getCalledFunction()->getName()
          //       << " is currently not supported in this analysis\n";
        }

      } else { // no mpi function

        if (function_metadata->may_conflict(call->getCalledFunction())) {
          errs() << "Call To " << call->getCalledFunction()->getName()
                 << "May conflict\n";
          // conflict found
          return true;
        } else if (function_metadata->will_sync(call->getCalledFunction())) {
          // sync point detected
          current_inst = nullptr;
          errs() << "call to " << call->getCalledFunction()->getName()
                 << " will sync, no overtaking possible beyond it\n";
        } else if (function_metadata->is_unknown(call->getCalledFunction())) {
          errs() << "Could not determine if call to "
                 << call->getCalledFunction()->getName()
                 << "will result in a conflict, for safety we will assume it "
                    "does \n";
          // assume conflict
          return true;
        }

        // for sanity:
        assert(function_metadata->is_unknown(call->getCalledFunction()) ==
               false);
      }
    } // end if CallBase

    // now fetch the next inst
    next_inst = nullptr;

    if (current_inst !=
        nullptr) // if not discovered to stop analyzing this code path
    {
      if (current_inst->isTerminator()) {
        for (unsigned int i = 0; i < current_inst->getNumSuccessors(); ++i) {
          auto *next_block = current_inst->getSuccessor(i);

          if (already_checked.find(next_block) == already_checked.end()) {
            to_check.insert(std::make_tuple(next_block->getFirstNonPHI(),
                                            scope_ended, in_Ibarrier));
          }
        }
        if (isa<ReturnInst>(current_inst)) {
          // we have to check all exit points of this function for conflicts as
          // well...
          Function *f = current_inst->getFunction();
          for (auto *user : f->users()) {
            if (auto *where_returns = dyn_cast<CallBase>(user)) {
              if (where_returns->getCalledFunction() == f) {
                assert(where_returns->getNextNode() != nullptr);
                to_check.insert(std::make_tuple(where_returns->getNextNode(),
                                                scope_ended, in_Ibarrier));
              }
            }
          }
        }
      }

      next_inst = current_inst->getNextNode();
    }

    if (next_inst == nullptr) {
      // errs() << to_check.size();
      if (!to_check.empty()) {
        auto it_pos = to_check.begin();

        std::tuple<Instruction *, bool, bool> tup = *it_pos;
        next_inst = std::get<0>(tup);
        scope_ended = std::get<1>(tup);
        in_Ibarrier = std::get<2>(tup);
        to_check.erase(it_pos);
        already_checked.insert(
            next_inst->getParent()); // will be checked now, so no revisiting
                                     // necessary
      }
    }
  } // end while

  // check for conflicts:
  for (auto *call : potential_conflicts) {
    bool conflict = are_calls_conflicting(mpi_call, call);
    if (conflict) {
      // found at least one conflict, currently we can stop then
      return true;
    }
  }

  // no conflict found
  return false;
}

bool check_conflicts(llvm::Module &M, llvm::Function *f) {
  if (f == nullptr) {
    // no messages: no conflict
    return false;
  }

  for (auto user : f->users()) {
    if (CallBase *call = dyn_cast<CallBase>(user)) {
      if (call->getCalledFunction() == f) {
        if (check_call_for_conflict(call, std::vector<CallBase *>())) {
          // found conflict
          return true;
        }

      } else {
        call->dump();
        errs() << "\nWhy do you do that?\n";
      }
    }
  }

  // no conflicts found
  return false;
}

bool check_mpi_send_conflicts(Module &M) {
  return check_conflicts(M, mpi_func->mpi_send);
}

bool check_mpi_recv_conflicts(Module &M) {
  return check_conflicts(M, mpi_func->mpi_recv);
}

bool check_mpi_Irecv_conflicts(Module &M) {

  Function *f = mpi_func->mpi_Irecv;
  if (f == nullptr) {
    // no messages: no conflict
    return false;
  }

  for (auto user : f->users()) {
    if (CallBase *call = dyn_cast<CallBase>(user)) {
      if (call->getCalledFunction() == f) {
        auto scope_endings = get_corresponding_wait(call);
        if (check_call_for_conflict(call, scope_endings)) {
          // found conflict
          return true;
        }

      } else {
        call->dump();
        errs() << "\nWhy do you do that?\n";
      }
    }
  }

  // no conflicts found
  return false;
}

bool check_mpi_Isend_conflicts(Module &M) {

  if (mpi_func->mpi_Ibsend != nullptr || mpi_func->mpi_Issend != nullptr ||
      mpi_func->mpi_Irsend != nullptr) {
    errs() << "This analysis does not cover the usage of any of Ib Ir or "
              "Issend operations. Replace them with another send mode like "
              "Isend instead\n";
    return false;
  }

  Function *f = mpi_func->mpi_Isend;
  if (f == nullptr) {
    // no messages: no conflict
    return false;
  }

  for (auto user : f->users()) {
    if (CallBase *call = dyn_cast<CallBase>(user)) {
      if (call->getCalledFunction() == f) {
        auto scope_endings = get_corresponding_wait(call);
        if (check_call_for_conflict(call, scope_endings)) {
          // found conflict
          return true;
        }

      } else {
        call->dump();
        errs() << "\nWhy do you do that?\n";
      }
    }
  }

  // no conflicts found
  return false;
}

bool check_mpi_Bsend_conflicts(Module &M) {

  Function *f = mpi_func->mpi_Bsend;
  if (f == nullptr) {
    // no messages: no conflict
    return false;
  }

  std::vector<CallBase *> scope_endings;
  for (auto *user : mpi_func->mpi_buffer_detach->users()) {
    if (auto *buffer_detach_call = dyn_cast<CallBase>(user)) {
      assert(buffer_detach_call->getCalledFunction() ==
             mpi_func->mpi_buffer_detach);
      scope_endings.push_back(buffer_detach_call);
    }
  }

  for (auto user : f->users()) {
    if (CallBase *call = dyn_cast<CallBase>(user)) {
      if (call->getCalledFunction() == f) {
        if (check_call_for_conflict(call, scope_endings)) {
          // found conflict
          return true;
        }

      } else {
        call->dump();
        errs() << "\nWhy do you do that?\n";
      }
    }
  }

  // no conflicts found
  return false;
}

bool check_mpi_Ssend_conflicts(Module &M) {
  // return check_conflicts(M,mpi_func->mpi_Ssend);
  // Ssend may not yield to conflicts regarding overtaking messages:
  // when Ssend returns: the receiver have begun execution of the matching recv
  // therefore further send operation may not overtake this one

  // this ssend can overtake another send ofc but this conflict will be handled
  // when the overtakend send is analyzed
  return false;
}

bool check_mpi_Rsend_conflicts(Module &M) {
  // return check_conflicts(M, mpi_func->mpi_Rsend);

  // standard:
  // A send operation that uses the ready mode has the same semantics as a
  // standard send operation, or a synchronous send operation; it is merely that
  // the sender provides additional information to the system (namely that a
  // matching receive is already posted), that can save some overhead. In a
  // correct program, therefore, a ready send couldbe replaced by a standard
  // send with no effect on the behavior of the program other than performance.

  // This means, the sender have started to execute the matching send, therefore
  // it has the same as Ssend.

  return false;
}

bool are_calls_conflicting(CallBase *orig_call, CallBase *conflict_call) {

  errs() << "\n";
  orig_call->dump();
  errs() << "potential conflict detected: ";
  conflict_call->dump();
  errs() << "\n";

  // if one is send and the other a recv: fond a match which means no conflict
  if ((is_send_function(orig_call->getCalledFunction()) &&
       is_recv_function(conflict_call->getCalledFunction())) ||
      (is_recv_function(orig_call->getCalledFunction()) &&
       is_send_function(conflict_call->getCalledFunction()))) {
    return false;
  }

  if (orig_call == conflict_call) {
    errs() << "Send is conflicting with itself, probably in a loop, if using "
              "different msg tags on each iteration this is safe nonetheless\n";
    return true;
  }

  // check communicator
  auto *comm1 = get_communicator(orig_call);
  auto *comm2 = get_communicator(conflict_call);

  if (auto *c1 = dyn_cast<Constant>(comm1)) {
    if (auto *c2 = dyn_cast<Constant>(comm2)) {
      if (c1 != c2) {
        // different communicators
        return false;
      }
    }
  } // otherwise, we have not proven that the communicator is be different
    // TODO: (very advanced) if e.g. mpi comm split is used, we might be able to
    // statically prove different communicators

  // check src
  auto *src1 = get_src(orig_call);
  auto *src2 = get_src(conflict_call);
  if (auto *s1 = dyn_cast<Constant>(src1)) {
    if (auto *s2 = dyn_cast<Constant>(src2)) {
      if (s1 != s2) {
        // different sources
        return false;
      }
    }
  } // otherwise, we have not proven that the src might be different

  // check tag
  auto *tag1 = get_tag(orig_call);
  auto *tag2 = get_tag(conflict_call);
  if (auto *t1 = dyn_cast<Constant>(tag1)) {
    if (auto *t2 = dyn_cast<Constant>(tag2)) {
      if (t1 != t2) {
        // different tags
        return false;
      }
    }
  } // otherwise, we have not proven that the tag is be different

  // cannot disprove conflict, have to assume it indeed relays on msg ordering
  return true;
}

std::vector<CallBase *> get_corresponding_wait(CallBase *call) {

  // errs() << "Analyzing scope of \n";
  // call->dump();

  std::vector<CallBase *> result;
  unsigned int req_arg_pos = 6;
  if (call->getCalledFunction() == mpi_func->mpi_Ibarrier) {
    assert(call->getNumArgOperands() == 2);
    req_arg_pos = 1;
  } else {
    assert(call->getNumArgOperands() == 7);
    assert(call->getCalledFunction() == mpi_func->mpi_Isend ||
           call->getCalledFunction() == mpi_func->mpi_Ibsend ||
           call->getCalledFunction() == mpi_func->mpi_Issend ||
           call->getCalledFunction() == mpi_func->mpi_Irsend ||
           call->getCalledFunction() == mpi_func->mpi_Irecv);
  }

  Value *req = call->getArgOperand(req_arg_pos);

  // req->dump();
  if (auto *alloc = dyn_cast<AllocaInst>(req)) {
    for (auto *user : alloc->users()) {
      if (auto *other_call = dyn_cast<CallBase>(user)) {
        if (other_call->getCalledFunction() == mpi_func->mpi_wait) {
          assert(other_call->getNumArgOperands() == 2);
          assert(other_call->getArgOperand(0) == req &&
                 "First arg of MPi wait is MPI_Request");
          // found end of scope
          // errs() << "possible ending of scope here \n";
          // other_call->dump();
          result.push_back(other_call);
        }
      }
    }
    // TODO scope detection in basic waitall
    // ofc at some point of pointer arithmetic, we cannot follow it

  } else {
    errs() << "could not determine scope of \n";
    call->dump();
    errs() << "Assuming it will finish at mpi_finalize.\n"
           << "The Analysis result is still valid, although the chance of "
              "false positives is higher\n";

    for (auto *user : mpi_func->mpi_finalize->users()) {
      if (auto *finalize_call = dyn_cast<CallBase>(user)) {
        assert(finalize_call->getCalledFunction() == mpi_func->mpi_finalize);
        result.push_back(finalize_call);
      }
    }
  }

  return result;
}

Value *get_communicator(CallBase *mpi_call) {

  unsigned int total_num_args = 0;
  unsigned int communicator_arg_pos = 0;

  if (mpi_call->getCalledFunction() == mpi_func->mpi_send ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Bsend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Ssend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Rsend) {
    total_num_args = 6;
    communicator_arg_pos = 5;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_Isend) {
    total_num_args = 7;
    communicator_arg_pos = 5;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_recv ||
             mpi_call->getCalledFunction() == mpi_func->mpi_Irecv) {
    total_num_args = 7;
    communicator_arg_pos = 5;
  } else {
    errs() << mpi_call->getCalledFunction()->getName()
           << ": This MPI function is currently not supported\n";
    assert(false);
  }

  assert(mpi_call->getNumArgOperands() == total_num_args);

  return mpi_call->getArgOperand(communicator_arg_pos);
}

Value *get_src(CallBase *mpi_call) {

  unsigned int total_num_args = 0;
  unsigned int src_arg_pos = 0;

  if (mpi_call->getCalledFunction() == mpi_func->mpi_send ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Bsend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Ssend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Rsend) {
    total_num_args = 6;
    src_arg_pos = 3;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_Isend) {
    total_num_args = 7;
    src_arg_pos = 3;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_recv ||
             mpi_call->getCalledFunction() == mpi_func->mpi_Irecv) {
    total_num_args = 7;
    src_arg_pos = 3;

  } else {
    errs() << mpi_call->getCalledFunction()->getName()
           << ": This MPI function is currently not supported\n";
    assert(false);
  }

  assert(mpi_call->getNumArgOperands() == total_num_args);

  return mpi_call->getArgOperand(src_arg_pos);
}

Value *get_tag(CallBase *mpi_call) {

  unsigned int total_num_args = 0;
  unsigned int tag_arg_pos = 0;

  if (mpi_call->getCalledFunction() == mpi_func->mpi_send ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Bsend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Ssend ||
      mpi_call->getCalledFunction() == mpi_func->mpi_Rsend) {
    total_num_args = 6;
    tag_arg_pos = 4;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_Isend) {
    total_num_args = 7;
    tag_arg_pos = 4;
  } else if (mpi_call->getCalledFunction() == mpi_func->mpi_recv ||
             mpi_call->getCalledFunction() == mpi_func->mpi_Irecv) {
    total_num_args = 7;
    tag_arg_pos = 4;
  } else {
    errs() << mpi_call->getCalledFunction()->getName()
           << ": This MPI function is currently not supported\n";
    assert(false);
  }

  assert(mpi_call->getNumArgOperands() == total_num_args);

  return mpi_call->getArgOperand(tag_arg_pos);
}
