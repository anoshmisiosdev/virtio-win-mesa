/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Resource{,1,2} overrides: Map / Unmap / WriteToSubresource /
 * ReadFromSubresource are skip_default (the generated marshaling would
 * hand the app a garbage host pointer or drop unsized data).
 *
 * Mapping takes one of two paths.
 *
 * Persistent map: resources on shmem-backed heaps (npt_d3d12_heap.c --
 * UPLOAD/READBACK/CPU-CUSTOM buffers) carry {shmem_heap, heap_offset} in
 * their aux, so Map returns shmem_base + heap_offset with no wire
 * traffic at all (the host imported the same pages as the heap's device
 * memory) and Unmap is a local no-op.
 *
 * Sync map: everything else.  Map is a synchronous MAP_RESOURCE round
 * trip -- the host maps the resource and copies its current contents
 * into a per-resource shmem slot, the app writes the slot, and Unmap
 * copies back and unmaps.  Buffers only, and always READ|WRITE so the
 * slot holds the full real contents and a partial app write cannot
 * smear garbage into the bytes it did not touch.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_d3d12_heap.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_resource.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_client_id3d12resource.h"
#include "neptune-protocol/npt_protocol_guest_id3d12resource.h"
#include "neptune-protocol/npt_protocol_defs.h"

#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource2, m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE1(m, f)

static const GUID *const resource12_tiers[] = {
   &NPT_IID_ID3D12Resource, &NPT_IID_ID3D12Resource1,
   &NPT_IID_ID3D12Resource2, NULL,
};

struct npt_d3d12_resource_aux {
   struct npt_d3d_map_ring map_ring;

   /* From GetDesc, fetched once on first Map.  dimension < 0 means
    * unqueried. */
   uint64_t width;
   int32_t dimension;

   /* D3D12 allows nested Map; only the outermost pair round-trips. */
   uint32_t map_count;

   /* Persistent-map path.  shmem_heap is borrowed from hidden_heap's
    * aux; hidden_heap is an owned wrapper ref, kept so the heap -- and
    * with it the host import and guest blob -- outlives this resource.
    * A NULL shmem_heap selects the sync path. */
   struct npt_d3d12_shmem_heap *shmem_heap;
   struct npt_com_base *hidden_heap;
   uint64_t heap_offset;

   /* App's original heap properties, answered guest-side by
    * GetHeapProperties (the host heap is an external-memory import
    * whose properties don't round-trip). */
   bool has_app_heap;
   D3D12_HEAP_PROPERTIES app_heap_props;
   D3D12_HEAP_FLAGS app_heap_flags;

   /* GetGPUVirtualAddress is immutable per resource but a sync wire
    * round-trip; cache the first answer.  valid's release store
    * publishes gpu_va to racing readers. */
   uint64_t gpu_va;
   _Atomic bool gpu_va_valid;

   /* Sync-path persistent-map flush list (see npt_d3d12_sync_maps_flush).
    * self is the wrapper this aux belongs to; link/next guarded by
    * g_sync_map_lock. */
   void *self;
   struct npt_d3d12_resource_aux *flush_next;
   bool in_flush_list;
};

/* Where the host cannot import guest shmem as heap memory, every mapped
 * buffer takes the sync path, whose only write-back point is Unmap.  An
 * app that maps its upload buffers once and never unmaps them would
 * then never deliver vertex or constant data to the host, and every
 * frame renders from zeroed buffers.  Track outstanding sync-path maps
 * and flush them to the
 * host before every ExecuteCommandLists: unmap (host copies shadow ->
 * resource) + immediate WRITE-only remap (no READ prime, so a racing
 * app write through the still-valid slot pointer is never overwritten
 * with older host bytes). */
static SRWLOCK g_sync_map_lock = SRWLOCK_INIT;
static struct npt_d3d12_resource_aux *g_sync_map_head;
static _Atomic uint32_t g_sync_map_count;

