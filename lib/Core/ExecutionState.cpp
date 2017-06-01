//===-- ExecutionState.cpp ------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/ExecutionState.h"

#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/Debug.h"

#include "klee/CommandLine.h"
#include "klee/Expr.h"

#include "ITree.h"
#include "Memory.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/IR/Function.h"
#else
#include "llvm/Function.h"
#endif
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <iomanip>
#include <sstream>
#include <cassert>
#include <map>
#include <set>
#include <stdarg.h>

using namespace llvm;
using namespace klee;

namespace {
cl::opt<bool> DebugLogStateMerge("debug-log-state-merge");
}

/***/

StackFrame::StackFrame(KInstIterator _caller, KFunction *_kf) :
		caller(_caller), kf(_kf), callPathNode(0), minDistToUncoveredOnReturn(
				0), varargs(0) {
	locals = new Cell[kf->numRegisters];
	for (unsigned i = 0; i < kf->numRegisters; i++) {
		locals[i].taint = 0;
	}
}

StackFrame::StackFrame(const StackFrame &s) :
		caller(s.caller), kf(s.kf), callPathNode(s.callPathNode), allocas(
				s.allocas), minDistToUncoveredOnReturn(
				s.minDistToUncoveredOnReturn), varargs(s.varargs) {
	locals = new Cell[s.kf->numRegisters];
	for (unsigned i = 0; i < s.kf->numRegisters; i++)
		locals[i] = s.locals[i];
}

StackFrame::~StackFrame() {
	delete[] locals;
}

/***/

ExecutionState::ExecutionState(KFunction *kf) :
		pc(kf->instructions), prevPC(pc), queryCost(0.), weight(1), depth(0), instsSinceCovNew(
				0), coveredNew(false), forkDisabled(false), ptreeNode(0), itreeNode(
				0), taint(0), startPCDest(0), nInstruction(0), depthCount(0) {
	pushFrame(0, kf);

	maxSpecialCount = 100;
	pathSpecial = new KInstIterator[maxSpecialCount];
	pathSpecialCount = 0;

	maxCurrentTaint = 100;
	stateTrackingTaint = new int[maxCurrentTaint];
	currentTaintCount = 0;

	executionTime = 0;
	startPCDest = 0;
	splitCount = 0;
}

#ifdef SUPPORT_Z3
ExecutionState::ExecutionState(const KInstIterator &srcPrevPC,
		const std::vector<ref<Expr> > &assumptions) :
		prevPC(srcPrevPC), constraints(assumptions), queryCost(0.), ptreeNode(
				0), itreeNode(0), nInstruction(0), depthCount(0) {

	maxSpecialCount = 100;
	pathSpecial = new KInstIterator[maxSpecialCount];
	pathSpecialCount = 0;

	maxCurrentTaint = 100;
	stateTrackingTaint = new int[maxCurrentTaint];
	currentTaintCount = 0;
	executionTime = 0;

	startPCDest = 0;
	splitCount = 0;
}
#else
ExecutionState::ExecutionState(const std::vector<ref<Expr> > &assumptions)
: constraints(assumptions), queryCost(0.), ptreeNode(0), itreeNode(0), nInstruction(0),depthCount(0) {
	maxSpecialCount = 100;
	pathSpecial = new KInstIterator[maxSpecialCount];
	pathSpecialCount = 0;

	maxCurrentTaint = 100;
	stateTrackingTaint = new int[maxCurrentTaint];
	currentTaintCount = 0;
	executionTime = 0;

	startPCDest = 0;
	splitCount = 0;
}
#endif

ExecutionState::~ExecutionState() {
	for (unsigned int i = 0; i < symbolics.size(); i++) {
		const MemoryObject *mo = symbolics[i].first;
		assert(mo->refCount > 0);
		mo->refCount--;
		if (mo->refCount == 0)
			delete mo;
	}

	while (!stack.empty())
		popFrame(0, ConstantExpr::alloc(0, Expr::Bool));

	while (!funcDestStack.empty()) {
		popFuncDest();
	}
	while (!logIns.empty()) {
		popInsLog();
	}
}

ExecutionState::ExecutionState(const ExecutionState& state) :
		fnAliases(state.fnAliases), pc(state.pc), prevPC(state.prevPC), stack(
				state.stack), incomingBBIndex(state.incomingBBIndex),

		addressSpace(state.addressSpace), constraints(state.constraints),

		queryCost(state.queryCost), weight(state.weight), depth(state.depth),

		pathOS(state.pathOS), symPathOS(state.symPathOS),

		instsSinceCovNew(state.instsSinceCovNew), coveredNew(state.coveredNew), forkDisabled(
				state.forkDisabled), coveredLines(state.coveredLines), ptreeNode(
				state.ptreeNode), itreeNode(state.itreeNode), symbolics(
				state.symbolics), arrayNames(state.arrayNames), taint(
				state.taint), startPCDest(state.startPCDest), splitCount(
				state.splitCount), nInstruction(state.nInstruction), depthCount(
				state.depthCount)

