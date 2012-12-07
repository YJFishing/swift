//===--- SILGenDecl.cpp - Implements Lowering of ASTs -> SIL for Decls ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "ManagedValue.h"
#include "Scope.h"
#include "TypeInfo.h"
#include "swift/SIL/BBArgument.h"
#include "swift/AST/AST.h"
#include <iterator>
using namespace swift;
using namespace Lowering;

#include "llvm/Support/raw_ostream.h"

namespace {
  class CleanupClosure : public Cleanup {
    Value closure;
  public:
    CleanupClosure(Value closure) : closure(closure) {}
    void emit(SILGenFunction &gen) override {
      gen.B.createRelease(SILLocation(), closure);
    }
  };
}

void SILGenFunction::visitFuncDecl(FuncDecl *fd) {
  // Generate the local function body.
  SGM.emitFunction(fd, fd->getBody());
  
  // If there are captures, build the local closure value for the function and
  // store it as a local constant.
  if (!fd->getBody()->getCaptures().empty()) {
    Value closure = emitClosureForCapturingExpr(fd, SILConstant(fd),
                                                fd->getBody())
      .forward(*this);
    Cleanups.pushCleanup<CleanupClosure>(closure);
    LocalConstants[fd] = closure;
  }
}

namespace {

class CleanupVar : public Cleanup {
  AllocBoxInst *box;
public:
  CleanupVar(AllocBoxInst *box) : box(box) {}
  void emit(SILGenFunction &gen) override {
    gen.B.createRelease(SILLocation(), Value(box, 0));
  }
};
  
/// InitPatternWithExpr - A visitor for traversing a pattern and generating SIL
/// code to allocate the declared variables and generate initialization
/// sequences for their values.  If an initial value is specified by the "Init"
/// expression, then it is used to initialize the corresponding decls.  If not,
/// the decls are default initialized.  This does not occur within the
/// initializer's full-expression;  that should be pushed at the appropriate
/// moment.
struct InitPatternWithExpr : public PatternVisitor<InitPatternWithExpr> {
  SILGenFunction &Gen;
  Value Init;
  InitPatternWithExpr(SILGenFunction &Gen, Value Init) : Gen(Gen), Init(Init) {}
  
  // Paren & Typed patterns are noops, just look through them.
  void visitParenPattern(ParenPattern *P) { visit(P->getSubPattern()); }
  void visitTypedPattern(TypedPattern *P) { visit(P->getSubPattern()); }
  
  // AnyPatterns (i.e, _) don't require any further codegen beyond emitting the
  // initializer.
  void visitAnyPattern(AnyPattern *P) {}
  
  // Bind to a named pattern by creating a memory location and initializing it
  // with the initial value.
  void visitNamedPattern(NamedPattern *P) {
    VarDecl *vd = P->getDecl();

    // If this is a [byref] argument, just use the argument lvalue as our
    // address.
    // FIXME: capturing byrefs?
    if (vd->getType()->is<LValueType>()) {
      Gen.VarLocs[vd] = {Value(), Init};
      return;
    }

    // FIXME: Use escape analysis info to generate "alloc_var"/"dealloc_var"
    // stack allocations instead of "alloc_box"/"release" for values that don't
    // escape and thus don't need boxes.
    auto allocBox = Gen.B.createAllocBox(vd, vd->getType());
    auto box = Value(allocBox, 0);
    auto addr = Value(allocBox, 1);

    /// Remember that this is the memory location that we're emitting the
    /// decl to.
    Gen.VarLocs[vd] = {box, addr};

    // FIXME: "Default zero initialization" is a dubious concept.  When we get
    // something like typestate or another concept that allows us to model
    // definitive assignment, then we can consider removing it.
    // FIXME: Indirect returns (such as for address-only types) could be
    // emplaced directly into the storage for the variable.
    auto initVal = Init ? Init : Gen.B.createZeroValue(vd, vd->getType());
    Gen.B.createStore(vd, initVal, addr);

    Gen.Cleanups.pushCleanup<CleanupVar>(allocBox);
  }
  