static void
sync_map_list_add(struct npt_d3d12_resource_aux *aux, void *self)
{
   AcquireSRWLockExclusive(&g_sync_map_lock);
   if (!aux->in_flush_list) {
      aux->self = self;
      aux->flush_next = g_sync_map_head;
      g_sync_map_head = aux;
      aux->in_flush_list = true;
      atomic_fetch_add_explicit(&g_sync_map_count, 1, memory_order_relaxed);
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

static void
sync_map_list_remove(struct npt_d3d12_resource_aux *aux)
{
   AcquireSRWLockExclusive(&g_sync_map_lock);
   if (aux->in_flush_list) {
      struct npt_d3d12_resource_aux **pp = &g_sync_map_head;
      while (*pp && *pp != aux)
         pp = &(*pp)->flush_next;
      if (*pp)
         *pp = aux->flush_next;
      aux->in_flush_list = false;
      atomic_fetch_sub_explicit(&g_sync_map_count, 1, memory_order_relaxed);
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

void npt_d3d12_sync_maps_flush(struct npt_ring *queue_ring);

void
npt_d3d12_sync_maps_flush(struct npt_ring *queue_ring)
{
   if (!atomic_load_explicit(&g_sync_map_count, memory_order_relaxed))
      return;
   AcquireSRWLockExclusive(&g_sync_map_lock);
   for (struct npt_d3d12_resource_aux *aux = g_sync_map_head; aux;
        aux = aux->flush_next) {
      if (!aux->map_count || !aux->self)
         continue;
      /* Fire-and-forget on the QUEUE ring: ring FIFO orders the host
       * copy before the ExecuteCommandLists that follows on the same
       * ring, so no reply round trip is needed.  Waiting for one costs
       * hundreds of microseconds per buffer per Execute and dominates
       * submit-thread time.
       *
       * Known race, accepted: an app Unmap on another thread can reach
       * the host through its own ring between these two commands, after
       * which the remap leaves a stale host-side map entry until the
       * resource dies.  That requires unmapping a buffer the app is
       * concurrently submitting from. */
      const uint64_t id = ((struct npt_com_base *)aux->self)->base.id;
      npt_dispatch_resource_unmap12_async(
         queue_ring, id, 0,
         npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
         aux->width,
         npt_d3d_map_ring_slot_offset(&aux->map_ring, 0));
      npt_dispatch_resource_map12_async_write(
         queue_ring, id, 0,
         npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
         aux->width,
         npt_d3d_map_ring_slot_offset(&aux->map_ring, 0));
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

static void npt_d3d12_resource_aux_destroy(void *aux_raw);

static void
npt_d3d12_resource_aux_init(struct npt_com_base *com,
                            struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_resource_aux *aux = com->aux;
   npt_d3d_map_ring_init(&aux->map_ring, com);
   aux->width = 0;
   aux->dimension = -1;
   aux->map_count = 0;
   aux->shmem_heap = NULL;
   aux->hidden_heap = NULL;
   aux->heap_offset = 0;
   aux->has_app_heap = false;
   aux->gpu_va = 0;
   atomic_store_explicit(&aux->gpu_va_valid, false, memory_order_relaxed);
   aux->self = NULL;
   aux->flush_next = NULL;
   aux->in_flush_list = false;
   com->aux_destroy = npt_d3d12_resource_aux_destroy;
   (void)dev; (void)host_id;
}

static void
npt_d3d12_resource_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_resource_aux *aux = aux_raw;
   sync_map_list_remove(aux);
   npt_d3d_map_ring_fini(&aux->map_ring);
   /* COM_RELEASE for the resource was queued by npt_com_destroy before
    * this runs, so the host drops the placed resource before the heap:
    * the heap's release (possibly triggered here) then frees the
    * VkDeviceMemory import in the right order. */
   if (aux->hidden_heap)
      npt_com_default_release(aux->hidden_heap);
   free(aux);
}

/* NULL on wrappers outside the resource family (QI tier aliases
 * resolve to the primary's aux). */
static struct npt_d3d12_resource_aux *
res12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_resource_aux_destroy);
}

bool
npt_d3d12_resource_bind_shmem_heap(void *resource_wrapper,
                                   void *heap_wrapper,
                                   uint64_t heap_offset,
                                   uint64_t buffer_width,
                                   const D3D12_HEAP_PROPERTIES *app_props,
                                   D3D12_HEAP_FLAGS app_flags)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(resource_wrapper);
   struct npt_d3d12_heap_aux *haux = npt_d3d12_heap_aux_cast(heap_wrapper);
   if (!aux || !haux || !haux->shmem_heap)
      return false;
   if (aux->hidden_heap) {
      npt_log("bind_shmem_heap: resource already bound to a heap");
      return false;
   }

   npt_com_default_addref(heap_wrapper);
   aux->hidden_heap = heap_wrapper;

   if (app_props) {
      aux->has_app_heap = true;
      aux->app_heap_props = *app_props;
      aux->app_heap_flags = app_flags;
   }

   if (buffer_width > 0) {
      if (heap_offset + buffer_width <= haux->shmem_heap->size) {
         aux->shmem_heap = haux->shmem_heap;
         aux->heap_offset = heap_offset;
         aux->width = buffer_width;
         aux->dimension = (int32_t)D3D12_RESOURCE_DIMENSION_BUFFER;
      } else {
         npt_log("bind_shmem_heap: window [%" PRIu64 ", +%" PRIu64
                 ") exceeds heap size %" PRIu64 "; Map stays on the "
                 "sync path", heap_offset, buffer_width,
                 haux->shmem_heap->size);
      }
   }
   return true;
}

static bool
res12_ensure_desc(void *self, struct npt_d3d12_resource_aux *aux)
{
   if (aux->dimension >= 0)
      return true;
   D3D12_RESOURCE_DESC desc;
   memset(&desc, 0, sizeof(desc));
   if (!npt_id3d12resource_default_GetDesc(self, &desc))
      return false;
   aux->width = desc.Width;
   aux->dimension = (int32_t)desc.Dimension;
   return true;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_Map_override(void *self, UINT Subresource,
                   const D3D12_RANGE *pReadRange, void **ppData)
{
   (void)pReadRange;  /* Wire always maps READ|WRITE; see file header. */

   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux)
      return NPT_E_NOTIMPL;

   /* Persistent-map fast path: the host GPU aliases the shmem pages
    * (VK_EXT_external_memory_host heap import), so the mapping is
    * always live -- no wire traffic, persistent maps just work. */
   if (aux->shmem_heap) {
      if (Subresource != 0)
         return NPT_E_INVALIDARG;
      aux->map_count++;
      if (ppData)
         *ppData = (uint8_t *)npt_d3d12_shmem_heap_ptr(aux->shmem_heap) +
                   aux->heap_offset;
      return NPT_S_OK;
   }

   if (!res12_ensure_desc(self, aux))
      return NPT_E_FAIL;

   if (aux->dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      npt_log("ID3D12Resource::Map: only buffers supported on the sync "
              "map path (dimension %d)", aux->dimension);
      return NPT_E_NOTIMPL;
   }
   if (Subresource != 0)
      return NPT_E_INVALIDARG;
   if (!aux->width || aux->width > 0xffffffffu) {
      npt_log("ID3D12Resource::Map: unsupported buffer width %" PRIu64,
              aux->width);
      return NPT_E_FAIL;
   }

   if (aux->map_count > 0) {
      /* Nested map: same pointer, no round-trip. */
      aux->map_count++;
      if (ppData)
         *ppData = npt_d3d_map_ring_slot_ptr(&aux->map_ring, 0);
      return NPT_S_OK;
   }

   const uint32_t aligned = ((uint32_t)aux->width + 63u) & ~63u;
   if (!npt_d3d_map_ring_alloc_shmem(&aux->map_ring, aligned))
      return NPT_E_OUTOFMEMORY;

   struct npt_device *dev = npt_com_self_device(self);
   HRESULT hr = npt_dispatch_resource_map12(
      npt_device_method_ring(dev),
      ((struct npt_com_base *)self)->base.id,
      Subresource,
      NPT_MAP_ACCESS_READ | NPT_MAP_ACCESS_WRITE,
      npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
      aux->width,
      npt_d3d_map_ring_slot_offset(&aux->map_ring, 0),
      /*read_range=*/NPT_MAP_RANGE_NULL, NPT_MAP_RANGE_NULL);
   if (NPT_FAILED(hr))
      return hr;

   aux->map_count = 1;
   sync_map_list_add(aux, self);
   if (ppData)
      *ppData = npt_d3d_map_ring_slot_ptr(&aux->map_ring, 0);
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
res12_Unmap_override(void *self, UINT Subresource,
                     const D3D12_RANGE *pWrittenRange)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux || !aux->map_count) {
      npt_log("ID3D12Resource::Unmap: not mapped");
      return;
   }
   if (aux->shmem_heap) {
      /* Persistent-map fast path: local bookkeeping only. */
      aux->map_count--;
      return;
   }
   if (--aux->map_count > 0)
      return;
   sync_map_list_remove(aux);

   uint64_t written_begin = NPT_MAP_RANGE_NULL;
   uint64_t written_end = NPT_MAP_RANGE_NULL;
   if (pWrittenRange) {
      written_begin = pWrittenRange->Begin;
      written_end = pWrittenRange->End;
   }

   struct npt_device *dev = npt_com_self_device(self);
   HRESULT hr = npt_dispatch_resource_unmap12(
      npt_device_method_ring(dev),
      ((struct npt_com_base *)self)->base.id,
      Subresource,
      npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
      aux->width,
      npt_d3d_map_ring_slot_offset(&aux->map_ring, 0),
      written_begin, written_end);
   if (NPT_FAILED(hr))
      npt_log("ID3D12Resource::Unmap: host unmap failed 0x%08x",
              (unsigned)hr);
}

static D3D12_GPU_VIRTUAL_ADDRESS NPT_STDMETHODCALLTYPE
res12_GetGPUVirtualAddress_override(void *self)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux)
      return npt_id3d12resource_default_GetGPUVirtualAddress(self);
   if (atomic_load_explicit(&aux->gpu_va_valid, memory_order_acquire))
      return aux->gpu_va;
   D3D12_GPU_VIRTUAL_ADDRESS va =
      npt_id3d12resource_default_GetGPUVirtualAddress(self);
   /* Racing first calls store the same host answer; last-write-wins
    * is benign. */
   aux->gpu_va = va;
   atomic_store_explicit(&aux->gpu_va_valid, true, memory_order_release);
   return va;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_GetHeapProperties_override(void *self,
                                 D3D12_HEAP_PROPERTIES *pHeapProperties,
                                 D3D12_HEAP_FLAGS *pHeapFlags)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (aux && aux->has_app_heap) {
      if (pHeapProperties)
         *pHeapProperties = aux->app_heap_props;
      if (pHeapFlags)
         *pHeapFlags = aux->app_heap_flags;
      return NPT_S_OK;
   }
   return npt_id3d12resource_default_GetHeapProperties(self, pHeapProperties,
                                                       pHeapFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_WriteToSubresource_override(void *self, UINT DstSubresource,
                                  const D3D12_BOX *pDstBox,
                                  const void *pSrcData,
                                  UINT SrcRowPitch, UINT SrcDepthPitch)
{
   (void)self;
   (void)DstSubresource;
   (void)pDstBox;
   (void)pSrcData;
   (void)SrcRowPitch;
   (void)SrcDepthPitch;
   npt_log("ID3D12Resource::WriteToSubresource not implemented "
           "(CUSTOM-heap textures; see NEPTUNE_LIMITATIONS.md)");
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_ReadFromSubresource_override(void *self, void *pDstData,
                                   UINT DstRowPitch, UINT DstDepthPitch,
                                   UINT SrcSubresource,
                                   const D3D12_BOX *pSrcBox)
{
   (void)self;
   (void)pDstData;
   (void)DstRowPitch;
   (void)DstDepthPitch;
   (void)SrcSubresource;
   (void)pSrcBox;
   npt_log("ID3D12Resource::ReadFromSubresource not implemented "
           "(CUSTOM-heap textures; see NEPTUNE_LIMITATIONS.md)");
   return NPT_E_NOTIMPL;
}

void
npt_overrides_d3d12_resource_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(Map, res12_Map_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(Unmap, res12_Unmap_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(GetGPUVirtualAddress,
                                        res12_GetGPUVirtualAddress_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(GetHeapProperties,
                                        res12_GetHeapProperties_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(WriteToSubresource,
                                        res12_WriteToSubresource_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(ReadFromSubresource,
                                        res12_ReadFromSubresource_override);
   npt_com_register_family(resource12_tiers,
                           sizeof(struct npt_d3d12_resource_aux),
                           npt_d3d12_resource_aux_init);
}