{
	for (unsigned int i = 0; i < symbolics.size(); i++)
		symbolics[i].first->refCount++;

	maxSpecialCount = 100;
	pathSpecial = new KInstIterator[maxSpecialCount];
	pathSpecialCount = 0;

	maxCurrentTaint = 100;
	stateTrackingTaint = new int[maxCurrentTaint];
	currentTaintCount = 0;

	executionTime = state.executionTime;

	for (std::vector<unsigned>::const_iterator it = state.funcDestStack.begin();
			it != state.funcDestStack.end(); ++it) {
		pushFuncDest(*it);
	}

	depthCount++;
	//pushFuncDest(state.pc->dest);

	for (std::vector<std::string>::const_iterator it = state.logIns.begin();
			it != state.logIns.end(); ++it) {
		pushInsLog(*it);
	}
}

void ExecutionState::addITreeConstraint(ref<Expr> e, llvm::Instruction *instr) {
	if (!INTERPOLATION_ENABLED)
		return;

	llvm::BranchInst *binstr = llvm::dyn_cast < llvm::BranchInst > (instr);

	if (itreeNode && binstr && binstr->isConditional()) {
		itreeNode->addConstraint(e, binstr->getCondition());
	} else if (itreeNode && !binstr) {
		itreeNode->addConstraint(e, instr->getOperand(0));
	}

}

ExecutionState *ExecutionState::branch() {
	depth++;

	ExecutionState *falseState = new ExecutionState(*this);
	falseState->coveredNew = false;
	falseState->coveredLines.clear();
	falseState->taint = this->taint;

	weight *= .5;
	falseState->weight -= weight;

	KInstIterator* pathNew = new KInstIterator[this->maxSpecialCount];
	for (int i = 0; i <= this->pathSpecialCount; i++) {
		pathNew[i] = this->pathSpecial[i];
	}
	falseState->pathSpecial = pathNew;
	falseState->maxSpecialCount = this->maxSpecialCount;
	falseState->pathSpecialCount = this->pathSpecialCount;
	int* taintNew = new int[this->maxCurrentTaint];
	for (int i = 0; i <= this->maxCurrentTaint; i++) {
		taintNew[i] = this->stateTrackingTaint[i];
	}
	falseState->stateTrackingTaint = taintNew;
	falseState->maxCurrentTaint = this->maxCurrentTaint;
	falseState->currentTaintCount = this->currentTaintCount;
	falseState->executionTime = this->executionTime;
	falseState->startPCDest = this->startPCDest;
	return falseState;
}

//current Program counter taint getter
TaintSet ExecutionState::getPCTaint() {
	return taint;
}
//current Program counter taint setter
void ExecutionState::setPCTaint(TaintSet new_taint) {
	taint = new_taint;
	KLEE_DEBUG(llvm::errs() << "PC TAINTED! " << taint << "\n");
}

//get the depth of the SESE region stack
int ExecutionState::getRegionDepth() {
	return stack.back().regionStack.size();
}

//called when entering a new SESE region 
void ExecutionState::enterRegion() {
	stack.back().regionStack.push(taint);
	KLEE_DEBUG(llvm::errs() << "REGION CHANGED! PUSH!" << taint << "\n");
}

//called when leaving a SESE region 
void ExecutionState::leaveRegion() {
	taint = stack.back().regionStack.top();
	stack.back().regionStack.pop();

}

void ExecutionState::pushFuncDest(unsigned value) {
	funcDestStack.push_back(value);
}

void ExecutionState::popFuncDest() {
	if (funcDestStack.size() > 0)
		funcDestStack.pop_back();
}

void ExecutionState::pushInsLog(std::string ins) {
	logIns.push_back(ins);
}

void ExecutionState::popInsLog() {
	if (logIns.size() > 0)
		logIns.pop_back();
}

bool ExecutionState::logCurInstruction(unsigned maxLoop) {
	std::stringstream ss;
	for (std::vector<unsigned>::const_iterator it = funcDestStack.begin();
			it != funcDestStack.end(); ++it) {
		ss << *it;
	}
	ss << this->pc->dest;

	unsigned countLoop = 0;
	std::string curIns = ss.str();
	for (std::vector<std::string>::const_iterator it = logIns.begin();
			it != logIns.end(); ++it) {
		if (curIns == *it)
			countLoop++;
		if (countLoop == maxLoop)
			break;
	}

	if (countLoop < maxLoop) {
		pushInsLog(ss.str());
		return true;
	} else
		return false;

}

