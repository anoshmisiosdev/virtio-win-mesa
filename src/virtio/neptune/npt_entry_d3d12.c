/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12.dll entry points: forward to the generated host call and wrap
 * the returned host pointers.
 *
 * The D3D12 protocol layer (npt_protocol_*_id3d12*) and the 73 D3D12
 * constructors in the client ctor table already exist; this file is the
 * guest-side public API surface that was missing.  It mirrors
 * npt_entry_dxgi.c, which has the same (riid, void **) shape.
 *
 * Unlike npt_entry_d3d11.c -- whose exports are reached through the WDDM
 * DDI via OpenAdapter10_2 -- these are the *API* entry points, so they are
 * exported from a separate d3d12.dll that an application loads in place of
 * the system one (the same deployment model vkd3d-proton uses on Windows).
 */

#include "npt_com.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_guest_toplevel.h"
#include "neptune-protocol/npt_protocol_defs.h"

/*
 * Shared tail for the "returns one COM object" entry points.  Failure
 * paths release both the singleton acquire and the host ref on `raw` so
 * the use-count stays balanced, exactly as finish_create_dxgi_factory()
 * does.
 */
static HRESULT
finish_wrap_object(struct npt_device *dev, const GUID *iid, void *raw,
                   HRESULT call_hr, void **ppOut)
{
   if (NPT_FAILED(call_hr) || !raw) {
      if (raw)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw);
      npt_device_release();
      return NPT_FAILED(call_hr) ? call_hr : NPT_E_FAIL;
   }

   void *wrapper = npt_com_get_or_wrap_or_release(
      dev, iid, (uint64_t)(uintptr_t)raw, NULL);
   if (!wrapper) {
      npt_device_release();
      return NPT_E_OUTOFMEMORY;
   }
   /* Top-level: device_ref_holds carries the acquire onto the wrapper. */
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);

   *ppOut = wrapper;
   return NPT_S_OK;
}

HRESULT NPT_API
D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                  REFIID riid, void **ppDevice)
{
   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_D3D12CreateDevice(dev->ring, pAdapter,
                                           MinimumFeatureLevel, riid, &raw);

   /*
    * ppDevice == NULL is the documented capability probe: the runtime asks
    * "would this succeed?" and expects S_FALSE with no object retained.
    * The host still creates one, so release it rather than leak it.
    */
   if (!ppDevice) {
      if (raw)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw);
      npt_device_release();
      return NPT_FAILED(hr) ? hr : NPT_S_FALSE;
   }

   *ppDevice = NULL;
   /* Wrap as base ID3D12Device regardless of riid; QI handles the tiers. */
   return finish_wrap_object(dev, &NPT_IID_ID3D12Device, raw, hr, ppDevice);
}

HRESULT NPT_API
D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
                            D3D_ROOT_SIGNATURE_VERSION Version,
                            ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob)
{
   if (ppBlob)
      *ppBlob = NULL;
   if (ppErrorBlob)
      *ppErrorBlob = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   void *raw_blob = NULL;
   void *raw_error = NULL;
   HRESULT hr = npt_call_D3D12SerializeRootSignature(
      dev->ring, pRootSignature, Version, (ID3DBlob **)&raw_blob,
      (ID3DBlob **)&raw_error);

   /*
    * Both blobs are independent objects and either may be present: on
    * failure only the error blob comes back, and the caller may pass NULL
    * for either out-param, in which case the host ref must still be
    * dropped.  npt_device_release() therefore happens exactly once, below.
    */
   if (raw_error) {
      if (ppErrorBlob) {
         void *w = npt_com_get_or_wrap_or_release(
            dev, &NPT_IID_ID3D10Blob, (uint64_t)(uintptr_t)raw_error, NULL);
         if (w) {
            atomic_fetch_add_explicit(
               &((struct npt_com_base *)w)->base.device_ref_holds, 1,
               memory_order_relaxed);
            *ppErrorBlob = (ID3DBlob *)w;
         }
      } else {
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_error);
      }
   }

   if (NPT_FAILED(hr) || !raw_blob) {
      if (raw_blob)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_blob);
      npt_device_release();
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   if (!ppBlob) {
      npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_blob);
      npt_device_release();
      return hr;
   }

   return finish_wrap_object(dev, &NPT_IID_ID3D10Blob, raw_blob, hr,
                             (void **)ppBlob);
}

