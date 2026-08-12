/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 present path.  The runtime performs the kernel
 * present itself; our list-table pfnPresent fills D3D12DDI_PRESENT_0003
 * with the backbuffer's shared-blob KM allocation and the presenting
 * queue's kernel context.  Presentable committed resources (PRIMARY
 * heap flag) get their KM allocation via the same shared-bridge blob
 * export + pfnAllocateCb flow as Triton D3D11 primaries
 * (tritonRegisterSharedBlob); the host exports its resources through
 * ID3D12Device::CreateSharedHandle natively.
 */

#include "triton12.h"
#include "tritonSharedBridge.h"
#include "npt_shared_texture.h"
#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU escape / allocation ABI */
#include "triton_log.h"

static HRESULT
t12Escape(PTRITON12_DEVICE p, VIOGPU_ESCAPE *esc)
{
    if (!p->KTCallbacks.pfnEscapeCb || !p->pAdapter)
        return E_NOTIMPL;
    D3DDDICB_ESCAPE cb;
    memset(&cb, 0, sizeof(cb));
    cb.hDevice               = p->hRTDevice.handle;
    cb.pPrivateDriverData    = esc;
    cb.PrivateDriverDataSize = sizeof(*esc);
    return p->KTCallbacks.pfnEscapeCb(p->pAdapter->hRTAdapter.handle, &cb);
}

static void
t12EnsureRuntimeCtx(PTRITON12_DEVICE p)
{
    if (p->RuntimeCtxInited)
        return;
    p->RuntimeCtxInited = TRUE;
    VIOGPU_ESCAPE esc;
    memset(&esc, 0, sizeof(esc));
    esc.Type             = VIOGPU_CTX_INIT;
    esc.DataLength       = sizeof(esc.CtxInit);
    esc.CtxInit.CapsetID = 7;
    esc.CtxInit.NumRings = 1;
    (void)t12Escape(p, &esc);
}

BOOL
triton12RegisterSharedBlob(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                           BOOL primary)
{
    /* D3D12 leaves the legacy D3DDDI_DEVICECALLBACKS.pfnAllocateCb NULL;
     * allocation rides the corelayer callback instead. */
    if (!p->pUMCallbacks || !p->pUMCallbacks->pfnAllocateCb || !r->pResource)
        return FALSE;

    t12EnsureRuntimeCtx(p);

    VIOGPU_CREATE_ALLOCATION_EXCHANGE ax;
    memset(&ax, 0, sizeof(ax));
    ax.Type = VIOGPU_RESOURCE_TYPE_SHARED;
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *o = &ax.OptionsShared;

    struct triton_shared_texture_desc exp;
    memset(&exp, 0, sizeof(exp));
    if (!tritonSharedBridgeExportBlob(r->pResource, &exp)) {
        TR_LOG("12.shared: export failed %llux%u fmt=%d",
               (unsigned long long)r->Desc.Width, r->Desc.Height,
               (int)r->Desc.Format);
        return FALSE;
    }
    o->blob_id         = exp.blob_id;
    o->create_ctx_id   = exp.create_ctx_id;
    o->plane_count     = exp.plane_count;
    o->texture_layout  = exp.texture_layout;
    o->modifier        = exp.modifier;
    o->allocation_size = exp.allocation_size;
    for (UINT i = 0; i < exp.plane_count && i < 4; i++) {
        o->planes[i].offset = exp.planes[i].offset;
        o->planes[i].pitch  = exp.planes[i].pitch;
    }

    o->primary          = primary ? 1u : 0u;
    o->width            = (ULONG)r->Desc.Width;
    o->height           = r->Desc.Height;
    o->mip_levels       = r->Desc.MipLevels ? r->Desc.MipLevels : 1;
    o->array_size       = r->Desc.DepthOrArraySize ? r->Desc.DepthOrArraySize : 1;
    o->format           = (ULONG)r->Desc.Format;
    o->sample_count     = r->Desc.SampleDesc.Count ? r->Desc.SampleDesc.Count : 1;
    o->usage            = 0;    /* D3D11_USAGE_DEFAULT */
    o->bind_flags       = 0x28; /* RENDER_TARGET | SHADER_RESOURCE */
    o->cpu_access_flags = 0;
    o->misc_flags       = 0x2;  /* D3D11_RESOURCE_MISC_SHARED */

    if (primary) {
        o->ScanoutInfo.width      = (ULONG)r->Desc.Width;
        o->ScanoutInfo.height     = r->Desc.Height;
        o->ScanoutInfo.format     = npt_shared_texture_virgl_format(r->Desc.Format);
        o->ScanoutInfo.strides[0] = (ULONG)o->planes[0].pitch;
        o->ScanoutInfo.offsets[0] = (ULONG)o->planes[0].offset;
    }

    ax.Size = (o->allocation_size + 4095ull) & ~4095ull;
    if (!ax.Size)
        ax.Size = (((ULONGLONG)r->Desc.Width * r->Desc.Height * 4) + 4095)
                  & ~4095ull;

    D3D12DDI_ALLOCATION_INFO_0022 ai;
    memset(&ai, 0, sizeof(ai));
    ai.pPrivateDriverData    = &ax;
    ai.PrivateDriverDataSize = sizeof(ax);
    if (primary) {
        ai.Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
        ai.VidPnSourceId = 0;
    }

    D3D12DDICB_ALLOCATE_0022 cb;
    memset(&cb, 0, sizeof(cb));
    cb.hResource        = r->hRTResource.handle;
    cb.NumAllocations   = 1;
    cb.pAllocationInfo  = &ai;

    HRESULT hr = p->pUMCallbacks->pfnAllocateCb(p->hRTDevice, &cb);
    if (FAILED(hr) || !ai.hAllocation) {
        TR_LOG("12.shared: pfnAllocateCb failed hr=0x%08lx",
               (unsigned long)hr);
        return FALSE;
    }
    r->hKMAllocation = ai.hAllocation;
    r->Primary       = primary;
    TR_LOG("12.shared: exporter blob_id=0x%llx alloc=0x%x %llux%u primary=%d",
           (unsigned long long)o->blob_id, ai.hAllocation,
           (unsigned long long)r->Desc.Width, r->Desc.Height, primary);
    return TRUE;
}

