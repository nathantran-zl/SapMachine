/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_OOPSTORAGEPARSTATE_INLINE_HPP
#define SHARE_GC_SHARED_OOPSTORAGEPARSTATE_INLINE_HPP

#include "gc/shared/oopStorage.inline.hpp"
#include "memory/allocation.hpp"
#include "metaprogramming/conditional.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_ALL_GCS

//////////////////////////////////////////////////////////////////////////////
// Support for parallel and optionally concurrent state iteration.
//
// Parallel iteration is for the exclusive use of the GC.  Other iteration
// clients must use serial iteration.
//
// Concurrent Iteration
//
// Iteration involves the _active_list, which contains all of the blocks owned
// by a storage object.  This is a doubly-linked list, linked through
// dedicated fields in the blocks.
//
// At most one concurrent ParState can exist at a time for a given storage
// object.
//
// A concurrent ParState sets the associated storage's
// _concurrent_iteration_active flag true when the state is constructed, and
// sets it false when the state is destroyed.  These assignments are made with
// _active_mutex locked.  Meanwhile, empty block deletion is not done while
// _concurrent_iteration_active is true.  The flag check and the dependent
// removal of a block from the _active_list is performed with _active_mutex
// locked.  This prevents concurrent iteration and empty block deletion from
// interfering with with each other.
//
// Both allocate() and delete_empty_blocks_concurrent() lock the
// _allocate_mutex while performing their respective list manipulations,
// preventing them from interfering with each other.
//
// When allocate() creates a new block, it is added to the front of the
// _active_list.  Then _active_head is set to the new block.  When concurrent
// iteration is started (by a parallel worker thread calling the state's
// iterate() function), the current _active_head is used as the initial block
// for the iteration, with iteration proceeding down the list headed by that
// block.
//
// As a result, the list over which concurrent iteration operates is stable.
// However, once the iteration is started, later allocations may add blocks to
// the front of the list that won't be examined by the iteration.  And while
// the list is stable, concurrent allocate() and release() operations may
// change the set of allocated entries in a block at any time during the
// iteration.
//
// As a result, a concurrent iteration handler must accept that some
// allocations and releases that occur after the iteration started will not be
// seen by the iteration.  Further, some may overlap examination by the
// iteration.  To help with this, allocate() and release() have an invariant
// that an entry's value must be NULL when it is not in use.
//
// An in-progress delete_empty_blocks_concurrent() operation can contend with
// the start of a concurrent iteration over the _active_mutex.  Since both are
// under GC control, that potential contention can be eliminated by never
// scheduling both operations to run at the same time.
//
// ParState<concurrent, is_const>
//   concurrent must be true if iteration is concurrent with the
//   mutator, false if iteration is at a safepoint.
//
//   is_const must be true if the iteration is over a constant storage
//   object, false if the iteration may modify the storage object.
//
// ParState([const] OopStorage* storage)
//   Construct an object for managing an iteration over storage.  For a
//   concurrent ParState, empty block deletion for the associated storage
//   is inhibited for the life of the ParState.  There can be no more
//   than one live concurrent ParState at a time for a given storage object.
//
// template<typename F> void iterate(F f)
//   Repeatedly claims a block from the associated storage that has
//   not been processed by this iteration (possibly by other threads),
//   and applies f to each entry in the claimed block. Assume p is of
//   type const oop* or oop*, according to is_const. Then f(p) must be
//   a valid expression whose value is ignored.  Concurrent uses must
//   be prepared for an entry's value to change at any time, due to
//   mutator activity.
//
// template<typename Closure> void oops_do(Closure* cl)
//   Wrapper around iterate, providing an adaptation layer allowing
//   the use of OopClosures and similar objects for iteration.  Assume
//   p is of type const oop* or oop*, according to is_const.  Then
//   cl->do_oop(p) must be a valid expression whose value is ignored.
//   Concurrent uses must be prepared for the entry's value to change
//   at any time, due to mutator activity.
//
// Optional operations, provided only if !concurrent && !is_const.
// These are not provided when is_const, because the storage object
// may be modified by the iteration infrastructure, even if the
// provided closure doesn't modify the storage object.  These are not
// provided when concurrent because any pre-filtering behavior by the
// iteration infrastructure is inappropriate for concurrent iteration;
// modifications of the storage by the mutator could result in the
// pre-filtering being applied (successfully or not) to objects that
// are unrelated to what the closure finds in the entry.
//
// template<typename Closure> void weak_oops_do(Closure* cl)
// template<typename IsAliveClosure, typename Closure>
// void weak_oops_do(IsAliveClosure* is_alive, Closure* cl)
//   Wrappers around iterate, providing an adaptation layer allowing
//   the use of is-alive closures and OopClosures for iteration.
//   Assume p is of type oop*.  Then
//
//   - cl->do_oop(p) must be a valid expression whose value is ignored.
//
//   - is_alive->do_object_b(*p) must be a valid expression whose value
//   is convertible to bool.
//
//   If *p == NULL then neither is_alive nor cl will be invoked for p.
//   If is_alive->do_object_b(*p) is false, then cl will not be
//   invoked on p.

