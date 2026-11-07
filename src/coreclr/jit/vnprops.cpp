// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Contextual VN facts, their provenance, and the IR properties they establish.
#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif
#include "valuenum.h"
#include "ssaconfig.h"

//------------------------------------------------------------------------
// GetWriteBarrierForm: Determinate the exact type of write barrier required for the
//    given address.
//
// Arguments:
//    vnStore - ValueNumStore object
//    vn      - VN of the address
//
// Return Value:
//    Exact type of write barrier required for the given address.
//
static GCInfo::WriteBarrierForm GetWriteBarrierForm(Compiler* comp, ValueNum vn)
{
    ValueNumStore*  vnStore = comp->vnStore;
    const var_types type    = vnStore->TypeOfVN(vn);
    if (type == TYP_REF)
    {
        return GCInfo::WriteBarrierForm::WBF_BarrierUnchecked;
    }
    if (type != TYP_BYREF)
    {
        return GCInfo::WriteBarrierForm::WBF_BarrierUnknown;
    }

    VNFuncApp funcApp;
    if (vnStore->GetVNFunc(vnStore->VNNormalValue(vn), &funcApp))
    {
        if (funcApp.FuncIs(VNF_PtrToArrElem))
        {
            // Check whether the array is on the heap
            ValueNum arrayVN = funcApp.GetArg(1);
            return GetWriteBarrierForm(comp, arrayVN);
        }
        if (funcApp.FuncIs(VNF_PtrToLoc))
        {
            // Pointer to a local
            return GCInfo::WriteBarrierForm::WBF_NoBarrier;
        }
        if ((funcApp.FuncIs(VNF_PtrToStatic)) && vnStore->IsVNHandle(funcApp.GetArg(0), GTF_ICON_STATIC_BOX_PTR))
        {
            // Boxed static - always on the heap
            return GCInfo::WriteBarrierForm::WBF_BarrierUnchecked;
        }
        if (funcApp.FuncIs(VNF_ADD))
        {
            // Check arguments of the GT_ADD
            // To make it conservative, we require one of the arguments to be a constant, e.g.:
            //
            //   addressOfLocal + cns    -> NoBarrier
            //   cns + addressWithinHeap -> BarrierUnchecked
            //
            // Because "addressOfLocal + nativeIntVariable" could be in fact a pointer to the heap.
            // if "nativeIntVariable == addressWithinHeap - addressOfLocal".
            //
            if (vnStore->IsVNConstantNonHandle(funcApp.GetArg(0)))
            {
                return GetWriteBarrierForm(comp, funcApp.GetArg(1));
            }
            if (vnStore->IsVNConstantNonHandle(funcApp.GetArg(1)))
            {
                return GetWriteBarrierForm(comp, funcApp.GetArg(0));
            }
        }
    }
    return GCInfo::WriteBarrierForm::WBF_BarrierUnknown;
}

//------------------------------------------------------------------------
// fgValueNumberWriteBarrier: Publish the barrier classification before CSE can hide
// the store's value or address behind locals. Called after all reaching VNs are available.
//
void Compiler::fgValueNumberWriteBarrier(GenTree* store)
{
    assert(store->OperIs(GT_STOREIND, GT_STORE_BLK));
    if ((store->gtFlags & GTF_IND_TGT_NOT_HEAP) != 0)
    {
        return;
    }

    if (store->OperIs(GT_STOREIND))
    {
        if (!store->TypeIs(TYP_REF) || !store->Data()->TypeIs(TYP_REF))
        {
            return;
        }
        auto isNonGcValue = [this](ValueNum vn) {
            return ((vn == ValueNumStore::VNForNull()) || vnStore->IsVNObjHandle(vn)) ? ValueNumStore::VNVisit::Continue
                                                                                      : ValueNumStore::VNVisit::Abort;
        };
        if (vnStore->VNVisitReachingVNs(vnStore->VNConservativeNormalValue(store->Data()->gtVNPair), isNonGcValue) ==
            ValueNumStore::VNVisit::Continue)
        {
            store->gtFlags |= GTF_IND_TGT_NOT_HEAP;
            return;
        }
    }
    else if (!store->AsBlk()->GetLayout()->HasGCPtr())
    {
        return;
    }

    if ((store->gtFlags & GTF_IND_TGT_HEAP) == 0)
    {
        GCInfo::WriteBarrierForm form =
            GetWriteBarrierForm(this, vnStore->VNConservativeNormalValue(store->AsIndir()->Addr()->gtVNPair));
        if (form == GCInfo::WBF_NoBarrier)
        {
            store->gtFlags |= GTF_IND_TGT_NOT_HEAP;
        }
        else if (form == GCInfo::WBF_BarrierUnchecked)
        {
            store->gtFlags |= GTF_IND_TGT_HEAP;
        }
    }
}

