// SPDX-License-Identifier: MIT
/*
$info$
tags: ir|opts
desc: CapyPS5: folds guest data addresses above the host address space onto a host window
$end_info$
*/

#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/Passes.h"
#include "Interface/IR/PassManager.h"

#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/AddressFold.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/Profiler.h>

namespace FEXCore::IR {
namespace {
  // Rewrites the address of every guest memory access to `address & GuestAddressFoldMask` while the
  // mask is set. MemSet and MemCpy fold their working pointers in the backend instead, since their
  // results are the updated guest pointers. Push/Pop only touch the stack, which is a host mapping.
  class GuestAddressFold final : public Pass {
  public:
    void Run(IREmitter* IREmit) override;

  private:
    IREmitter* Emit {};
    uint64_t Mask {};

    Ref Fold(Ref Address) {
      return Emit->_And(OpSize::i64Bit, Address, Emit->_Constant(Mask));
    }

    // Inline constants have no register; an absolute address that small never needs folding.
    bool IsInline(OrderedNodeWrapper Wrapped) {
      return Emit->GetOpHeader(Wrapped)->Op == OP_INLINECONSTANT;
    }

    // Ops with only a base address (an immediate offset, if any, is small).
    void FoldArg(Ref Node, IROp_Header* IROp, size_t Index) {
      const auto Wrapped = IROp->Args[Index];
      if (Wrapped.IsInvalid() || IsInline(Wrapped)) {
        return;
      }
      Emit->SetWriteCursorBefore(Node);
      Emit->ReplaceNodeArgument(Node, Index, Fold(Emit->UnwrapNode(Wrapped)));
    }

    // Ops addressing `Addr + extend(Offset) << log2(OffsetScale)`.
    template<typename T>
    void FoldAddrOffset(Ref Node, IROp_Header* IROp) {
      auto* Op = IROp->CW<T>();
      const auto AddrWrapped = IROp->Args[T::Addr_Index];
      const auto OffsetWrapped = IROp->Args[T::Offset_Index];
      if (AddrWrapped.IsInvalid() || IsInline(AddrWrapped)) {
        return;
      }
      Emit->SetWriteCursorBefore(Node);
      Ref Address = Emit->UnwrapNode(AddrWrapped);
      if (OffsetWrapped.IsInvalid()) {
        Emit->ReplaceNodeArgument(Node, T::Addr_Index, Fold(Address));
        return;
      }

      // A small constant offset cannot cross into another window: fold the base only and keep the
      // addressing mode. Inline constants are the encodable memory offsets (possibly negative);
      // they must stay on the memory op, as an Add cannot encode them.
      uint64_t Constant {};
      if (Emit->GetOpHeader(OffsetWrapped)->Op == OP_INLINECONSTANT ||
          (Emit->IsValueConstant(OffsetWrapped, &Constant) && Op->OffsetType == MemOffsetType::SXTX && Op->OffsetScale == 1 &&
           static_cast<int64_t>(Constant) > -4096 && static_cast<int64_t>(Constant) < 4096)) {
        Emit->ReplaceNodeArgument(Node, T::Addr_Index, Fold(Address));
        return;
      }

      Ref Offset = Emit->UnwrapNode(OffsetWrapped);
      switch (Op->OffsetType) {
      case MemOffsetType::UXTW: Offset = Emit->_Bfe(OpSize::i64Bit, 32, 0, Offset); break;
      case MemOffsetType::SXTW: Offset = Emit->_Sbfe(OpSize::i64Bit, 32, 0, Offset); break;
      default: break;
      }
      if (Op->OffsetScale > 1) {
        Offset = Emit->_Lshl(OpSize::i64Bit, Offset, Emit->_Constant(FEXCore::ilog2(Op->OffsetScale)));
      }
      Ref Folded = Fold(Emit->_Add(OpSize::i64Bit, Address, Offset));
      Emit->ReplaceNodeArgument(Node, T::Addr_Index, Folded);
      Emit->ReplaceNodeArgument(Node, T::Offset_Index, Emit->Invalid());
      Op->OffsetType = MemOffsetType::SXTX;
      Op->OffsetScale = 1;
    }
  };