  // Bind to a tuple pattern by first trying to see if we can emit
  // the initializers independently.
  void visitTuplePattern(TuplePattern *P) {
    // If we have no initializer, just emit the subpatterns using
    // the missing initializer.
    if (Init == nullptr) {
      for (auto &elt : P->getFields())
        visit(elt.getPattern());
      return;
    }

    // Otherwise, iterate through the fields of the tuple that we're
    // initializing and extract the interesting bits of Init out for each tuple
    // element.
    unsigned FieldNo = 0;
    Value TupleInit = Init;
    for (auto &elt : P->getFields()) {
      Init = Gen.B.createExtract(SILLocation(), TupleInit, FieldNo++,
                                 elt.getPattern()->getType());
      visit(elt.getPattern());
    }
  }
};
} // end anonymous namespace


void SILGenFunction::visitPatternBindingDecl(PatternBindingDecl *D) {
  // FIXME: Implement cleanups in a way that stands up to unwinding and handles
  // cleanup of partial initializations.
  
  // Actually emit the code to allocate space for each declared variable, and
  // then initialize them.

  // If an initial value was specified by the decl, use it to produce the
  // initial values, otherwise use a "zero" placeholder value as the
  // initializer.
  Value initializer = nullptr;
  if (D->getInit()) {
    FullExpr Scope(Cleanups);
    initializer = visit(D->getInit()).forward(*this);
  }
  InitPatternWithExpr(*this, initializer).visit(D->getPattern());
}


namespace {

/// ArgumentCreatorVisitor - A visitor for traversing a pattern and creating
/// BBArguments for each pattern variable.  This is used to create function
/// arguments and to set up cleanups to release the arguments on function exit.
struct ArgumentCreatorVisitor :
  public PatternVisitor<ArgumentCreatorVisitor, Value> {
  SILGenFunction &gen;
  Function &f;
  ArgumentCreatorVisitor(SILGenFunction &gen, Function &f) : gen(gen), f(f) {}

  Value makeArgument(Type ty, BasicBlock *parent) {
    return new (f.getModule()) BBArgument(ty, parent);
  }
    
  // Paren & Typed patterns are noops, just look through them.
  Value visitParenPattern(ParenPattern *P) {return visit(P->getSubPattern());}
  Value visitTypedPattern(TypedPattern *P) {return visit(P->getSubPattern());}

  // Bind to a tuple pattern by first trying to see if we can emit
  // the initializers independently.
  Value visitTuplePattern(TuplePattern *P) {
    SmallVector<Value, 4> Elements;
    for (auto &elt : P->getFields())
      Elements.push_back(visit(elt.getPattern()));

    SILBuilder B(f.begin(), f);
    return B.createTuple(SILLocation(), P->getType(), Elements);
  }

  Value visitAnyPattern(AnyPattern *P) {
    return makeArgument(P->getType(), f.begin());
  }

  Value visitNamedPattern(NamedPattern *P) {
    return makeArgument(P->getType(), f.begin());
  }
};

class CleanupCaptureBox : public Cleanup {
  Value box;
public:
  CleanupCaptureBox(Value box) : box(box) {}
  void emit(SILGenFunction &gen) override {
    gen.B.createRelease(SILLocation(), box);
  }
};
  
class CleanupCaptureValue : public Cleanup {
  Value v;
public:
  CleanupCaptureValue(Value v) : v(v) {}
  void emit(SILGenFunction &gen) override {
    gen.emitReleaseRValue(SILLocation(), v);
  }
};
  
static void makeCaptureBBArguments(SILGenFunction &gen, ValueDecl *capture) {
  switch (gen.getDeclCaptureKind(capture)) {
  case CaptureKind::LValue: {
    // LValues are captured as two arguments: a retained ObjectPointer that owns
    // the captured value, and the address of the value itself.
    Type ty = capture->getTypeOfReference();
    ASTContext &ctx = ty->getASTContext();
    Value box = new (gen.SGM.M) BBArgument(ctx.TheObjectPointerType,
                                           gen.F.begin());
    Value addr = new (gen.SGM.M) BBArgument(ty,
                                            gen.F.begin());
    gen.VarLocs[capture] = {box, addr};
    gen.Cleanups.pushCleanup<CleanupCaptureBox>(box);
    break;
  }
  case CaptureKind::Byref: {
    // Byref captures are non-escaping, so it's sufficient to capture only the
    // address.
    Type ty = capture->getTypeOfReference();
    Value addr = new (gen.SGM.M) BBArgument(ty, gen.F.begin());
    gen.VarLocs[capture] = {Value(), addr};
    break;
  }
  case CaptureKind::Constant: {
    // Constants are captured by value.
    Type ty = capture->getType();
    assert(!ty->is<LValueType>() && "capturing byref by value?!");
    Value value = new (gen.SGM.M) BBArgument(ty, gen.F.begin());
    gen.LocalConstants[capture] = value;
    gen.Cleanups.pushCleanup<CleanupCaptureValue>(value);
    break;
  }
  }
}
  
} // end anonymous namespace

