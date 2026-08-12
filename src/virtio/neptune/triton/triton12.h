/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Triton D3D12 DDI: private handle structs behind the D3D12DDI_H*
 * handles.  As in triton.h for D3D11, the runtime allocates
 * the private storage (pfnCalcPrivate*Size), we place our struct in it,
 * and every wrapped Neptune COM pointer is a HOST pointer forwarded
 * verbatim through the wrapper vtbl layer.
 */

#ifndef TRITON12_H
#define TRITON12_H

#include "triton.h"   /* windows.h, d3dkmthk.h, TR_LOG plumbing */

#include <d3d12.h>
#include <d3d12umddi.h>

typedef struct TRITON12_ADAPTER
{
    D3D12DDI_HRTADAPTER     hRTAdapter;
    D3DDDI_ADAPTERCALLBACKS RtCallbacks;
    /* The DDI version token the runtime settled on (from
     * pfnCreateDevice's Interface/Version). */
    UINT                    uIfVersion;
    UINT                    uRtVersion;
    /* Runtime handles of the filled COMMAND_LIST_3D tables (num=0
     * direct, num=1 bundle), captured in pfnFillDDITable.
     * pfnCreateCommandList must hand the right one back through
     * pfnSetCommandListDDITableCb or the runtime dispatches through a
     * NULL table. */
    D3D12DDI_HRTTABLE       hRTTableCmdList[2];
} TRITON12_ADAPTER, *PTRITON12_ADAPTER;

typedef struct TRITON12_DEVICE
{
    PTRITON12_ADAPTER            pAdapter;
    D3D12DDI_HRTDEVICE           hRTDevice;
    UINT                         uIfVersion;
    UINT                         uRtVersion;
    D3DDDI_DEVICECALLBACKS       KTCallbacks;
    CONST D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *pUMCallbacks;

    /* Neptune COM wrappers (host-side ID3D12Device behind the wrapper
     * vtbl layer).  Populated by tritonCreateDevice12 via
     * npt_d3d12_create_device_internal. */
    ID3D12Device                *pDev;
    /* One-shot VIOGPU_CTX_INIT on the runtime's kernel device so the
     * KMD can bind exported blobs; see
     * tritonPresentEnsureRuntimeCtx. */
    BOOL                         RuntimeCtxInited;
} TRITON12_DEVICE, *PTRITON12_DEVICE;

static inline PTRITON12_DEVICE
triton12Device(D3D12DDI_HDEVICE hDevice)
{
    return (PTRITON12_DEVICE)hDevice.pDrvPrivate;
}

typedef struct TRITON12_DESCRIPTOR_HEAP
{
    ID3D12DescriptorHeap        *pHeap;
} TRITON12_DESCRIPTOR_HEAP, *PTRITON12_DESCRIPTOR_HEAP;

typedef struct TRITON12_HEAP
{
    ID3D12Heap                  *pHeap;      /* standalone heap, or NULL */
    ID3D12Resource              *pResource;  /* committed create: the resource
                                              * owning the implicit heap, so
                                              * pfnMapHeap can Map it */
    /* Standalone-heap create desc (type/flags/size), kept so a later
     * placed-resource create in this heap can rebuild inner-API
     * arguments without a wire GetDesc round-trip. */
    D3D12_HEAP_DESC              Desc;
} TRITON12_HEAP, *PTRITON12_HEAP;

typedef struct TRITON12_RESOURCE
{
    ID3D12Resource              *pResource;
    /* Create-time DDI desc, kept for CheckSubresourceInfo /
     * footprint math without a wire GetDesc round-trip. */
    D3D12DDIARG_CREATERESOURCE_0003 Desc;
    /* Whole-heap implicit buffer only (API CreateHeap arrives as heap +
     * covering BUFFER): the inner heap placed resources alias into.
     * Borrowed from the owning TRITON12_HEAP (which releases it); placed
     * creates reach it via ReuseBufferGPUVA.BaseAddress.UMD.hResource. */
    ID3D12Heap                  *pPlaceHeap;
    /* Runtime resource handle + the shared-blob KM allocation the
     * runtime's kernel present flips.  Presentable resources only. */
    D3D12DDI_HRTRESOURCE         hRTResource;
    D3DKMT_HANDLE                hKMAllocation;
    BOOL                         Primary;
    /* Cached host GPU VA (pfnCheckResourceVirtualAddress). */
    UINT64                       GpuVa;
} TRITON12_RESOURCE, *PTRITON12_RESOURCE;

typedef struct TRITON12_QUERYHEAP
{
    ID3D12QueryHeap             *pHeap;
} TRITON12_QUERYHEAP, *PTRITON12_QUERYHEAP;

typedef struct TRITON12_FENCE
{
    ID3D12Fence                 *pFence;
    /* Runtime-provided fence value placements. */
    D3D12DDI_FENCE_PLACEMENT     Value;
    D3D12DDI_FENCE_PLACEMENT     MonitoredValue;
} TRITON12_FENCE, *PTRITON12_FENCE;

typedef struct TRITON12_QUEUE
{
    ID3D12CommandQueue          *pQueue;
    PTRITON12_DEVICE             pDev;
    /* Per-queue kernel context (pfnCreateContextVirtualCb): the runtime
     * binds it to hRTCommandQueue and performs its kernel-side fence
     * signals/waits against it -- without one, Queue::Signal throws
     * STATUS_INVALID_PARAMETER. */
    D3D12DDI_HRTCOMMANDQUEUE     hRTCommandQueue;
    HANDLE                       hKMContext;
    /* Monitored-fence gate state: the runtime services app fences through
     * dxgkrnl packets on hKMContext, which complete when the context's
     * prior DMA completes.  Per ECL batch we Signal pDrainFence on the
     * inner host queue and submit a VIOGPU_CMD_GATE DMA packet
     * that the KMD parks until the host GPU truly reached DrainValue --
     * making every fence the app observes GPU-true.  See tritonQueue12.c. */
    ID3D12Fence                 *pDrainFence;
    UINT64                       DrainValue;
} TRITON12_QUEUE, *PTRITON12_QUEUE;

typedef struct TRITON12_ALLOCATOR
{
    ID3D12CommandAllocator      *pAlloc;
} TRITON12_ALLOCATOR, *PTRITON12_ALLOCATOR;

typedef struct TRITON12_LIST
{
    ID3D12GraphicsCommandList   *pList;
    PTRITON12_DEVICE             pDev;
    /* Recording-thread stamp, kept only under
     * NPT_DEBUG=d3d12_list_migration.  A change between Reset and Close
     * means the runtime migrated this list across threads mid-recording
     * — under multi-ring the wire then splits the list across TLS rings
     * with no intra-list ordering. */
    DWORD                        RecordTid;
} TRITON12_LIST, *PTRITON12_LIST;

/* Each object family installs its own slots into the tables the runtime
 * hands to pfnFillDDITable. */
void triton12InstallDescriptorFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallResourceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueueDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueueFuncs(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void triton12InstallListDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallListFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t);
void triton12InstallPipelineFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t);
void triton12InstallPresentDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueryDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);

/* Give a presentable committed resource its shared-blob KM allocation,
 * which the runtime's kernel present needs to flip it. */
BOOL triton12RegisterSharedBlob(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                                BOOL primary);

#endif /* TRITON12_H */