  void GuestAddressFold::Run(IREmitter* IREmit) {
    Mask = GuestAddressFoldMask.load(std::memory_order_relaxed);
    if (Mask == 0) {
      return;
    }
    FEXCORE_PROFILE_SCOPED("PassManager::GuestAddressFold");
    Emit = IREmit;

    auto CurrentIR = IREmit->ViewIR();
    for (auto [BlockNode, BlockHeader] : CurrentIR.GetBlocks()) {
      IREmit->SetCurrentCodeBlock(BlockNode);
      for (auto [CodeNode, IROp] : CurrentIR.GetCode(BlockNode)) {
        switch (IROp->Op) {
        case OP_LOADMEM: FoldAddrOffset<IROp_LoadMem>(CodeNode, IROp); break;
        case OP_STOREMEM: FoldAddrOffset<IROp_StoreMem>(CodeNode, IROp); break;
        case OP_LOADMEMTSO: FoldAddrOffset<IROp_LoadMemTSO>(CodeNode, IROp); break;
        case OP_STOREMEMTSO: FoldAddrOffset<IROp_StoreMemTSO>(CodeNode, IROp); break;
        case OP_VLOADVECTORMASKED: FoldAddrOffset<IROp_VLoadVectorMasked>(CodeNode, IROp); break;
        case OP_VSTOREVECTORMASKED: FoldAddrOffset<IROp_VStoreVectorMasked>(CodeNode, IROp); break;
        case OP_PREFETCH: FoldAddrOffset<IROp_Prefetch>(CodeNode, IROp); break;
        case OP_STORESTACKMEM: FoldAddrOffset<IROp_StoreStackMem>(CodeNode, IROp); break;

        case OP_LOADMEMPAIR: FoldArg(CodeNode, IROp, IROp_LoadMemPair::Addr_Index); break;
        case OP_STOREMEMPAIR: FoldArg(CodeNode, IROp, IROp_StoreMemPair::Addr_Index); break;
        case OP_STOREMEMX87SVEOPTPREDICATE: FoldArg(CodeNode, IROp, IROp_StoreMemX87SVEOptPredicate::Addr_Index); break;
        case OP_LOADMEMX87SVEOPTPREDICATE: FoldArg(CodeNode, IROp, IROp_LoadMemX87SVEOptPredicate::Addr_Index); break;
        case OP_VLOADVECTORGATHERMASKED: FoldArg(CodeNode, IROp, IROp_VLoadVectorGatherMasked::AddrBase_Index); break;
        case OP_VLOADVECTORGATHERMASKEDQPS: FoldArg(CodeNode, IROp, IROp_VLoadVectorGatherMaskedQPS::AddrBase_Index); break;
        case OP_VLOADVECTORELEMENT: FoldArg(CodeNode, IROp, IROp_VLoadVectorElement::Addr_Index); break;
        case OP_VSTOREVECTORELEMENT: FoldArg(CodeNode, IROp, IROp_VStoreVectorElement::Addr_Index); break;
        case OP_VBROADCASTFROMMEM: FoldArg(CodeNode, IROp, IROp_VBroadcastFromMem::Address_Index); break;
        case OP_CACHELINECLEAR: FoldArg(CodeNode, IROp, IROp_CacheLineClear::Addr_Index); break;
        case OP_CACHELINECLEAN: FoldArg(CodeNode, IROp, IROp_CacheLineClean::Addr_Index); break;
        case OP_CACHELINEZERO: FoldArg(CodeNode, IROp, IROp_CacheLineZero::Addr_Index); break;
        case OP_VSTORENONTEMPORAL: FoldArg(CodeNode, IROp, IROp_VStoreNonTemporal::Addr_Index); break;
        case OP_VSTORENONTEMPORALPAIR: FoldArg(CodeNode, IROp, IROp_VStoreNonTemporalPair::Addr_Index); break;
        case OP_VLOADNONTEMPORAL: FoldArg(CodeNode, IROp, IROp_VLoadNonTemporal::Addr_Index); break;
        case OP_CAS: FoldArg(CodeNode, IROp, IROp_CAS::Addr_Index); break;
        case OP_CASPAIR: FoldArg(CodeNode, IROp, IROp_CASPair::Addr_Index); break;
        case OP_ATOMICSWAP: FoldArg(CodeNode, IROp, IROp_AtomicSwap::Addr_Index); break;
        case OP_ATOMICFETCHADD: FoldArg(CodeNode, IROp, IROp_AtomicFetchAdd::Addr_Index); break;
        case OP_ATOMICFETCHSUB: FoldArg(CodeNode, IROp, IROp_AtomicFetchSub::Addr_Index); break;
        case OP_ATOMICFETCHAND: FoldArg(CodeNode, IROp, IROp_AtomicFetchAnd::Addr_Index); break;
        case OP_ATOMICFETCHCLR: FoldArg(CodeNode, IROp, IROp_AtomicFetchCLR::Addr_Index); break;
        case OP_ATOMICFETCHOR: FoldArg(CodeNode, IROp, IROp_AtomicFetchOr::Addr_Index); break;
        case OP_ATOMICFETCHXOR: FoldArg(CodeNode, IROp, IROp_AtomicFetchXor::Addr_Index); break;
        case OP_ATOMICFETCHNEG: FoldArg(CodeNode, IROp, IROp_AtomicFetchNeg::Addr_Index); break;
        default: break;
        }
      }
    }
  }
} // namespace

fextl::unique_ptr<Pass> CreateGuestAddressFoldPass() {
  return fextl::make_unique<GuestAddressFold>();
}
} // namespace FEXCore::IR
