//===- Steensgaard.cpp - Context Insensitive Data Structure Analysis ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass computes a context-insensitive data analysis graph.  It does this
// by computing the local analysis graphs for all of the functions, then merging
// them together into a single big graph without cloning.
//
//===----------------------------------------------------------------------===//

#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include <ostream>

using namespace llvm;

SteensgaardDataStructures::~SteensgaardDataStructures() { }

typedef svset<const Function*> FuncSet;

void
SteensgaardDataStructures::getAllCallees(const DSCallSite &CS,
                                         FuncSet &Callees) {
  if (CS.isDirectCall()) {
    if (!CS.getCalleeFunc()->isDeclaration())
      Callees.insert(CS.getCalleeFunc());
  } else  {
    // Get all callees.
    // Note that we don't care about incomplete/external...
    FuncSet TempCallees;
    CS.getCalleeNode()->addFullFunctionSet(TempCallees);
    // Filter out the ones that are invalid targets with respect
    // to this particular callsite.
    {
      FuncSet::iterator I = TempCallees.begin();
      while (I != TempCallees.end()) {
        if (functionIsCallable(CS.getCallSite(), *I)) {
          ++I;
        } else {
          I = TempCallees.erase(I);
        }
      }
    }
    // Insert the remaining callees (the legally callable ones)
    // into the master 'Callees' list
    Callees.insert(TempCallees.begin(), TempCallees.end());
  }
}

void
SteensgaardDataStructures::releaseMemory() {
  delete ResultGraph; 
  ResultGraph = 0;
  DataStructures::releaseMemory();
}

// print - Implement the Pass::print method...
void
SteensgaardDataStructures::print(llvm::raw_ostream &O, const Module *M) const {
  assert(ResultGraph && "Result graph has not yet been computed!");
  ResultGraph->writeGraphToFile(O, "steensgaards");
}

/// run - Build up the result graph, representing the pointer graph for the
/// program.
///
bool
SteensgaardDataStructures::runOnModule(Module &M) {
  DS = &getAnalysis<StdLibDataStructures>();
  init(&getAnalysis<TargetData>());
  return runOnModuleInternal(M);
}

bool
SteensgaardDataStructures::runOnModuleInternal(Module &M) {
  assert(ResultGraph == 0 && "Result graph already allocated!");
  
  // Get a copy for the globals graph.
  DSGraph * GG = DS->getGlobalsGraph();
  GlobalsGraph = new DSGraph(GG, GG->getGlobalECs(), *TypeSS);

  // Create a new, empty, graph...
  ResultGraph = new DSGraph(GG->getGlobalECs(), getTargetData(), *TypeSS);
  ResultGraph->setGlobalsGraph(GlobalsGraph);
  // ResultGraph->spliceFrom(DS->getGlobalsGraph());

  
  // Loop over the rest of the module, merging graphs for non-external functions
  // into this graph.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    if (!I->isDeclaration()) {
      ResultGraph->spliceFrom(DS->getDSGraph(*I));
    }
  }

  ResultGraph->removeTriviallyDeadNodes();
  ResultGraph->maskIncompleteMarkers();
  ResultGraph->markIncompleteNodes(DSGraph::MarkFormalArgs | DSGraph::IgnoreGlobals);

  // Now that we have all of the graphs inlined, we can go about eliminating
  // call nodes...
  //

  // Start with a copy of the original call sites.
  std::list<DSCallSite> & Calls = ResultGraph->getFunctionCalls();

  // It's quite possible that a callsite will have potential callees
  // added to it during merging.  Since we're not set up to handle
  // that particularly elegantly, instead simply iterate through
  // all callsites until we've done no merging.
  // Thereby ensuring we've merged all arg/param pairs for all
  // possible targets of each call.
  // FWIW even on larger codes we don't need more than 2 iterations,
  // including ones like 403.gcc and 400.perlbench
  while (buildCallGraph()) {
    for (std::list<DSCallSite>::iterator CI = Calls.begin(), E = Calls.end();
         CI != E; ++CI) {
      DSCallSite &CurCall = *CI;
      FuncSet CallTargets = CallGraph[&CurCall];

      // Loop over all callees, merging the callsite's arguments
      // with the function's parameters.
      for (FuncSet::iterator I = CallTargets.begin(), E = CallTargets.end();
           I != E; ++I) {
        // If we can eliminate this function call, do so!
        const Function *F = *I;
        if (!F->isDeclaration()) {
          ResolveFunctionCall(F, CurCall);
        }
      }
    }
  }

  // Remove our knowledge of what the return values of the functions are, except
  // for functions that are externally visible from this module (e.g. main).  We
  // keep these functions so that their arguments are marked incomplete.
  for (DSGraph::ReturnNodesTy::iterator I =
         ResultGraph->getReturnNodes().begin(),
         E = ResultGraph->getReturnNodes().end(); I != E; )
    if (I->first->hasInternalLinkage())
      ResultGraph->getReturnNodes().erase(I++);
    else
      ++I;

  // Update the "incomplete" markers on the nodes, ignoring unknownness due to
  // incoming arguments...
  ResultGraph->maskIncompleteMarkers();
  ResultGraph->markIncompleteNodes(DSGraph::MarkFormalArgs | DSGraph::IgnoreGlobals);

  // Remove any nodes that are dead after all of the merging we have done...

  ResultGraph->removeDeadNodes(DSGraph::KeepUnreachableGlobals);

  GlobalsGraph->removeTriviallyDeadNodes();
  GlobalsGraph->maskIncompleteMarkers();

  // Mark external globals incomplete.
  GlobalsGraph->markIncompleteNodes(DSGraph::IgnoreGlobals);

  formGlobalECs();

  // Clone the global nodes into this graph.
  cloneGlobalsInto(ResultGraph, DSGraph::DontCloneCallNodes |
                              DSGraph::DontCloneAuxCallNodes);

  DEBUG(print(errs(), &M));
  return false;
}