//--------------------------------------------------------------------------------
// fgVNRecordNullBranchFacts: Record null and non-null facts established on branch edges.
//
// Arguments:
//    block - the block whose trees have just been value numbered
//
void Compiler::fgVNRecordNullBranchFacts(BasicBlock* block)
{
    if (!block->KindIs(BBJ_COND) || (block->GetTrueTarget() == block->GetFalseTarget()))
    {
        return;
    }

    GenTree* cond = block->lastStmt()->GetRootNode()->gtGetOp1();
    if (!cond->OperIs(GT_EQ, GT_NE))
    {
        return;
    }

    GenTree* value = cond->gtGetOp1();
    GenTree* zero  = cond->gtGetOp2();
    if (value->IsIntegralConst(0))
    {
        std::swap(value, zero);
    }
    if (!zero->IsIntegralConst(0) || !value->TypeIs(TYP_REF) || !value->gtVNPair.BothDefined())
    {
        return;
    }

    auto edgeEstablishesFact = [this, block](BasicBlock* successor) {
        if ((successor == block) || (successor == fgFirstBB) || !m_dfsTree->Contains(successor))
        {
            return false;
        }

        // A fact about one edge is a fact at the successor only if every entry takes
        // that edge. Include exceptional entries, just as the join proof does.
        FlowEdge* preds = BlockDominancePreds(successor);
        if (preds == nullptr)
        {
            return false;
        }
        for (FlowEdge* edge = preds; edge != nullptr; edge = edge->getNextPredEdge())
        {
            BasicBlock* pred = edge->getSourceBlock();
            if (m_dfsTree->Contains(pred) && (pred != block))
            {
                return false;
            }
        }

        return true;
    };

    BasicBlock* successor = cond->OperIs(GT_NE) ? block->GetTrueTarget() : block->GetFalseTarget();
    if (edgeEstablishesFact(successor))
    {
        ValueNum valueVN = vnStore->VNNormalValue(value->GetVN(VNK_Liberal));
        if (!vnStore->IsKnownNonNull(valueVN))
        {
            fgVNSurvivedException(vnStore->VNForFunc(TYP_REF, VNF_NullPtrExc, valueVN), successor);
            JITDUMP("Non-null branch fact for " FMT_VN " at " FMT_BB "\n", valueVN, successor->bbNum);
        }
        ValueNum conservativeVN = vnStore->VNNormalValue(value->GetVN(VNK_Conservative));
        if ((conservativeVN != valueVN) && !vnStore->IsKnownNonNull(conservativeVN))
        {
            fgVNSurvivedException(vnStore->VNForFunc(TYP_REF, VNF_NullPtrExc, conservativeVN), successor);
            vnHasConservativeBranchFacts = true;
            JITDUMP("Conservative non-null branch fact for " FMT_VN " at " FMT_BB "\n", conservativeVN,
                    successor->bbNum);
        }
    }

    successor = cond->OperIs(GT_EQ) ? block->GetTrueTarget() : block->GetFalseTarget();
    if (edgeEstablishesFact(successor))
    {
        ValueNum valueVN = vnStore->VNConservativeNormalValue(value->gtVNPair);
        if (valueVN != ValueNumStore::VNForNull())
        {
            if (vnKnownNullValues == nullptr)
            {
                vnKnownNullValues = new (getAllocator(CMK_ValueNumber)) VNFactMap(getAllocator(CMK_ValueNumber));
            }
            fgVNGetFactAnchor(vnKnownNullValues, valueVN, successor, true);
            JITDUMP("Null branch fact for " FMT_VN " at " FMT_BB "\n", valueVN, successor->bbNum);
        }
    }
}