void ExecutionState::pushFrame(KInstIterator caller, KFunction *kf) {
	stack.push_back(StackFrame(caller, kf));
}

void ExecutionState::popFrame(KInstruction *ki, ref<Expr> returnValue) {
	StackFrame &sf = stack.back();
	llvm::CallInst *site = (
			sf.caller ? llvm::dyn_cast < CallInst > (sf.caller->inst) : 0);
	for (std::vector<const MemoryObject*>::iterator it = sf.allocas.begin(),
			ie = sf.allocas.end(); it != ie; ++it)
		addressSpace.unbindObject(*it);
	stack.pop_back();

	if (INTERPOLATION_ENABLED && site && ki)
		itreeNode->bindReturnValue(site, ki->inst, returnValue);
}

void ExecutionState::addSymbolic(const MemoryObject *mo, const Array *array) {
	mo->refCount++;
	symbolics.push_back(std::make_pair(mo, array));
}
///

std::string ExecutionState::getFnAlias(std::string fn) {
	std::map<std::string, std::string>::iterator it = fnAliases.find(fn);
	if (it != fnAliases.end())
		return it->second;
	else
		return "";
}

void ExecutionState::addFnAlias(std::string old_fn, std::string new_fn) {
	fnAliases[old_fn] = new_fn;
}

void ExecutionState::removeFnAlias(std::string fn) {
	fnAliases.erase(fn);
}

/**/

llvm::raw_ostream &klee::operator<<(llvm::raw_ostream &os,
		const MemoryMap &mm) {
	os << "{";
	MemoryMap::iterator it = mm.begin();
	MemoryMap::iterator ie = mm.end();
	if (it != ie) {
		os << "MO" << it->first->id << ":" << it->second;
		for (++it; it != ie; ++it)
			os << ", MO" << it->first->id << ":" << it->second;
	}
	os << "}";
	return os;
}