/// ResolveFunctionCall - Resolve the actual arguments of a call to function F
/// with the specified call site descriptor.  This function links the arguments
/// and the return value for the call site context-insensitively.
///
void
SteensgaardDataStructures::ResolveFunctionCall(const Function *F,
                                               const DSCallSite &Call) {
  DEBUG(errs() << *Call.getCallSite().getInstruction()
               << " calls: " << F->getName() << "\n");

  assert(ResultGraph != 0 && "Result graph not allocated!");
  DSGraph::ScalarMapTy &ValMap = ResultGraph->getScalarMap();

  // Handle the return value of the function...
  DSNodeHandle RetVal = ResultGraph->getReturnNodeFor(*F);
  if (Call.getRetVal().getNode() && RetVal.getNode())
    RetVal.mergeWith(Call.getRetVal());

  // As well as the var-args nodes...
  DSNodeHandle VAVal = ResultGraph->getVANodeFor(*F);
  if (Call.getVAVal().getNode() && VAVal.getNode())
    VAVal.mergeWith(Call.getVAVal());

  // Loop over all pointer arguments, resolving them to their provided pointers
  unsigned PtrArgIdx = 0;
  for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
       AI != AE && PtrArgIdx < Call.getNumPtrArgs(); ++AI) {
    DSGraph::ScalarMapTy::iterator I = ValMap.find(AI);
    if (I != ValMap.end())    // If its a pointer argument...
      I->second.mergeWith(Call.getPtrArg(PtrArgIdx++));
  }
}

bool
SteensgaardDataStructures::buildCallGraph() {
  std::list<DSCallSite> & Calls = ResultGraph->getFunctionCalls();
  bool changed = false;
  for (std::list<DSCallSite>::iterator CI = Calls.begin(), E = Calls.end();
       CI != E; ++CI) {
      DSCallSite &CurCall = *CI;

      // Even though we're during unification, refuse to consider
      // call edges that are /illegal/ to exist in a valid program.
      // Use getCallCallees to do this filtering for us:
      FuncSet Callees;
      getAllCallees(CurCall, Callees);

      FuncSet & OldCallees = CallGraph[&CurCall];
      changed |= !(OldCallees == Callees);
      OldCallees.swap(Callees);
  }

  return changed;
}

char SteensgaardDataStructures::ID = 0;

// Register the pass...
static RegisterPass<SteensgaardDataStructures> X
("dsa-steens",
 "Context-insensitive Data Structure Analysis");