/* ---------- lazy KM allocation on demand ----------
 *
 * No DDI flag marks swapchain backbuffers at create time (they arrive
 * as plain RT textures, heapFlags=0x20 resFlags=0x11).  The runtime
 * asks the driver for a resource's kernel allocation through
 * pfnCheckResourceAllocationHandle when it needs one (CreateSharedHandle
 * for the DWM bind) -- create the shared-blob allocation lazily there,
 * so ordinary resources stay host-only. */

static D3DKMT_HANDLE APIENTRY
t12CheckResourceAllocationHandle(D3D12DDI_HDEVICE hDevice,
                                 D3D10DDI_HRESOURCE hResource)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!p || !r)
        return 0;
    if (!r->hKMAllocation)
        triton12RegisterSharedBlob(p, r, FALSE);
    TR_LOG("12.CheckResourceAllocationHandle: r=%p -> 0x%x", (void *)r,
           r->hKMAllocation);
    return r->hKMAllocation;
}

static UINT APIENTRY
t12GetPresentPrivateDriverDataSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_PRESENT_0001 *pArgs)
{
    /* No per-present private driver data; the KMD present path reads
     * everything it needs from the allocation.  (Was a logged stub --
     * returning 0 is the real contract, now explicit.) */
    (void)hDevice; (void)pArgs;
    return 0;
}

void
triton12InstallPresentDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCheckResourceAllocationHandle = t12CheckResourceAllocationHandle;
    t->pfnGetPresentPrivateDriverDataSize = t12GetPresentPrivateDriverDataSize;
}

/* ---------- list-table pfnPresent ---------- */

static VOID APIENTRY
t12Present(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
           const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0003 *pOut)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    (void)hList;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (!pArgs || !q)
        return;

    PTRITON12_RESOURCE src = NULL;
    if (pArgs->SurfacesToPresent && pArgs->phSurfacesToPresent)
        src = (PTRITON12_RESOURCE)
            pArgs->phSurfacesToPresent[0].hSurface.pDrvPrivate;
    PTRITON12_RESOURCE dst =
        (PTRITON12_RESOURCE)pArgs->hDstResource.pDrvPrivate;

    pOut->hSrcAllocation = src ? src->hKMAllocation : 0;
    pOut->hDstAllocation = dst ? dst->hKMAllocation : 0;
    pOut->hContext       = q->hKMContext;
    pOut->AddedGpuWork   = FALSE;
    pOut->BackBufferMultiplicity = 1;

    static LONG s_presentN;
    LONG n = InterlockedIncrement(&s_presentN);
    if (n <= 8 || (n & 255) == 0)
        TR_LOG("12.Present #%ld: src=0x%x dst=0x%x ctx=%p surfaces=%u "
               "flipInterval=%d", (long)n,
               src ? src->hKMAllocation : 0, dst ? dst->hKMAllocation : 0,
               q->hKMContext, pArgs->SurfacesToPresent,
               (int)pArgs->FlipInterval);
}

void
triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t)
{
    t->pfnPresent = t12Present;
}