bool ExecutionState::merge(const ExecutionState &b) {
	if (DebugLogStateMerge)
		llvm::errs() << "-- attempting merge of A:" << this << " with B:" << &b
				<< "--\n";
	if (pc != b.pc)
		return false;

	// XXX is it even possible for these to differ? does it matter? probably
	// implies difference in object states?
	if (symbolics != b.symbolics)
		return false;

	{
		std::vector<StackFrame>::const_iterator itA = stack.begin();
		std::vector<StackFrame>::const_iterator itB = b.stack.begin();
		while (itA != stack.end() && itB != b.stack.end()) {
			// XXX vaargs?
			if (itA->caller != itB->caller || itA->kf != itB->kf)
				return false;
			++itA;
			++itB;
		}
		if (itA != stack.end() || itB != b.stack.end())
			return false;
	}

	std::set<ref<Expr> > aConstraints(constraints.begin(), constraints.end());
	std::set<ref<Expr> > bConstraints(b.constraints.begin(),
			b.constraints.end());
	std::set<ref<Expr> > commonConstraints, aSuffix, bSuffix;
	std::set_intersection(aConstraints.begin(), aConstraints.end(),
			bConstraints.begin(), bConstraints.end(),
			std::inserter(commonConstraints, commonConstraints.begin()));
	std::set_difference(aConstraints.begin(), aConstraints.end(),
			commonConstraints.begin(), commonConstraints.end(),
			std::inserter(aSuffix, aSuffix.end()));
	std::set_difference(bConstraints.begin(), bConstraints.end(),
			commonConstraints.begin(), commonConstraints.end(),
			std::inserter(bSuffix, bSuffix.end()));
	if (DebugLogStateMerge) {
		llvm::errs() << "\tconstraint prefix: [";
		for (std::set<ref<Expr> >::iterator it = commonConstraints.begin(), ie =
				commonConstraints.end(); it != ie; ++it)
			llvm::errs() << *it << ", ";
		llvm::errs() << "]\n";
		llvm::errs() << "\tA suffix: [";
		for (std::set<ref<Expr> >::iterator it = aSuffix.begin(), ie =
				aSuffix.end(); it != ie; ++it)
			llvm::errs() << *it << ", ";
		llvm::errs() << "]\n";
		llvm::errs() << "\tB suffix: [";
		for (std::set<ref<Expr> >::iterator it = bSuffix.begin(), ie =
				bSuffix.end(); it != ie; ++it)
			llvm::errs() << *it << ", ";
		llvm::errs() << "]\n";
	}

	// We cannot merge if addresses would resolve differently in the
	// states. This means:
	//
	// 1. Any objects created since the branch in either object must
	// have been free'd.
	//
	// 2. We cannot have free'd any pre-existing object in one state
	// and not the other

	if (DebugLogStateMerge) {
		llvm::errs() << "\tchecking object states\n";
		llvm::errs() << "A: " << addressSpace.objects << "\n";
		llvm::errs() << "B: " << b.addressSpace.objects << "\n";
	}

	std::set<const MemoryObject*> mutated;
	MemoryMap::iterator ai = addressSpace.objects.begin();
	MemoryMap::iterator bi = b.addressSpace.objects.begin();
	MemoryMap::iterator ae = addressSpace.objects.end();
	MemoryMap::iterator be = b.addressSpace.objects.end();
	for (; ai != ae && bi != be; ++ai, ++bi) {
		if (ai->first != bi->first) {
			if (DebugLogStateMerge) {
				if (ai->first < bi->first) {
					llvm::errs() << "\t\tB misses binding for: "
							<< ai->first->id << "\n";
				} else {
					llvm::errs() << "\t\tA misses binding for: "
							<< bi->first->id << "\n";
				}
			}
			return false;
		}
		if (ai->second != bi->second) {
			if (DebugLogStateMerge)
				llvm::errs() << "\t\tmutated: " << ai->first->id << "\n";
			mutated.insert(ai->first);
		}
	}
	if (ai != ae || bi != be) {
		if (DebugLogStateMerge)
			llvm::errs() << "\t\tmappings differ\n";
		return false;
	}

	// merge stack

	ref<Expr> inA = ConstantExpr::alloc(1, Expr::Bool);
	ref<Expr> inB = ConstantExpr::alloc(1, Expr::Bool);
	for (std::set<ref<Expr> >::iterator it = aSuffix.begin(), ie =
			aSuffix.end(); it != ie; ++it)
		inA = AndExpr::create(inA, *it);
	for (std::set<ref<Expr> >::iterator it = bSuffix.begin(), ie =
			bSuffix.end(); it != ie; ++it)
		inB = AndExpr::create(inB, *it);

	// XXX should we have a preference as to which predicate to use?
	// it seems like it can make a difference, even though logically
	// they must contradict each other and so inA => !inB

	std::vector<StackFrame>::iterator itA = stack.begin();
	std::vector<StackFrame>::const_iterator itB = b.stack.begin();
	for (; itA != stack.end(); ++itA, ++itB) {
		StackFrame &af = *itA;
		const StackFrame &bf = *itB;
		for (unsigned i = 0; i < af.kf->numRegisters; i++) {
			ref<Expr> &av = af.locals[i].value;
			const ref<Expr> &bv = bf.locals[i].value;
			if (av.isNull() || bv.isNull()) {
				// if one is null then by implication (we are at same pc)
				// we cannot reuse this local, so just ignore
			} else {
				av = SelectExpr::create(inA, av, bv);
			}
		}
	}

	for (std::set<const MemoryObject*>::iterator it = mutated.begin(), ie =
			mutated.end(); it != ie; ++it) {
		const MemoryObject *mo = *it;
		const ObjectState *os = addressSpace.findObject(mo);
		const ObjectState *otherOS = b.addressSpace.findObject(mo);
		assert(
				os && !os->readOnly
						&& "objects mutated but not writable in merging state");
		assert(otherOS);

		ObjectState *wos = addressSpace.getWriteable(mo, os);
		for (unsigned i = 0; i < mo->size; i++) {
			ref<Expr> av = wos->read8(i);
			ref<Expr> bv = otherOS->read8(i);
			wos->write(i, SelectExpr::create(inA, av, bv));
		}
	}

	constraints = ConstraintManager();
	for (std::set<ref<Expr> >::iterator it = commonConstraints.begin(), ie =
			commonConstraints.end(); it != ie; ++it)
		constraints.addConstraint(*it);
	constraints.addConstraint(OrExpr::create(inA, inB));

	return true;
}

void ExecutionState::dumpStack(llvm::raw_ostream &out) const {
	unsigned idx = 0;
	const KInstruction *target = prevPC;
	for (ExecutionState::stack_ty::const_reverse_iterator it = stack.rbegin(),
			ie = stack.rend(); it != ie; ++it) {
		const StackFrame &sf = *it;
		Function *f = sf.kf->function;
		const InstructionInfo &ii = *target->info;
		out << "\t#" << idx++;
		std::stringstream AssStream;
		AssStream << std::setw(8) << std::setfill('0') << ii.assemblyLine;
		out << AssStream.str();
		out << " in " << f->getName().str() << " (";
		// Yawn, we could go up and print varargs if we wanted to.
		unsigned index = 0;
		for (Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
				ai != ae; ++ai) {
			if (ai != f->arg_begin())
				out << ", ";

			out << ai->getName().str();
			// XXX should go through function
			ref<Expr> value = sf.locals[sf.kf->getArgRegister(index++)].value;
			if (isa < ConstantExpr > (value))
				out << "=" << value;
		}
		out << ")";
		if (ii.file != "")
			out << " at " << ii.file << ":" << ii.line;
		out << "\n";
		target = sf.caller;
	}
}