//--------------------------------------------------------------------------------
// fgVNSurvivedException: Whether this exception item is ruled out at the given point, and record
//    that the current block survives it. A block one of the recorded blocks dominates adds nothing; a
//    block dominating a recorded one takes its place; any other is a further way of reaching the item
//    and is added. One hash lookup serves both.
//
// Arguments:
//    excItemVN - the exception item
//    block     - the point to query and record; defaults to the current block
//
// Return Value:
//    The establishing block or covered join, or nullptr when the item has not been survived.
//
BasicBlock* Compiler::fgVNSurvivedException(ValueNum excItemVN, BasicBlock* block)
{
    return fgVNGetFactAnchor(vnSurvivedExceptions, excItemVN, block != nullptr ? block : compCurBB, true);
}

// Check a fact's dominance and join coverage. With record=true the caller establishes
// the fact on normal completion. Queries do not establish facts or mutate the map.
// Return the establishing block or covered join, or nullptr when the fact is unavailable.
BasicBlock* Compiler::fgVNGetFactAnchor(VNFactMap* facts, ValueNum factVN, BasicBlock* block, bool record)
{
    if ((facts == nullptr) || (block == nullptr) || (m_domTree == nullptr))
    {
        return nullptr;
    }

    VNFactBlocks* const blocks =
        record ? facts->LookupPointerOrAdd(factVN, VNFactBlocks()) : facts->LookupPointer(factVN);
    if (blocks == nullptr)
    {
        return nullptr;
    }
    if (blocks->first == nullptr)
    {
        if (record)
        {
            blocks->first = block;
        }
        return nullptr;
    }

    const int count   = 1 + ((blocks->more == nullptr) ? 0 : blocks->more->Height());
    auto      blockAt = [blocks](int i) -> BasicBlock*& {
        return (i == 0) ? blocks->first : blocks->more->BottomRef(i - 1);
    };

    for (int i = 0; i < count; i++)
    {
        if (m_domTree->Dominates(blockAt(i), block))
        {
            return blockAt(i);
        }
    }

    // Not dominated by a survivor. A join reached only through survivors counts as one and is recorded
    // as a survivor itself. Any other block is a further way of reaching the item. The join is judged on
    // its dominance predecessors, so a handler counts every way the try can be left before completing.
    FlowEdge* const domPreds    = BlockDominancePreds(block);
    bool            joinCovered = domPreds != nullptr;
    for (FlowEdge* edge = domPreds; edge != nullptr; edge = edge->getNextPredEdge())
    {
        BasicBlock* const pred = edge->getSourceBlock();
        if (!m_dfsTree->Contains(pred))
        {
            continue;
        }

        bool predCovered = false;
        for (int i = 0; i < count; i++)
        {
            if (m_domTree->Dominates(blockAt(i), pred))
            {
                predCovered = true;
                break;
            }
        }
        if (!predCovered)
        {
            joinCovered = false;
            break;
        }
    }

    BasicBlock* proof = joinCovered ? block : nullptr;

    if (!record)
    {
        return proof;
    }

    for (int i = 0; i < count; i++)
    {
        if (m_domTree->Dominates(block, blockAt(i)))
        {
            blockAt(i) = block;
            return proof;
        }
    }
    if (blocks->more == nullptr)
    {
        blocks->more = new (getAllocator(CMK_ValueNumber)) ArrayStack<BasicBlock*>(getAllocator(CMK_ValueNumber), 2);
    }
    blocks->more->Push(block);
    return proof;
}

