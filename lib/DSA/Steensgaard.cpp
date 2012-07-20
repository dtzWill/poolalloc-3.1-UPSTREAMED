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
  if (handleTest(O, M)) return;
  assert(ResultGraph && "Result graph has not yet been computed!");
  ResultGraph->writeGraphToFile(O, "steensgaards");
}

/// run - Build up the result graph, representing the pointer graph for the
/// program.
///
bool
SteensgaardDataStructures::runOnModule(Module &M) {
  DataStructures* DS = &getAnalysis<StdLibDataStructures>();
  init(DS, true, true, false, false);
  return runOnModuleInternal(M);
}

bool
SteensgaardDataStructures::runOnModuleInternal(Module &M) {
  assert(ResultGraph == 0 && "Result graph already allocated!");
  assert(GlobalsGraph);

  // Create a new, empty, graph...
  ResultGraph = new DSGraph(GlobalECs, getTargetData(), *TypeSS, GlobalsGraph);
  ResultGraph->setUseAuxCalls();

  // Loop over the rest of the module, merging graphs for non-external functions
  // into this graph.
  //
  DataStructures* DS = &getAnalysis<StdLibDataStructures>();
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    if (!I->isDeclaration()) {
      ResultGraph->spliceFrom(DS->getDSGraph(*I));
    }
  }

  // Remove trivially dead nodes resulting from merging
  ResultGraph->removeTriviallyDeadNodes();

  // Recompute incomplete,
  ResultGraph->maskIncompleteMarkers();
  ResultGraph->markIncompleteNodes(DSGraph::MarkFormalArgs | DSGraph::IgnoreGlobals);


  //
  // Go through call callsites and merge in all possible callees.
  // If afterwards we find new caller/callee pairings
  // due to the merging we just did, then repeat the process.
  //
  // Ideally we'd recognize when we were merging function pointers
  // and merge the caller/callee mappings at that point
  // (how it's supposed to be done in Steens)
  // but we're not set up to do that elegantly so we do this instead.
  //
  // On codes like 403.gcc, 400.perlbench, etc, we only require 2 iterations,
  // so this seems like a reasonable solution.
  std::list<DSCallSite> & Calls = ResultGraph->getFunctionCalls();
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
        DEBUG(errs() << *CurCall.getCallSite().getInstruction() << " calls: "
                     << F->getName() << "\n");
        if (!F->isDeclaration()) {
          // Let DSGraph's mergeInGraph figure out argument bindings
          // for us, and merge the DSNodes appropriately.
          ResultGraph->mergeInGraph(CurCall, *F, *ResultGraph,
                                    DSGraph::DontCloneCallNodes);
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

  // Recompute incomplete, dropping things that were incomplete due
  // to arguments of internal-linkage functions.
  ResultGraph->maskIncompleteMarkers();
  ResultGraph->markIncompleteNodes(DSGraph::MarkFormalArgs |
                                   DSGraph::IgnoreGlobals);

  // Figure out what calls we can consider fully-resolved then,
  // and update incomplete accordingly.
  // We could have callsites in aux afterwards that are complete,
  // ignore them--one iteration hack is enough for me.
  {
    // Remove all complete Aux calls
    std::list<DSCallSite> & Calls = ResultGraph->getAuxFunctionCalls();
    for (std::list<DSCallSite>::iterator CI = Calls.begin(), E = Calls.end();
         CI != E; ) {
      if (CI->isDirectCall() || CI->getCalleeNode()->isCompleteNode())
        CI = Calls.erase(CI);
      else
        ++CI;
    }

    // Recompute incomplete, this time not marking things
    // passed to resolved callsites as incomplete.
    ResultGraph->maskIncompleteMarkers();
    ResultGraph->markIncompleteNodes(DSGraph::MarkFormalArgs |
                                     DSGraph::IgnoreGlobals);
  }

  // Propagate External and Int2Ptr flags
  ResultGraph->computeExternalFlags(DSGraph::DontResetExternal |
                                    DSGraph::DontMarkFormalsExternal |
                                    DSGraph::IgnoreCallSites);
  ResultGraph->computeIntPtrFlags();

  // Update globals graph
  // This is strange, because GlobalsGraph doesn't
  // really make sense in Steens.
  // However other code expects us to have one,
  // so update it accordingly.
  cloneIntoGlobals(ResultGraph, DSGraph::DontCloneCallNodes |
                                DSGraph::DontCloneAuxCallNodes |
                                DSGraph::StripAllocaBit);
  GlobalsGraph->removeTriviallyDeadNodes();
  GlobalsGraph->maskIncompleteMarkers();
  GlobalsGraph->markIncompleteNodes(DSGraph::IgnoreGlobals);
  GlobalsGraph->computeExternalFlags(DSGraph::DontMarkFormalsExternal);
  GlobalsGraph->computeIntPtrFlags();
  formGlobalECs();

  // After all merging and all flag calculations,
  // construct our callgraph, and put it into canonical form:
  // (This *must* be done before removeDeadNodes!)
  ResultGraph->buildCallGraph(callgraph, GlobalFunctionList, true);
  ResultGraph->buildCompleteCallGraph(callgraph, GlobalFunctionList, true);
  callgraph.buildSCCs();
  callgraph.buildRoots();

  // Remove any nodes dead nodes
  ResultGraph->removeDeadNodes(DSGraph::KeepUnreachableGlobals);

  // Clear out our callgraph
  CallGraph.clear();

  DEBUG(print(errs(), &M));
  return false;
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