HRESULT NPT_API
D3D12SerializeVersionedRootSignature(
   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
   ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob)
{
   if (ppBlob)
      *ppBlob = NULL;
   if (ppErrorBlob)
      *ppErrorBlob = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   void *raw_blob = NULL;
   void *raw_error = NULL;
   HRESULT hr = npt_call_D3D12SerializeVersionedRootSignature(
      dev->ring, pRootSignature, (ID3DBlob **)&raw_blob,
      (ID3DBlob **)&raw_error);

   if (raw_error) {
      if (ppErrorBlob) {
         void *w = npt_com_get_or_wrap_or_release(
            dev, &NPT_IID_ID3D10Blob, (uint64_t)(uintptr_t)raw_error, NULL);
         if (w) {
            atomic_fetch_add_explicit(
               &((struct npt_com_base *)w)->base.device_ref_holds, 1,
               memory_order_relaxed);
            *ppErrorBlob = (ID3DBlob *)w;
         }
      } else {
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_error);
      }
   }

   if (NPT_FAILED(hr) || !raw_blob) {
      if (raw_blob)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_blob);
      npt_device_release();
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   if (!ppBlob) {
      npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_blob);
      npt_device_release();
      return hr;
   }

   return finish_wrap_object(dev, &NPT_IID_ID3D10Blob, raw_blob, hr,
                             (void **)ppBlob);
}

HRESULT NPT_API
D3D12CreateRootSignatureDeserializer(
   const void *pSrcData, SIZE_T SrcDataSizeInBytes,
   REFIID pRootSignatureDeserializerInterface,
   void **ppRootSignatureDeserializer)
{
   if (!ppRootSignatureDeserializer)
      return NPT_E_INVALIDARG;
   *ppRootSignatureDeserializer = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_D3D12CreateRootSignatureDeserializer(
      dev->ring, pSrcData, SrcDataSizeInBytes,
      pRootSignatureDeserializerInterface, &raw);

   return finish_wrap_object(dev, &NPT_IID_ID3D12RootSignatureDeserializer,
                             raw, hr, ppRootSignatureDeserializer);
}

HRESULT NPT_API
D3D12CreateVersionedRootSignatureDeserializer(
   const void *pSrcData, SIZE_T SrcDataSizeInBytes,
   REFIID pRootSignatureDeserializerInterface,
   void **ppRootSignatureDeserializer)
{
   if (!ppRootSignatureDeserializer)
      return NPT_E_INVALIDARG;
   *ppRootSignatureDeserializer = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_D3D12CreateVersionedRootSignatureDeserializer(
      dev->ring, pSrcData, SrcDataSizeInBytes,
      pRootSignatureDeserializerInterface, &raw);

   return finish_wrap_object(
      dev, &NPT_IID_ID3D12VersionedRootSignatureDeserializer, raw, hr,
      ppRootSignatureDeserializer);
}

/*
 * Exports below have no wire protocol behind them, but d3d12.dll must
 * still provide them: an application that imports d3d12.dll statically
 * (npt_test.exe imports ten symbols) fails to LOAD if any one is
 * missing, with "the procedure entry point ... could not be located".
 * A missing export is therefore not a degraded feature, it is a program
 * that will not start.
 */

/*
 * No debug-layer protocol coverage: vkd3d-proton's validation is armed
 * host-side via VKD3D_DEBUG/VKD3D_CONFIG in the render server, not
 * through a guest ID3D12Debug.  Return a real, documented failure and a
 * NULL out-pointer -- callers defensively probe-and-continue.
 */
HRESULT NPT_API
D3D12GetDebugInterface(REFIID riid, void **ppvDebug)
{
   (void)riid;
   if (ppvDebug)
      *ppvDebug = NULL;
   return NPT_E_NOINTERFACE;
}

/* No backend; mirrors D3D12GetDebugInterface. Real apps probe and continue. */
HRESULT NPT_API
D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID *pIIDs,
                                void *pConfigurationStructs,
                                UINT *pConfigurationStructSizes)
{
   (void)NumFeatures;
   (void)pIIDs;
   (void)pConfigurationStructs;
   (void)pConfigurationStructSizes;
   return NPT_E_NOINTERFACE;
}

/*
 * The D3D12PIX* family is undocumented -- Microsoft has never published a
 * header for it -- so these signatures come from the declarations
 * npt_test.c carries.  They exist purely so PIX-instrumented binaries
 * link and run; doing nothing is the correct behaviour without a
 * profiler attached.
 */
UINT64 NPT_API
D3D12PIXGetThreadInfo(void)
{
   return 0;
}

UINT64 *NPT_API
D3D12PIXEventsReplaceBlock(BOOL getEarliestTime)
{
   (void)getEarliestTime;
   return NULL;
}

void NPT_API
D3D12PIXNotifyWakeFromFenceSignal(HANDLE hEvent)
{
   (void)hEvent;
}

void NPT_API
D3D12PIXReportCounter(PCWSTR name, float value)
{
   (void)name;
   (void)value;
}