//------------------------------------------------------------------------
// fgVNCopyExceptionDependencies: Copy proof provenance to a structurally identical expression clone.
//
// Arguments:
//    source - the original expression
//    clone  - its copy, with the same shape and values
//
void Compiler::fgVNCopyExceptionDependencies(GenTree* source, GenTree* clone)
{
    if (vnExceptionDependencies == nullptr)
    {
        return;
    }
    assert(source->OperGet() == clone->OperGet());
    VNExceptionDependency dependency;
    if (vnExceptionDependencies->Lookup(source, &dependency))
    {
        vnExceptionDependencies->Set(clone, dependency);
    }

    // Follow the same paired operand traversal used for loop-memory dependencies.
    auto cloneOperand = clone->OperandsBegin();
    for (GenTree* operand : source->Operands())
    {
        assert(cloneOperand != clone->OperandsEnd());
        fgVNCopyExceptionDependencies(operand, *cloneOperand);
        ++cloneOperand;
    }
    assert(cloneOperand == clone->OperandsEnd());
}

//--------------------------------------------------------------------------------
// fgVNRecordExceptionDependency: Record where a contextual non-throwing proof published on a node holds.
//
// Arguments:
//    node   - the node that was given an ordering side effect for the proof
//    anchor - the block establishing the proof. Consumers must preserve execution through
//             that block before the operation. Inherited dependencies are recorded separately.
//
// Notes:
//    The ordering side effect keeps the node below the check it relies on, but carries no
//    location. Record the proof's establishing block independently of its eventual consumer.
//    When several proofs pin the same node, the anchor dominated by the others is the binding one.
//
void Compiler::fgVNRecordExceptionDependency(GenTree* node, BasicBlock* anchor)
{
    assert(anchor != nullptr);
    if (vnExceptionDependencies == nullptr)
    {
        vnExceptionDependencies =
            new (getAllocator(CMK_ValueNumber)) VNExceptionDependencyMap(getAllocator(CMK_ValueNumber));
    }

    VNExceptionDependency* const existing = vnExceptionDependencies->LookupPointer(node);
    if (existing == nullptr)
    {
        vnExceptionDependencies->Set(node, VNExceptionDependency(anchor));
    }
    else if (existing->IsInherited() || m_domTree->Dominates(existing->GetEstablishingBlock(), anchor))
    {
        *existing = VNExceptionDependency(anchor);
    }
}

//------------------------------------------------------------------------
// fgVNUpdateSideEffects: Refresh flags while distinguishing inherited dependencies
// from ordering effects already present on a node.
//
void Compiler::fgVNUpdateSideEffects(Statement* stmt)
{
    // Nodes that gain an ordering side effect from the refresh below inherit it from a proof
    // published in this statement; record them so code motion can tell them apart from
    // nodes with an ordering dependence of their own.
    ArrayStack<GenTree*> unordered(getAllocator(CMK_ValueNumber));
    if (vnExceptionDependencies != nullptr)
    {
        for (GenTree* const tree : stmt->TreeList())
        {
            if ((tree->gtFlags & GTF_ORDER_SIDEEFF) == 0)
            {
                unordered.Push(tree);
            }
        }
    }

    // Publish the non-faulting and ordering facts to the whole expression before
    // later phases consider moving it or discarding its exception side effects.
    gtUpdateStmtSideEffects(stmt);

    for (int i = 0; i < unordered.Height(); i++)
    {
        GenTree* const tree = unordered.Bottom(i);
        if (((tree->gtFlags & GTF_ORDER_SIDEEFF) != 0) && !vnExceptionDependencies->Lookup(tree))
        {
            vnExceptionDependencies->Set(tree, VNExceptionDependency());
        }
    }
}