class OopStorage::BasicParState VALUE_OBJ_CLASS_SPEC {
  OopStorage* _storage;
  void* volatile _next_block;
  bool _concurrent;

  // Noncopyable.
  BasicParState(const BasicParState&);
  BasicParState& operator=(const BasicParState&);

  void update_iteration_state(bool value);
  void ensure_iteration_started();
  Block* claim_next_block();

  // Wrapper for iteration handler; ignore handler result and return true.
  template<typename F> class AlwaysTrueFn;

public:
  BasicParState(OopStorage* storage, bool concurrent);
  ~BasicParState();

  template<bool is_const, typename F> void iterate(F f) {
    // Wrap f in ATF so we can use Block::iterate.
    AlwaysTrueFn<F> atf_f(f);
    ensure_iteration_started();
    typename Conditional<is_const, const Block*, Block*>::type block;
    while ((block = claim_next_block()) != NULL) {
      block->iterate(atf_f);
    }
  }
};

template<typename F>
class OopStorage::BasicParState::AlwaysTrueFn VALUE_OBJ_CLASS_SPEC {
  F _f;

public:
  AlwaysTrueFn(F f) : _f(f) {}

  template<typename OopPtr>     // [const] oop*
  bool operator()(OopPtr ptr) const { _f(ptr); return true; }
};

template<bool concurrent, bool is_const>
class OopStorage::ParState VALUE_OBJ_CLASS_SPEC {
  BasicParState _basic_state;

public:
  ParState(const OopStorage* storage) :
    // For simplicity, always recorded as non-const.
    _basic_state(const_cast<OopStorage*>(storage), concurrent)
  {}

  template<typename F>
  void iterate(F f) {
    _basic_state.template iterate<is_const>(f);
  }

  template<typename Closure>
  void oops_do(Closure* cl) {
    this->iterate(oop_fn(cl));
  }
};

template<>
class OopStorage::ParState<false, false> VALUE_OBJ_CLASS_SPEC {
  BasicParState _basic_state;

public:
  ParState(OopStorage* storage) :
    _basic_state(storage, false)
  {}

  template<typename F>
  void iterate(F f) {
    _basic_state.template iterate<false>(f);
  }

  template<typename Closure>
  void oops_do(Closure* cl) {
    this->iterate(oop_fn(cl));
  }

  template<typename Closure>
  void weak_oops_do(Closure* cl) {
    this->iterate(skip_null_fn(oop_fn(cl)));
  }

  template<typename IsAliveClosure, typename Closure>
  void weak_oops_do(IsAliveClosure* is_alive, Closure* cl) {
    this->iterate(if_alive_fn(is_alive, oop_fn(cl)));
  }
};

#endif // INCLUDE_ALL_GCS

#endif // include guard
