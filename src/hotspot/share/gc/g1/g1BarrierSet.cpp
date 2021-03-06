/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/g1/satbMarkQueue.hpp"
#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/macros.hpp"

G1BarrierSet::G1BarrierSet(G1CardTable* card_table) :
  CardTableBarrierSet(make_barrier_set_assembler<G1BarrierSetAssembler>(),
                      card_table,
                      BarrierSet::FakeRtti(BarrierSet::G1BarrierSet)),
  _dcqs(JavaThread::dirty_card_queue_set())
{ }

void G1BarrierSet::enqueue(oop pre_val) {
  // Nulls should have been already filtered.
  assert(oopDesc::is_oop(pre_val, true), "Error");

  if (!JavaThread::satb_mark_queue_set().is_active()) return;
  Thread* thr = Thread::current();
  if (thr->is_Java_thread()) {
    JavaThread* jt = (JavaThread*)thr;
    jt->satb_mark_queue().enqueue(pre_val);
  } else {
    MutexLockerEx x(Shared_SATB_Q_lock, Mutex::_no_safepoint_check_flag);
    JavaThread::satb_mark_queue_set().shared_satb_queue()->enqueue(pre_val);
  }
}

void G1BarrierSet::write_ref_array_pre_oop_entry(oop* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

void G1BarrierSet::write_ref_array_pre_narrow_oop_entry(narrowOop* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

void G1BarrierSet::write_ref_array_post_entry(HeapWord* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->G1BarrierSet::write_ref_array(dst, length);
}

template <class T> void
G1BarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  if (!JavaThread::satb_mark_queue_set().is_active()) return;
  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      enqueue(CompressedOops::decode_not_null(heap_oop));
    }
  }
}

void G1BarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_field_post_slow(volatile jbyte* byte) {
  // In the slow path, we know a card is not young
  assert(*byte != G1CardTable::g1_young_card_val(), "slow path invoked without filtering");
  OrderAccess::storeload();
  if (*byte != G1CardTable::dirty_card_val()) {
    *byte = G1CardTable::dirty_card_val();
    Thread* thr = Thread::current();
    if (thr->is_Java_thread()) {
      JavaThread* jt = (JavaThread*)thr;
      jt->dirty_card_queue().enqueue(byte);
    } else {
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      _dcqs.shared_dirty_card_queue()->enqueue(byte);
    }
  }
}

void G1BarrierSet::invalidate(MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }
  volatile jbyte* byte = _card_table->byte_for(mr.start());
  jbyte* last_byte = _card_table->byte_for(mr.last());
  Thread* thr = Thread::current();
    // skip all consecutive young cards
  for (; byte <= last_byte && *byte == G1CardTable::g1_young_card_val(); byte++);

  if (byte <= last_byte) {
    OrderAccess::storeload();
    // Enqueue if necessary.
    if (thr->is_Java_thread()) {
      JavaThread* jt = (JavaThread*)thr;
      for (; byte <= last_byte; byte++) {
        if (*byte == G1CardTable::g1_young_card_val()) {
          continue;
        }
        if (*byte != G1CardTable::dirty_card_val()) {
          *byte = G1CardTable::dirty_card_val();
          jt->dirty_card_queue().enqueue(byte);
        }
      }
    } else {
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      for (; byte <= last_byte; byte++) {
        if (*byte == G1CardTable::g1_young_card_val()) {
          continue;
        }
        if (*byte != G1CardTable::dirty_card_val()) {
          *byte = G1CardTable::dirty_card_val();
          _dcqs.shared_dirty_card_queue()->enqueue(byte);
        }
      }
    }
  }
}

void G1BarrierSet::on_thread_attach(JavaThread* thread) {
  // This method initializes the SATB and dirty card queues before a
  // JavaThread is added to the Java thread list. Right now, we don't
  // have to do anything to the dirty card queue (it should have been
  // activated when the thread was created), but we have to activate
  // the SATB queue if the thread is created while a marking cycle is
  // in progress. The activation / de-activation of the SATB queues at
  // the beginning / end of a marking cycle is done during safepoints
  // so we have to make sure this method is called outside one to be
  // able to safely read the active field of the SATB queue set. Right
  // now, it is called just before the thread is added to the Java
  // thread list in the Threads::add() method. That method is holding
  // the Threads_lock which ensures we are outside a safepoint. We
  // cannot do the obvious and set the active field of the SATB queue
  // when the thread is created given that, in some cases, safepoints
  // might happen between the JavaThread constructor being called and the
  // thread being added to the Java thread list (an example of this is
  // when the structure for the DestroyJavaVM thread is created).
  assert(!SafepointSynchronize::is_at_safepoint(), "We should not be at a safepoint");
  assert(!thread->satb_mark_queue().is_active(), "SATB queue should not be active");
  assert(thread->satb_mark_queue().is_empty(), "SATB queue should be empty");
  assert(thread->dirty_card_queue().is_active(), "Dirty card queue should be active");

  // If we are creating the thread during a marking cycle, we should
  // set the active field of the SATB queue to true.
  if (thread->satb_mark_queue_set().is_active()) {
    thread->satb_mark_queue().set_active(true);
  }
}

void G1BarrierSet::on_thread_detach(JavaThread* thread) {
  // Flush any deferred card marks, SATB buffers and dirty card queue buffers
  CardTableBarrierSet::on_thread_detach(thread);
  thread->satb_mark_queue().flush();
  thread->dirty_card_queue().flush();
}
