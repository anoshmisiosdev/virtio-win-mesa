/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device guest-side overrides.
 *
 * CheckFeatureSupport: the (pFeatureSupportData, FeatureSupportDataSize)
 * payload is marshalled generically as an opaque byte blob, which is
 * correct for the vast majority of D3D12_FEATURE_DATA_* structs because
 * they are flat.  Three are not:
 *
 *   - D3D12_FEATURE_DATA_FEATURE_LEVELS::pFeatureLevelsRequested
 *   - D3D12_FEATURE_DATA_QUERY_META_COMMAND::pQueryInputData / pQueryOutputData
 *   - D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES::pTypes
 *
 * Copying the struct's own bytes verbatim carries those pointer fields
 * across as raw 64-bit values, but a pointer only means anything in the
 * address space it was written in.  For D3D12_FEATURE_FEATURE_LEVELS the
 * host's generic dispatch hands the blob straight to the real
 * vkd3d-proton CheckFeatureSupport, which dereferences
 * pFeatureLevelsRequested to read the caller's array -- a *guest* address,
 * meaningless in the render_server process.  The worker segfaults inside
 * vkd3d-proton and never writes a reply, so from the guest's point of view
 * the call simply never returns: a hang, not a crash.
 *
 * Fix: repack the request into a self-contained wire blob -- a small
 * header (NumFeatureLevels + MaxSupportedFeatureLevel) followed by the
 * requested D3D_FEATURE_LEVEL array *inline*, containing no pointer at
 * all.  The matching host override
 * (npt_overrides_id3d12device_checkfeaturesupport.c in virglrenderer)
 * unpacks it, points a real D3D12_FEATURE_DATA_FEATURE_LEVELS at the
 * array embedded in that same host-local buffer, calls the real entry
 * point, and writes MaxSupportedFeatureLevel back into the blob header --
 * which the existing generic reply-encode path returns unmodified.
 *
 * QUERY_META_COMMAND and PROTECTED_RESOURCE_SESSION_TYPES are rejected
 * host-side with E_INVALIDARG before reaching the real call, closing the
 * same crash class without pretending they are implemented.  They pass
 * through the untouched generic thunk here.
 */

#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_device.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_guest_id3d12device.h"

#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device, m, f)

/* Must match the host-side layout exactly -- see
 * npt_overrides_id3d12device_checkfeaturesupport.c (virglrenderer)
 * struct npt_wire_feature_levels_hdr. */
struct npt_wire_feature_levels_hdr {
   UINT NumFeatureLevels;
   D3D_FEATURE_LEVEL MaxSupportedFeatureLevel;
   /* D3D_FEATURE_LEVEL levels[NumFeatureLevels] follows inline */
};

static HRESULT NPT_STDMETHODCALLTYPE
dev_CheckFeatureSupport_override(void *self, D3D12_FEATURE Feature,
                                 void *pFeatureSupportData,
                                 UINT FeatureSupportDataSize)
{
   if (Feature != D3D12_FEATURE_FEATURE_LEVELS) {
      /* Flat structs (the common case) and the two rejected
       * pointer-bearing ones (host fails those cleanly) both go
       * through the untouched generic thunk. */
      return npt_id3d12device_default_CheckFeatureSupport(
         self, Feature, pFeatureSupportData, FeatureSupportDataSize);
   }

   if (!pFeatureSupportData ||
       FeatureSupportDataSize < sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
      return NPT_E_INVALIDARG;

   D3D12_FEATURE_DATA_FEATURE_LEVELS *levels = pFeatureSupportData;
   const UINT n = levels->NumFeatureLevels;
   if (n == 0 || !levels->pFeatureLevelsRequested)
      return NPT_E_INVALIDARG;

   const size_t wire_size = sizeof(struct npt_wire_feature_levels_hdr) +
                            (size_t)n * sizeof(D3D_FEATURE_LEVEL);
   struct npt_wire_feature_levels_hdr *wire = npt_alloc(wire_size);
   if (!wire)
      return NPT_E_OUTOFMEMORY;

   wire->NumFeatureLevels = n;
   wire->MaxSupportedFeatureLevel = 0;
   memcpy(wire + 1, levels->pFeatureLevelsRequested,
          (size_t)n * sizeof(D3D_FEATURE_LEVEL));

   struct npt_device *dev = npt_com_self_device(self);
   HRESULT hr = npt_call_ID3D12Device_CheckFeatureSupport(
      npt_device_method_ring(dev), npt_com_self_id(self),
      D3D12_FEATURE_FEATURE_LEVELS, wire, (UINT)wire_size);

   if (NPT_SUCCEEDED(hr))
      levels->MaxSupportedFeatureLevel = wire->MaxSupportedFeatureLevel;

   free(wire);
   return hr;
}

void
npt_overrides_d3d12_device_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CheckFeatureSupport,
                                      dev_CheckFeatureSupport_override);
}
