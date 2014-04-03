#define DEBUG_TYPE "boguscf"
#include <algorithm>
#include <vector>
#include "llvm/Pass.h"
#include "llvm/IR/Value.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/User.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CFG.h"
using namespace llvm;

/*
        TODO:
                - Probabilistic insertion
                - More than two bogus flow based
                - External opaque predicate or generation or more extensive
   generation
*/

static cl::opt<std::string>
bcfFunc("bcfFunc", cl::init(""),
        cl::desc("Insert Bogus Control Flow only for some functions: "
                 "bcfFunc=\"func1,func2\""));

namespace {
struct BogusCF : public FunctionPass {
  static char ID;
  BogusCF() : FunctionPass(ID) {}

  virtual bool runOnFunction(Function &F) {
    bool hasBeenModified = false;
    // If the function is declared elsewhere in other translation unit
    // we should not modify it here
    if (F.isDeclaration()) {
      return false;
    }
    DEBUG_WITH_TYPE("opt", errs() << "bcf: Function '" << F.getName() << "'\n");

    if ((bcfFunc.size() != 0 &&
         bcfFunc.find(F.getName()) == std::string::npos)) {
      DEBUG_WITH_TYPE("opt",
                      errs() << "\tFunction not requested -- skipping\n");
      return false;
    }

    // Use a vector to store the list of blocks for probabilistic
    // splitting into two bogus control flow for a later time
    std::vector<BasicBlock *> blocks;
    blocks.reserve(F.size());

    DEBUG_WITH_TYPE("opt", errs() << "\t" << F.size()
                                  << " basic blocks found\n");
    for (Function::iterator B = F.begin(), BEnd = F.end(); B != BEnd; ++B) {
      blocks.push_back((BasicBlock *)B);
    }
    Twine blockPrefix = "block_";
    // std::random_shuffle(blocks.begin(), blocks.end());
    unsigned i = 0;
    DEBUG_WITH_TYPE("opt", for (BasicBlock * block
                                : blocks) {
      if (!block->hasName()) {
        block->setName(blockPrefix + Twine(i++));
      }
    });
    DEBUG_WITH_TYPE("cfg", F.viewCFG());

    for (BasicBlock *block : blocks) {
      DEBUG_WITH_TYPE("opt", errs() << "\tBlock " << block->getName() << "\n");
      DEBUG_WITH_TYPE("opt", errs() << "\t\tSplitting Basic Block\n");
      BasicBlock::iterator inst1 = block->begin();
      if (block->getFirstNonPHIOrDbgOrLifetime()) {
        inst1 = block->getFirstNonPHIOrDbgOrLifetime();
      }

      // We do not want to split a basic block that is only involved with some
      // terminator instruction
      if (isa<TerminatorInst>(inst1))
        continue;

      auto terminator = block->getTerminator();

      if (!isa<ReturnInst>(terminator) && terminator->getNumSuccessors() > 1) {
        DEBUG_WITH_TYPE("opt", errs() << "\t\tSkipping: >1 successor\n");
        continue;
      }

      // 1 Successor or return block
      BasicBlock *successor = nullptr;
      if (!isa<ReturnInst>(terminator)) {
        successor = *succ_begin(block);
      }

      BasicBlock *originalBlock = block->splitBasicBlock(inst1);
      DEBUG_WITH_TYPE("opt",
                      originalBlock->setName(block->getName() + "_original"));
      DEBUG_WITH_TYPE("opt", errs() << "\t\tCloning Basic Block\n");
      Twine prefix = "Cloned";
      ValueToValueMapTy VMap;
      BasicBlock *copyBlock = CloneBasicBlock(originalBlock, VMap, prefix, &F);
      DEBUG_WITH_TYPE("opt", copyBlock->setName(block->getName() + "_cloned"));

      // Remap operands, phi nodes, and metadata
      DEBUG_WITH_TYPE("opt", errs() << "\t\tRemapping information\n");

      for (auto &inst : *copyBlock) {
        RemapInstruction(&inst, VMap, RF_IgnoreMissingEntries);
      }

      // If this block has a successor, we need to worry about use of Values
      // generated by this block
      if (successor) {
        DEBUG_WITH_TYPE("opt", errs() << "\t\tHandling successor use\n");
        for (auto &inst : *originalBlock) {
          DEBUG_WITH_TYPE("opt", errs() << "\t\t\t" << inst << "\n");
          PHINode *phi = nullptr;
          std::vector<User *> users;
          // The instruction object itself is the Value for the result
          for (auto user = inst.use_begin(), useEnd = inst.use_end();
               user != useEnd; ++user) {
            // User is an instruction
            if (Instruction *userInst = dyn_cast<Instruction>(*user)) {
              BasicBlock *userBlock = userInst->getParent();
              // Instruction belongs to another block that is not us
              if (userBlock != copyBlock && userBlock != originalBlock) {
                DEBUG_WITH_TYPE("opt", errs() << "\t\t\t\tUsed in "
                                              << userBlock->getName() << "\n");
                // Check if inst is a phinode
                if (PHINode *phiCheck = dyn_cast<PHINode>(userInst)) {
                  DEBUG_WITH_TYPE("opt", errs() << "\t\t\t\t\tPHI Node\n");
                  phiCheck->addIncoming(VMap[&inst], copyBlock);
                  break; // done with this instruction
                } else {
                  // This is an artificially created PHINode whose value will
                  // always come from the "upper" blocks as originally
                  // Might only happen in a loop
                  // cf
                  // http://llvm.org/docs/doxygen/html/classllvm_1_1LoopInfo.html
                  DEBUG_WITH_TYPE("opt", errs() << "\t\t\t\t\tNone-PHI Node\n");
                  if (!phi) {
                    DEBUG_WITH_TYPE(
                        "opt", errs() << "\t\t\t\t\t\t\tCreating PHI Node\n");
                    // If still not, then we will create in successor
                    phi = PHINode::Create(
                        inst.getType(), 2, "",
                        successor->getFirstNonPHIOrDbgOrLifetime());
                    phi->addIncoming(&inst, originalBlock);
                    phi->addIncoming(VMap[&inst], copyBlock);
                  }
                  users.push_back(*user);
                }
              }
            }
          }
          // If we have artificially created a PHINode
          if (phi) {
            // Update users
            for (User *user : users) {
              user->replaceUsesOfWith(&inst, phi);
            }
            // Add incoming phi nodes for all the successor's predecessors
            // to point to itself
            // This is because the Value was used not in a PHINode but from
            // our own created one, then the Value must have only been produced
            // in the block that we just split. And thus not going to be changed
            for (auto pred = pred_begin(successor),
                      predEnd = pred_end(successor);
                 pred != predEnd; ++pred) {
              if (*pred != originalBlock && *pred != copyBlock) {
                phi->addIncoming(phi, *pred);
              }
            }
          }
        }

        // Now iterate through all the PHINode of successor to see if there
        // are any incoming blocks from the original block but not from the
        // clone block -- could be due to constant Value for example
        for (auto &inst : *successor) {
          if (PHINode *phi = dyn_cast<PHINode>(&inst)) {
            bool phiOriginalBlock = false, phiClonedBlock = false;
            Value *originalValue = nullptr;

            for (unsigned j = 0, jEnd = phi->getNumIncomingValues(); j != jEnd;
                 ++j) {
              if (phi->getIncomingBlock(j) == originalBlock) {
                phiOriginalBlock = true;
                originalValue = phi->getIncomingValue(j);
              } else if (phi->getIncomingBlock(j) == copyBlock) {
                phiClonedBlock = true;
              }
            }
            if (!phiClonedBlock && phiOriginalBlock && originalValue) {
              phi->addIncoming(originalValue, copyBlock);
            }
          }
        }
      }

      // Clear the unconditional branch from the "husk" original block
      block->getTerminator()->eraseFromParent();

      // Create Opaque Predicate
      // Always true for now
      Value *lhs = ConstantFP::get(Type::getFloatTy(F.getContext()), 1.0);
      Value *rhs = ConstantFP::get(Type::getFloatTy(F.getContext()), 1.0);
      FCmpInst *condition = new FCmpInst(*block, FCmpInst::FCMP_TRUE, lhs, rhs);

      // Bogus conditional branch
      BranchInst::Create(originalBlock, copyBlock, (Value *)condition, block);

      // DEBUG_WITH_TYPE("cfg", F.viewCFG());
      hasBeenModified |= true;
    }
    DEBUG_WITH_TYPE("cfg", F.viewCFG());
    return hasBeenModified;
  }

  virtual void getAnalysisUsage(AnalysisUsage &Info) const {
    Info.addRequired<DominatorTree>();
  }
};
}

char BogusCF::ID = 0;
static RegisterPass<BogusCF>
X("boguscf", "Insert bogus control flow paths into basic blocks", false, false);