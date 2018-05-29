/*
 * Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "opto/addnode.hpp"
#include "opto/callnode.hpp"
#include "opto/castnode.hpp"
#include "opto/connode.hpp"
#include "opto/graphKit.hpp"
#include "opto/matcher.hpp"
#include "opto/phaseX.hpp"
#include "opto/rootnode.hpp"
#include "opto/subnode.hpp"
#include "opto/type.hpp"
#include "opto/valuetypenode.hpp"

//=============================================================================
// If input is already higher or equal to cast type, then this is an identity.
Node* ConstraintCastNode::Identity(PhaseGVN* phase) {
  Node* dom = dominating_cast(phase, phase);
  if (dom != NULL) {
    return dom;
  }
  if (_carry_dependency) {
    return this;
  }
  return phase->type(in(1))->higher_equal_speculative(_type) ? in(1) : this;
}

//------------------------------Value------------------------------------------
// Take 'join' of input and cast-up type
const Type* ConstraintCastNode::Value(PhaseGVN* phase) const {
  if (in(0) && phase->type(in(0)) == Type::TOP) return Type::TOP;
  const Type* ft = phase->type(in(1))->filter_speculative(_type);

#ifdef ASSERT
  // Previous versions of this function had some special case logic,
  // which is no longer necessary.  Make sure of the required effects.
  switch (Opcode()) {
    case Op_CastII:
    {
      const Type* t1 = phase->type(in(1));
      if( t1 == Type::TOP )  assert(ft == Type::TOP, "special case #1");
      const Type* rt = t1->join_speculative(_type);
      if (rt->empty())       assert(ft == Type::TOP, "special case #2");
      break;
    }
    case Op_CastPP:
    if (phase->type(in(1)) == TypePtr::NULL_PTR &&
        _type->isa_ptr() && _type->is_ptr()->_ptr == TypePtr::NotNull)
    assert(ft == Type::TOP, "special case #3");
    break;
  }
#endif //ASSERT

  return ft;
}

//------------------------------Ideal------------------------------------------
// Return a node which is more "ideal" than the current node.  Strip out
// control copies
Node *ConstraintCastNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  return (in(0) && remove_dead_region(phase, can_reshape)) ? this : NULL;
}

uint ConstraintCastNode::cmp(const Node &n) const {
  return TypeNode::cmp(n) && ((ConstraintCastNode&)n)._carry_dependency == _carry_dependency;
}

uint ConstraintCastNode::size_of() const {
  return sizeof(*this);
}

Node* ConstraintCastNode::make_cast(int opcode, Node* c, Node *n, const Type *t, bool carry_dependency) {
  switch(opcode) {
  case Op_CastII: {
    Node* cast = new CastIINode(n, t, carry_dependency);
    cast->set_req(0, c);
    return cast;
  }
  case Op_CastPP: {
    Node* cast = new CastPPNode(n, t, carry_dependency);
    cast->set_req(0, c);
    return cast;
  }
  case Op_CheckCastPP: return new CheckCastPPNode(c, n, t, carry_dependency);
  default:
    fatal("Bad opcode %d", opcode);
  }
  return NULL;
}

TypeNode* ConstraintCastNode::dominating_cast(PhaseGVN* gvn, PhaseTransform* pt) const {
  Node* val = in(1);
  Node* ctl = in(0);
  int opc = Opcode();
  if (ctl == NULL) {
    return NULL;
  }
  // Range check CastIIs may all end up under a single range check and
  // in that case only the narrower CastII would be kept by the code
  // below which would be incorrect.
  if (is_CastII() && as_CastII()->has_range_check()) {
    return NULL;
  }
  if (type()->isa_rawptr() && (gvn->type_or_null(val) == NULL || gvn->type(val)->isa_oopptr())) {
    return NULL;
  }
  for (DUIterator_Fast imax, i = val->fast_outs(imax); i < imax; i++) {
    Node* u = val->fast_out(i);
    if (u != this &&
        u->outcnt() > 0 &&
        u->Opcode() == opc &&
        u->in(0) != NULL &&
        u->bottom_type()->higher_equal(type())) {
      if (pt->is_dominator(u->in(0), ctl)) {
        return u->as_Type();
      }
      if (is_CheckCastPP() && u->in(1)->is_Proj() && u->in(1)->in(0)->is_Allocate() &&
          u->in(0)->is_Proj() && u->in(0)->in(0)->is_Initialize() &&
          u->in(1)->in(0)->as_Allocate()->initialization() == u->in(0)->in(0)) {
        // CheckCastPP following an allocation always dominates all
        // use of the allocation result
        return u->as_Type();
      }
    }
  }
  return NULL;
}

#ifndef PRODUCT
void ConstraintCastNode::dump_spec(outputStream *st) const {
  TypeNode::dump_spec(st);
  if (_carry_dependency) {
    st->print(" carry dependency");
  }
}
#endif

const Type* CastIINode::Value(PhaseGVN* phase) const {
  const Type *res = ConstraintCastNode::Value(phase);

  // Try to improve the type of the CastII if we recognize a CmpI/If
  // pattern.
  if (_carry_dependency) {
    if (in(0) != NULL && in(0)->in(0) != NULL && in(0)->in(0)->is_If()) {
      assert(in(0)->is_IfFalse() || in(0)->is_IfTrue(), "should be If proj");
      Node* proj = in(0);
      if (proj->in(0)->in(1)->is_Bool()) {
        Node* b = proj->in(0)->in(1);
        if (b->in(1)->Opcode() == Op_CmpI) {
          Node* cmp = b->in(1);
          if (cmp->in(1) == in(1) && phase->type(cmp->in(2))->isa_int()) {
            const TypeInt* in2_t = phase->type(cmp->in(2))->is_int();
            const Type* t = TypeInt::INT;
            BoolTest test = b->as_Bool()->_test;
            if (proj->is_IfFalse()) {
              test = test.negate();
            }
            BoolTest::mask m = test._test;
            jlong lo_long = min_jint;
            jlong hi_long = max_jint;
            if (m == BoolTest::le || m == BoolTest::lt) {
              hi_long = in2_t->_hi;
              if (m == BoolTest::lt) {
                hi_long -= 1;
              }
            } else if (m == BoolTest::ge || m == BoolTest::gt) {
              lo_long = in2_t->_lo;
              if (m == BoolTest::gt) {
                lo_long += 1;
              }
            } else if (m == BoolTest::eq) {
              lo_long = in2_t->_lo;
              hi_long = in2_t->_hi;
            } else if (m == BoolTest::ne) {
              // can't do any better
            } else {
              stringStream ss;
              test.dump_on(&ss);
              fatal("unexpected comparison %s", ss.as_string());
            }
            int lo_int = (int)lo_long;
            int hi_int = (int)hi_long;

            if (lo_long != (jlong)lo_int) {
              lo_int = min_jint;
            }
            if (hi_long != (jlong)hi_int) {
              hi_int = max_jint;
            }

            t = TypeInt::make(lo_int, hi_int, Type::WidenMax);

            res = res->filter_speculative(t);

            return res;
          }
        }
      }
    }
  }
  return res;
}

Node *CastIINode::Ideal(PhaseGVN *phase, bool can_reshape) {
  Node* progress = ConstraintCastNode::Ideal(phase, can_reshape);
  if (progress != NULL) {
    return progress;
  }

  // Similar to ConvI2LNode::Ideal() for the same reasons
  // Do not narrow the type of range check dependent CastIINodes to
  // avoid corruption of the graph if a CastII is replaced by TOP but
  // the corresponding range check is not removed.
  if (can_reshape && !_range_check_dependency && !phase->C->major_progress()) {
    const TypeInt* this_type = this->type()->is_int();
    const TypeInt* in_type = phase->type(in(1))->isa_int();
    if (in_type != NULL && this_type != NULL &&
        (in_type->_lo != this_type->_lo ||
         in_type->_hi != this_type->_hi)) {
      jint lo1 = this_type->_lo;
      jint hi1 = this_type->_hi;
      int w1  = this_type->_widen;

      if (lo1 >= 0) {
        // Keep a range assertion of >=0.
        lo1 = 0;        hi1 = max_jint;
      } else if (hi1 < 0) {
        // Keep a range assertion of <0.
        lo1 = min_jint; hi1 = -1;
      } else {
        lo1 = min_jint; hi1 = max_jint;
      }
      const TypeInt* wtype = TypeInt::make(MAX2(in_type->_lo, lo1),
                                           MIN2(in_type->_hi, hi1),
                                           MAX2((int)in_type->_widen, w1));
      if (wtype != type()) {
        set_type(wtype);
        return this;
      }
    }
  }
  return NULL;
}

uint CastIINode::cmp(const Node &n) const {
  return ConstraintCastNode::cmp(n) && ((CastIINode&)n)._range_check_dependency == _range_check_dependency;
}

uint CastIINode::size_of() const {
  return sizeof(*this);
}

#ifndef PRODUCT
void CastIINode::dump_spec(outputStream* st) const {
  ConstraintCastNode::dump_spec(st);
  if (_range_check_dependency) {
    st->print(" range check dependency");
  }
}
#endif

//=============================================================================
//------------------------------Identity---------------------------------------
// If input is already higher or equal to cast type, then this is an identity.
Node* CheckCastPPNode::Identity(PhaseGVN* phase) {
  // This is a value type, its input is a phi. That phi is also a
  // value type of that same type and its inputs are value types of
  // the same type: push the cast through the phi.
  if (phase->is_IterGVN() &&
      in(0) == NULL &&
      type()->is_valuetypeptr() &&
      in(1) != NULL &&
      in(1)->is_Phi()) {
    PhaseIterGVN* igvn = phase->is_IterGVN();
    Node* phi = in(1);
    const Type* vtptr = type();
    for (uint i = 1; i < phi->req(); i++) {
      if (phi->in(i) != NULL && !phase->type(phi->in(i))->higher_equal(vtptr)) {
        Node* cast = phase->transform(new CheckCastPPNode(NULL, phi->in(i), vtptr));
        igvn->replace_input_of(phi, i, cast);
      }
    }
    return phi;
  }

  Node* dom = dominating_cast(phase, phase);
  if (dom != NULL) {
    return dom;
  }
  if (_carry_dependency) {
    return this;
  }
  // Toned down to rescue meeting at a Phi 3 different oops all implementing
  // the same interface.  CompileTheWorld starting at 502, kd12rc1.zip.
  return (phase->type(in(1)) == phase->type(this)) ? in(1) : this;
}

//------------------------------Value------------------------------------------
// Take 'join' of input and cast-up type, unless working with an Interface
const Type* CheckCastPPNode::Value(PhaseGVN* phase) const {
  if( in(0) && phase->type(in(0)) == Type::TOP ) return Type::TOP;

  const Type *inn = phase->type(in(1));
  if( inn == Type::TOP ) return Type::TOP;  // No information yet

  const TypePtr *in_type   = inn->isa_ptr();
  const TypePtr *my_type   = _type->isa_ptr();
  const Type *result = _type;
  if( in_type != NULL && my_type != NULL ) {
    TypePtr::PTR   in_ptr    = in_type->ptr();
    if (in_ptr == TypePtr::Null) {
      result = in_type;
    } else if (in_ptr == TypePtr::Constant) {
      if (my_type->isa_rawptr()) {
        result = my_type;
      } else {
        const TypeOopPtr *jptr = my_type->isa_oopptr();
        assert(jptr, "");
        result = !in_type->higher_equal(_type)
          ? my_type->cast_to_ptr_type(TypePtr::NotNull)
          : in_type;
      }
    } else {
      result =  my_type->cast_to_ptr_type( my_type->join_ptr(in_ptr) );
    }
  }

  // This is the code from TypePtr::xmeet() that prevents us from
  // having 2 ways to represent the same type. We have to replicate it
  // here because we don't go through meet/join.
  if (result->remove_speculative() == result->speculative()) {
    result = result->remove_speculative();
  }

  // Same as above: because we don't go through meet/join, remove the
  // speculative type if we know we won't use it.
  return result->cleanup_speculative();

  // JOIN NOT DONE HERE BECAUSE OF INTERFACE ISSUES.
  // FIX THIS (DO THE JOIN) WHEN UNION TYPES APPEAR!

  //
  // Remove this code after overnight run indicates no performance
  // loss from not performing JOIN at CheckCastPPNode
  //
  // const TypeInstPtr *in_oop = in->isa_instptr();
  // const TypeInstPtr *my_oop = _type->isa_instptr();
  // // If either input is an 'interface', return destination type
  // assert (in_oop == NULL || in_oop->klass() != NULL, "");
  // assert (my_oop == NULL || my_oop->klass() != NULL, "");
  // if( (in_oop && in_oop->klass()->is_interface())
  //   ||(my_oop && my_oop->klass()->is_interface()) ) {
  //   TypePtr::PTR  in_ptr = in->isa_ptr() ? in->is_ptr()->_ptr : TypePtr::BotPTR;
  //   // Preserve cast away nullness for interfaces
  //   if( in_ptr == TypePtr::NotNull && my_oop && my_oop->_ptr == TypePtr::BotPTR ) {
  //     return my_oop->cast_to_ptr_type(TypePtr::NotNull);
  //   }
  //   return _type;
  // }
  //
  // // Neither the input nor the destination type is an interface,
  //
  // // history: JOIN used to cause weird corner case bugs
  // //          return (in == TypeOopPtr::NULL_PTR) ? in : _type;
  // // JOIN picks up NotNull in common instance-of/check-cast idioms, both oops.
  // // JOIN does not preserve NotNull in other cases, e.g. RawPtr vs InstPtr
  // const Type *join = in->join(_type);
  // // Check if join preserved NotNull'ness for pointers
  // if( join->isa_ptr() && _type->isa_ptr() ) {
  //   TypePtr::PTR join_ptr = join->is_ptr()->_ptr;
  //   TypePtr::PTR type_ptr = _type->is_ptr()->_ptr;
  //   // If there isn't any NotNull'ness to preserve
  //   // OR if join preserved NotNull'ness then return it
  //   if( type_ptr == TypePtr::BotPTR  || type_ptr == TypePtr::Null ||
  //       join_ptr == TypePtr::NotNull || join_ptr == TypePtr::Constant ) {
  //     return join;
  //   }
  //   // ELSE return same old type as before
  //   return _type;
  // }
  // // Not joining two pointers
  // return join;
}

Node* CheckCastPPNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  // This is a value type. Its input is the return of a call: the call
  // returns a value type and we now know its exact type: build a
  // ValueTypePtrNode from the call.
  const TypeInstPtr* cast_type = type()->isa_instptr();
  if (can_reshape &&
      in(0) == NULL &&
      phase->C->can_add_value_type() &&
      cast_type && cast_type->is_valuetypeptr() &&
      in(1) != NULL && in(1)->is_Proj() &&
      in(1)->in(0) != NULL && in(1)->in(0)->is_CallStaticJava() &&
      in(1)->in(0)->as_CallStaticJava()->method() != NULL &&
      in(1)->as_Proj()->_con == TypeFunc::Parms) {
    ciValueKlass* vk = cast_type->value_klass();
    PhaseIterGVN* igvn = phase->is_IterGVN();

    if (ValueTypeReturnedAsFields && vk->can_be_returned_as_fields()) {
      igvn->set_delay_transform(true);
      CallNode* call = in(1)->in(0)->as_Call();
      igvn->C->remove_macro_node(call);
      // We now know the return type of the call
      const TypeTuple* range_sig = TypeTuple::make_range(vk, false);
      const TypeTuple* range_cc = TypeTuple::make_range(vk, true);
      assert(range_sig != call->_tf->range_sig() && range_cc != call->_tf->range_cc(), "type should change");
      call->_tf = TypeFunc::make(call->_tf->domain_sig(), call->_tf->domain_cc(),
                                 range_sig, range_cc);
      igvn->set_type(call, call->Value(igvn));
      igvn->set_type(in(1), in(1)->Value(igvn));

      Node* ctl_hook = new Node(1);
      Node* mem_hook = new Node(1);
      Node* io_hook = new Node(1);
      Node* res_hook = new Node(1);
      Node* ex_ctl_hook = new Node(1);
      Node* ex_mem_hook = new Node(1);
      Node* ex_io_hook = new Node(1);

      // Extract projections from the call and hook users to temporary nodes.
      // We will re-attach them to newly created PhiNodes below.
      CallProjections* projs = call->extract_projections(true, true);
      assert(projs->nb_resproj == 1, "unexpected number of results");
      igvn->replace_in_uses(projs->fallthrough_catchproj, ctl_hook);
      igvn->replace_in_uses(projs->fallthrough_memproj, mem_hook);
      igvn->replace_in_uses(projs->fallthrough_ioproj, io_hook);
      igvn->replace_in_uses(projs->resproj[0], res_hook);
      igvn->replace_in_uses(projs->catchall_catchproj, ex_ctl_hook);
      igvn->replace_in_uses(projs->catchall_memproj, ex_mem_hook);
      igvn->replace_in_uses(projs->catchall_ioproj, ex_io_hook);

      // Restore IO input of the CatchNode
      CatchNode* catchp = projs->fallthrough_catchproj->in(0)->as_Catch();
      catchp->set_req(TypeFunc::I_O, projs->catchall_ioproj);
      igvn->rehash_node_delayed(catchp);

      // Rebuild the output JVMState from the call and use it to initialize a GraphKit
      JVMState* new_jvms = call->jvms()->clone_shallow(igvn->C);
      SafePointNode* new_map = new SafePointNode(call->req(), new_jvms);
      for (uint i = TypeFunc::FramePtr; i < call->req(); i++) {
        new_map->init_req(i, call->in(i));
      }
      new_map->set_control(projs->fallthrough_catchproj);
      new_map->set_memory(MergeMemNode::make(projs->fallthrough_memproj));
      new_map->set_i_o(projs->fallthrough_ioproj);
      new_jvms->set_map(new_map);

      GraphKit kit(new_jvms, igvn);

      // Either we get a buffered value pointer and we can case use it
      // or we get a tagged klass pointer and we need to allocate a value.
      Node* cast = igvn->transform(new CastP2XNode(kit.control(), projs->resproj[0]));
      Node* masked = igvn->transform(new AndXNode(cast, igvn->MakeConX(0x1)));
      Node* cmp = igvn->transform(new CmpXNode(masked, igvn->MakeConX(0x1)));
      Node* bol = kit.Bool(cmp, BoolTest::eq);
      IfNode* iff = kit.create_and_map_if(kit.control(), bol, PROB_MAX, COUNT_UNKNOWN);
      Node* iftrue = kit.IfTrue(iff);
      Node* iffalse = kit.IfFalse(iff);

      Node* region = new RegionNode(3);
      Node* mem_phi = new PhiNode(region, Type::MEMORY, TypePtr::BOTTOM);
      Node* io_phi = new PhiNode(region, Type::ABIO);
      Node* res_phi = new PhiNode(region, cast_type);
      Node* ex_region = new RegionNode(3);
      Node* ex_mem_phi = new PhiNode(ex_region, Type::MEMORY, TypePtr::BOTTOM);
      Node* ex_io_phi = new PhiNode(ex_region, Type::ABIO);

      // True branch: result is a tagged klass pointer
      // Allocate a value type (will add extra projections to the call)
      kit.set_control(iftrue);
      Node* res = igvn->transform(ValueTypePtrNode::make_from_call(&kit, vk, call));
      res = res->isa_ValueTypePtr()->allocate(&kit);

      // Get exception state
      GraphKit ekit(kit.transfer_exceptions_into_jvms(), igvn);
      SafePointNode* ex_map = ekit.combine_and_pop_all_exception_states();
      Node* ex_oop = ekit.use_exception_state(ex_map);

      region->init_req(1, kit.control());
      mem_phi->init_req(1, kit.reset_memory());
      io_phi->init_req(1, kit.i_o());
      res_phi->init_req(1, res);
      ex_region->init_req(1, ekit.control());
      ex_mem_phi->init_req(1, ekit.reset_memory());
      ex_io_phi->init_req(1, ekit.i_o());

      // False branch: result is not tagged
      // Load buffered value type from returned oop
      kit.set_control(iffalse);
      kit.set_all_memory(projs->fallthrough_memproj);
      kit.set_i_o(projs->fallthrough_ioproj);
      // Cast oop to NotNull
      ConstraintCastNode* res_cast = clone()->as_ConstraintCast();
      res_cast->set_req(0, kit.control());
      res_cast->set_req(1, projs->resproj[0]);
      res_cast->set_type(cast_type->cast_to_ptr_type(TypePtr::NotNull));
      Node* ctl = kit.control(); // Control may get updated below
      res = ValueTypePtrNode::make_from_oop(*igvn, ctl, kit.merged_memory(), igvn->transform(res_cast));

      region->init_req(2, ctl);
      mem_phi->init_req(2, kit.reset_memory());
      io_phi->init_req(2, kit.i_o());
      res_phi->init_req(2, igvn->transform(res));
      ex_region->init_req(2, projs->catchall_catchproj);
      ex_mem_phi->init_req(2, projs->catchall_memproj);
      ex_io_phi->init_req(2, projs->catchall_ioproj);

      igvn->set_delay_transform(false);

      // Re-attach users to newly created PhiNodes
      igvn->replace_node(ctl_hook, igvn->transform(region));
      igvn->replace_node(mem_hook, igvn->transform(mem_phi));
      igvn->replace_node(io_hook, igvn->transform(io_phi));
      igvn->replace_node(res_hook, igvn->transform(res_phi));
      igvn->replace_node(ex_ctl_hook, igvn->transform(ex_region));
      igvn->replace_node(ex_mem_hook, igvn->transform(ex_mem_phi));
      igvn->replace_node(ex_io_hook, igvn->transform(ex_io_phi));
      return this;
    } else {
      CallNode* call = in(1)->in(0)->as_Call();
      // We now know the return type of the call
      const TypeTuple* range = TypeTuple::make_range(vk, false);
      if (range != call->_tf->range_sig()) {
        // Build the ValueTypePtrNode by loading the fields
        call->_tf = TypeFunc::make(call->_tf->domain_sig(), call->_tf->domain_cc(),
                                   range, range);
        phase->set_type(call, call->Value(phase));
        phase->set_type(in(1), in(1)->Value(phase));
        uint last = phase->C->unique();
        CallNode* call = in(1)->in(0)->as_Call();
        // Extract projections from the call and hook control users to temporary node
        CallProjections* projs = call->extract_projections(true, true);
        Node* ctl = projs->fallthrough_catchproj;
        Node* mem = projs->fallthrough_memproj;
        Node* ctl_hook = new Node(1);
        igvn->replace_in_uses(ctl, ctl_hook);
        Node* vtptr = ValueTypePtrNode::make_from_oop(*phase, ctl, mem, in(1));
        // Attach users to updated control
        igvn->replace_node(ctl_hook, ctl);
        return vtptr;
      }
    }
  }
  return NULL;
}

//=============================================================================
//------------------------------Value------------------------------------------
const Type* CastX2PNode::Value(PhaseGVN* phase) const {
  const Type* t = phase->type(in(1));
  if (t == Type::TOP) return Type::TOP;
  if (t->base() == Type_X && t->singleton()) {
    uintptr_t bits = (uintptr_t) t->is_intptr_t()->get_con();
    if (bits == 0)   return TypePtr::NULL_PTR;
    return TypeRawPtr::make((address) bits);
  }
  return CastX2PNode::bottom_type();
}

//------------------------------Idealize---------------------------------------
static inline bool fits_in_int(const Type* t, bool but_not_min_int = false) {
  if (t == Type::TOP)  return false;
  const TypeX* tl = t->is_intptr_t();
  jint lo = min_jint;
  jint hi = max_jint;
  if (but_not_min_int)  ++lo;  // caller wants to negate the value w/o overflow
  return (tl->_lo >= lo) && (tl->_hi <= hi);
}

static inline Node* addP_of_X2P(PhaseGVN *phase,
                                Node* base,
                                Node* dispX,
                                bool negate = false) {
  if (negate) {
    dispX = new SubXNode(phase->MakeConX(0), phase->transform(dispX));
  }
  return new AddPNode(phase->C->top(),
                      phase->transform(new CastX2PNode(base)),
                      phase->transform(dispX));
}

Node *CastX2PNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  // convert CastX2P(AddX(x, y)) to AddP(CastX2P(x), y) if y fits in an int
  int op = in(1)->Opcode();
  Node* x;
  Node* y;
  switch (op) {
    case Op_SubX:
    x = in(1)->in(1);
    // Avoid ideal transformations ping-pong between this and AddP for raw pointers.
    if (phase->find_intptr_t_con(x, -1) == 0)
    break;
    y = in(1)->in(2);
    if (fits_in_int(phase->type(y), true)) {
      return addP_of_X2P(phase, x, y, true);
    }
    break;
    case Op_AddX:
    x = in(1)->in(1);
    y = in(1)->in(2);
    if (fits_in_int(phase->type(y))) {
      return addP_of_X2P(phase, x, y);
    }
    if (fits_in_int(phase->type(x))) {
      return addP_of_X2P(phase, y, x);
    }
    break;
  }
  return NULL;
}

//------------------------------Identity---------------------------------------
Node* CastX2PNode::Identity(PhaseGVN* phase) {
  if (in(1)->Opcode() == Op_CastP2X)  return in(1)->in(1);
  return this;
}

//=============================================================================
//------------------------------Value------------------------------------------
const Type* CastP2XNode::Value(PhaseGVN* phase) const {
  const Type* t = phase->type(in(1));
  if (t == Type::TOP) return Type::TOP;
  if (t->base() == Type::RawPtr && t->singleton()) {
    uintptr_t bits = (uintptr_t) t->is_rawptr()->get_con();
    return TypeX::make(bits);
  }
  return CastP2XNode::bottom_type();
}

Node *CastP2XNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  return (in(0) && remove_dead_region(phase, can_reshape)) ? this : NULL;
}

//------------------------------Identity---------------------------------------
Node* CastP2XNode::Identity(PhaseGVN* phase) {
  if (in(1)->Opcode() == Op_CastX2P)  return in(1)->in(1);
  return this;
}
