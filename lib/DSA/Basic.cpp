//===- Basic.cpp ----------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the basic data structure analysis pass. It simply assumes
// that all pointers can points to all possible locations.
//
//===----------------------------------------------------------------------===//

#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"

#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/TypeBuilder.h"

using namespace llvm;

static RegisterPass<BasicDataStructures>
X("dsa-basic", "Basic Data Structure Analysis(No Analysis)");

char BasicDataStructures::ID = 0;

bool BasicDataStructures::runOnModule(Module &M) {
  init(&getAnalysis<TargetData>());

  //
  // Create a void pointer type.  This is simply a pointer to an 8 bit value.
  //

  DSNode * GVNodeInternal = new DSNode(GlobalsGraph);
  DSNode * GVNodeExternal = new DSNode(GlobalsGraph);
  for (Module::global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I) {
    if (I->isDeclaration() || (!(I->hasInternalLinkage()))) {
      GlobalsGraph->getNodeForValue(&*I).mergeWith(GVNodeExternal);
    } else {
      GlobalsGraph->getNodeForValue(&*I).mergeWith(GVNodeInternal);
    }
  }

  GVNodeInternal->foldNodeCompletely();
  GVNodeInternal->maskNodeTypes(DSNode::IncompleteNode);

  GVNodeExternal->foldNodeCompletely();
  GVNodeExternal->setExternalMarker();

  // Next step, iterate through the nodes in the globals graph, unioning
  // together the globals into equivalence classes.
  formGlobalECs();

  for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F) {
    if (!F->isDeclaration()) {
      DSGraph* G = new DSGraph(GlobalECs, getTargetData(), *TypeSS, GlobalsGraph);
      DSNode * Node = new DSNode(G);
          
      if (!F->hasInternalLinkage())
        Node->setExternalMarker();

      // Create scalar nodes for all pointer arguments...
      for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
          I != E; ++I) {
        if (isa<PointerType>(I->getType())) {
          G->getNodeForValue(&*I).mergeWith(Node);
        }
      }

      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        G->getNodeForValue(&*I).mergeWith(Node);
      }

      Node->foldNodeCompletely();
      Node->maskNodeTypes(DSNode::IncompleteNode);

      setDSGraph(*F, G);
    }
  }
 
  return false;
}
