/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12GraphicsCommandList{,1..10} overrides.
 *
 * Close is async.  The generated default is force_sync purely as a
 * cross-ring barrier: a list recorded on thread A's TLS ring may be
 * ExecuteCommandLists'd from thread B's ring, and the host must have
 * decoded the recording (including Close) before it decodes the
 * submission.  A sync Close stalls every recording thread once per
 * list per frame, which serializes exactly the parallel recording
 * D3D12 is built around.
 *
 * Instead Close encodes async and stamps {ring id, ring seqno} into
 * the list's aux; the ECL override waits each listed ring forward to
 * its stamped seqno (npt_tls_wait_ring_seqno).  By ECL time the host
 * has almost always consumed the recording already, so the wait is a
 * single atomic load instead of a blocking round-trip per Close.
 */

#include <stdatomic.h>
#include <stdlib.h>

#include "npt_com.h"
#include "npt_device.h"
#include "npt_overrides.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_tls.h"

/* Close reports device removal when its recording could not reach the
 * host; a genuine host-side recording error still surfaces as a decoder
 * fatal (async model). */
#define NPT_DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)

#include "neptune-protocol/npt_protocol_client_id3d12graphicscommandlist.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d12graphicscommandlist.h"

#define NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist1, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist2, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist3, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist4, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist5, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist6, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist7, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist8, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist9, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist10, m, f)

static const GUID *const list12_tiers[] = {
   &NPT_IID_ID3D12GraphicsCommandList,
   &NPT_IID_ID3D12GraphicsCommandList1,
   &NPT_IID_ID3D12GraphicsCommandList2,
   &NPT_IID_ID3D12GraphicsCommandList3,
   &NPT_IID_ID3D12GraphicsCommandList4,
   &NPT_IID_ID3D12GraphicsCommandList5,
   &NPT_IID_ID3D12GraphicsCommandList6,
   &NPT_IID_ID3D12GraphicsCommandList7,
   &NPT_IID_ID3D12GraphicsCommandList8,
   &NPT_IID_ID3D12GraphicsCommandList9,
   &NPT_IID_ID3D12GraphicsCommandList10,
   NULL,
};

struct npt_d3d12_list_aux {
   /* Stamped by Close, read by the ECL override.  close_pending's
    * release store publishes the two plain fields; Close/ECL are
    * app-ordered per list, so a racing Close+ECL on one list is
    * already invalid API use. */
   uint64_t close_ring_id;
   uint32_t close_seqno;
   _Atomic bool close_pending;
   /* Stamped by the ECL override, read by Reset.  D3D12 lets an app
    * Reset a list immediately after submitting it; the Reset rides
    * this thread's TLS ring and can overtake the queue-ring Execute,
    * which the host then rejects ("Command list ... is in recording
    * state") and marks the device removed.  Reset waits the Execute's ring
    * forward to the stamped seqno first. */
   uint64_t exec_ring_id;
   uint32_t exec_seqno;
   _Atomic bool exec_pending;
};

static void npt_d3d12_list_aux_destroy(void *aux_raw);

static void
npt_d3d12_list_aux_init(struct npt_com_base *com,
                        struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_list_aux *aux = com->aux;
   aux->close_ring_id = 0;
   aux->close_seqno = 0;
   atomic_store_explicit(&aux->close_pending, false, memory_order_relaxed);
   aux->exec_ring_id = 0;
   aux->exec_seqno = 0;
   atomic_store_explicit(&aux->exec_pending, false, memory_order_relaxed);
   com->aux_destroy = npt_d3d12_list_aux_destroy;
   /* D3D12 lists record single-threaded but may legally migrate threads
    * between calls; order their wire traffic across rings (npt_object.h
    * ring_ordered / npt_com_self_ring). */
   com->base.ring_ordered = true;
   (void)dev; (void)host_id;
}

static void
npt_d3d12_list_aux_destroy(void *aux_raw)
{
   free(aux_raw);
}

/* NULL on wrappers outside the graphics-command-list family; QI tier
 * aliases (a higher GraphicsCommandList tier) resolve to the primary's
 * aux. */
static struct npt_d3d12_list_aux *
list12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_list_aux_destroy);
}

bool
npt_d3d12_list_close_barrier(void *list_wrapper, uint64_t *out_ring_id,
                             uint32_t *out_seqno)
{
   struct npt_d3d12_list_aux *aux = list12_aux(list_wrapper);
   if (!aux ||
       !atomic_load_explicit(&aux->close_pending, memory_order_acquire))
      return false;
   *out_ring_id = aux->close_ring_id;
   *out_seqno = aux->close_seqno;
   return true;
}

bool
npt_d3d12_list_stamp_execute(void *list_wrapper, uint64_t ring_id,
                             uint32_t seqno)
{
   struct npt_d3d12_list_aux *aux = list12_aux(list_wrapper);
   if (!aux)
      return false;
   aux->exec_ring_id = ring_id;
   aux->exec_seqno = seqno;
   atomic_store_explicit(&aux->exec_pending, true, memory_order_release);
   return true;
}

static HRESULT NPT_STDMETHODCALLTYPE
list12_Reset_override(void *self, ID3D12CommandAllocator *pAllocator,
                      ID3D12PipelineState *pInitialState)
{
   struct npt_d3d12_list_aux *aux = list12_aux(self);
   if (aux &&
       atomic_load_explicit(&aux->exec_pending, memory_order_acquire)) {
      struct npt_ring *my_ring = npt_com_self_ring(self);
      if (!my_ring || my_ring->id != aux->exec_ring_id)
         npt_tls_wait_ring_seqno(npt_com_self_device(self),
                                 aux->exec_ring_id, aux->exec_seqno);
      atomic_store_explicit(&aux->exec_pending, false, memory_order_relaxed);
   }
   if (aux)
      atomic_store_explicit(&aux->close_pending, false, memory_order_relaxed);
   return npt_id3d12graphicscommandlist_default_Reset(self, pAllocator,
                                                      pInitialState);
}

static HRESULT NPT_STDMETHODCALLTYPE
list12_Close_override(void *self)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   struct npt_ring_submit_command submit;
   npt_submit_ID3D12GraphicsCommandList_Close(ring, 0, npt_com_self_id(self),
                                              &submit);

   /* A lost ring dropped the recording on the floor; the app must not
    * ExecuteCommandLists a list the host never saw closed.  MSDN:
    * ID3D12GraphicsCommandList::Close returns the recording error and
    * the list must not be submitted when it fails. */
   if (ring->renderer && npt_renderer_is_lost(ring->renderer))
      return NPT_DXGI_ERROR_DEVICE_REMOVED;

   struct npt_d3d12_list_aux *aux = list12_aux(self);
   if (aux) {
      aux->close_ring_id = ring->id;
      aux->close_seqno = submit.seqno;
      atomic_store_explicit(&aux->close_pending, true, memory_order_release);
   } else {
      /* No aux to stamp: keep the old barrier semantics by draining
       * this ring before returning. */
      npt_ring_wait_all(ring);
   }
   return NPT_S_OK;
}

void
npt_overrides_d3d12_list_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(Close, list12_Close_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(Reset, list12_Reset_override);
   npt_com_register_family(list12_tiers,
                           sizeof(struct npt_d3d12_list_aux),
                           npt_d3d12_list_aux_init);
}
