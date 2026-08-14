/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D10Blob::GetBufferPointer override.
 *
 * The real ABI returns LPVOID -- a pointer into the HOST process's
 * address space that the guest can never dereference. The generated
 * client thunk correctly couldn't marshal that generically (see
 * npt_protocol_guest_id3d10blob.h and npt_protocol_host_id3d10blob.h
 * for the wire-level half of this fix) and was left as a stub that
 * discarded the real return value entirely -- every caller (root
 * signature blobs from D3D12SerializeRootSignature, shader compile
 * output, error blobs) got back whatever garbage was left in the
 * return register. Downstream code then handed that garbage to real
 * D3D12/D3D11 calls (e.g. ID3D12Device::CreateRootSignature) as if it
 * were a valid pointer+length, which is what made vkd3d-proton's own
 * root signature deserializer hang trying to walk it.
 *
 * Fix: fetch the blob's real byte size (GetBufferSize already worked
 * correctly -- it's a plain SIZE_T scalar, no pointer-marshalling
 * problem) then do ONE proper sync round trip that copies the actual
 * bytes into a guest-owned buffer, cached on the wrapper's aux for the
 * object's lifetime so repeat calls -- and D3D contract requires
 * GetBufferPointer to return a stable pointer across calls -- don't
 * re-fetch.
 */

#include <stdlib.h>

#include "npt_com.h"
#include "npt_device.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d10blob.h"
#include "neptune-protocol/npt_protocol_defs.h"

static const GUID *const id3d10blob_tiers[] = {
   &NPT_IID_ID3D10Blob, NULL,
};

struct npt_d3d10blob_aux {
   void *cached_ptr;
   SIZE_T cached_size;
   bool fetched;
};

static void
npt_d3d10blob_aux_destroy(void *aux_raw)
{
   struct npt_d3d10blob_aux *aux = aux_raw;
   free(aux->cached_ptr);
   free(aux);
}

static void
npt_d3d10blob_aux_init(struct npt_com_base *com,
                       struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d10blob_aux *aux = com->aux;
   com->aux_destroy = npt_d3d10blob_aux_destroy;
   (void)aux; (void)dev; (void)host_id;
}

static inline struct npt_d3d10blob_aux *
blob_aux(void *self)
{
   struct npt_com_base *com = self;
   if (!com || com->aux_destroy != npt_d3d10blob_aux_destroy)
      return NULL;
   return com->aux;
}

static void * NPT_STDMETHODCALLTYPE
blob_GetBufferPointer_override(void *self)
{
   struct npt_d3d10blob_aux *aux = blob_aux(self);
   if (!aux) {
      /* No aux (shouldn't happen -- family is always registered below)
       * -- fall back to the safe do-nothing default rather than a
       * fresh, uncached, per-call leak. */
      return npt_id3d10blob_default_GetBufferPointer(self);
   }
   if (aux->fetched)
      return aux->cached_ptr;

   struct npt_ring *ring = npt_com_self_ring(self);
   npt_object_id id = npt_com_self_id(self);

   SIZE_T size = npt_call_ID3D10Blob_GetBufferSize(ring, id);
   void *buf = NULL;
   if (size) {
      buf = malloc(size);
      if (!buf) {
         /* Leave unfetched -- OOM is transient-ish and a future call
          * might succeed; returning NULL here without caching lets a
          * retry actually retry instead of permanently caching NULL. */
         return NULL;
      }
      size_t actual = 0;
      if (!npt_call_ID3D10Blob_GetBufferPointer(ring, id, buf, size, &actual)) {
         free(buf);
         return NULL;
      }
      if (actual != size) {
         npt_log("ID3D10Blob::GetBufferPointer: host byte count %zu != "
                 "GetBufferSize %zu (using the smaller)",
                 actual, (size_t)size);
      }
   }

   aux->cached_ptr = buf;
   aux->cached_size = size;
   aux->fetched = true;
   return aux->cached_ptr;
}

void
npt_overrides_d3d10_blob_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d10blob, GetBufferPointer,
                         blob_GetBufferPointer_override);
   npt_com_register_family(id3d10blob_tiers,
                           sizeof(struct npt_d3d10blob_aux),
                           npt_d3d10blob_aux_init);
}