void SILGenFunction::emitProlog(CapturingExpr *ce,
                                ArrayRef<Pattern*> paramPatterns) {
  // Emit the capture variables.
  for (auto capture : ce->getCaptures()) {
    makeCaptureBBArguments(*this, capture);
  }
  
  // Emit the argument variables.
  for (auto &ParamPattern : paramPatterns) {
    // Add the BBArguments and collect them as a Value.
    Value ArgInit = ArgumentCreatorVisitor(*this, F).visit(ParamPattern);
    // Use the value to initialize a (mutable) variable allocation.
    InitPatternWithExpr(*this, ArgInit).visit(ParamPattern);
  }
}

template<typename X>
static void rrLoadableValueElement(SILGenFunction &gen, SILLocation loc, Value v,
                                   X (SILBuilder::*createRR)(SILLocation,
                                                             Value),
                                   ReferenceTypeElement const &elt) {
  for (FragileElement comp : elt.path) {
    v = gen.B.createExtract(loc, v, comp.index, comp.type);
  }
  (gen.B.*createRR)(loc, v);
}

template<typename X>
static void rrLoadableValue(SILGenFunction &gen, SILLocation loc, Value v,
                            X (SILBuilder::*createRR)(SILLocation, Value),
                            ArrayRef<ReferenceTypeElement> elts) {
  for (auto &elt : elts)
    rrLoadableValueElement(gen, loc, v, createRR, elt);
}

void SILGenFunction::emitAssign(SILLocation loc, Value v, Value dest) {
  Type vTy = v.getType();

  if (vTy->is<LValueType>()) {
    // v is an address-only type; copy using the copy_addr instruction.
    assert(dest.getType()->getCanonicalType() == vTy->getCanonicalType() &&
           "type of copy_addr destination must match source address type");
    assert(getTypeInfo(vTy->getRValueType()).isAddressOnly() &&
           "source of copy may only be an address if type is address-only");
    B.createCopyAddr(loc, v, dest,
                     /*isTake=*/false,
                     /*isInitialize=*/false);
  } else {
    // v is a loadable type; release the old value if necessary
    TypeInfo const &ti = getTypeInfo(vTy);
    assert(dest.getType()->is<LValueType>() &&
           "copy destination must be an address");
    assert(dest.getType()->getRValueType()->getCanonicalType() ==
             vTy->getCanonicalType() &&
           "copy destination must be an address of the type of the source");
    assert(ti.isLoadable() &&
           "copy of address-only type must take address for source argument");
    Value old;

    if (!ti.isTrivial())
      old = B.createLoad(loc, dest);
    
    B.createStore(loc, v, dest);
    rrLoadableValue(*this, loc, old, &SILBuilder::createRelease,
                    ti.getReferenceTypeElements());
  }
}

void SILGenFunction::emitRetainRValue(SILLocation loc, Value v) {
  if (v.getType()->is<LValueType>()) {
    // v is an address-only type.
    // FIXME: should allocate, copy_addr, and return a temporary
    llvm_unreachable("FIXME: address-only types not implemented yet");
  } else {
    // v is a loadable type; retain it if necessary.
    rrLoadableValue(*this, loc, v, &SILBuilder::createRetain,
                    getTypeInfo(v.getType()).getReferenceTypeElements());
  }
}

void SILGenFunction::emitReleaseRValue(SILLocation loc, Value v) {
  if (v.getType()->is<LValueType>()) {
    // v is an address-only type; destroy it indirectly with destroy_addr.
    assert(getTypeInfo(v.getType()).isAddressOnly() &&
           "released address is not of an address-only type");
    B.createDestroyAddr(loc, v);
  } else {
    // v is a loadable type; release it if necessary.
    rrLoadableValue(*this, loc, v, &SILBuilder::createRelease,
                    getTypeInfo(v.getType()).getReferenceTypeElements());
  }
}