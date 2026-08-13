/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI command queues + fence operations: create, execute and the
 * fence entries all forward to the inner device's queue.  Ordering
 * against the app's fences is not established here but by the
 * monitored-fence gate below.
 */

#include "triton12.h"
#include "triton_log.h"

#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU_CMD_GATE / VIOGPU_DMA_PRIVATE */

/* Arm a GPU-true gate on the queue's drain fence reaching `value`;
 * returns the KMD gate token via *kmd_token_out (FALSE on failure). */
extern BOOL npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value,
                                   UINT64 *kmd_token_out);

static VOID APIENTRY t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice,
                                            D3D12DDI_HCOMMANDQUEUE hQueue);

static D3D12_COMMAND_LIST_TYPE
t12QueueFlagsToApiType(D3D12DDI_COMMAND_QUEUE_FLAGS Flags)
{
    /* A DIRECT queue carries 3D|COMPUTE|COPY (0x7) -- test 3D FIRST.
     * Checking COMPUTE first made the app's direct queue a compute
     * queue on the inner device, and the recorded clear/draw work then
     * hits the host's compute ring and hangs the GPU.  Copies are legal
     * on a compute queue, so a copy-only workload hides the mistake. */
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

static SIZE_T APIENTRY
t12CalcPrivateCommandQueueSize(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUEUE);
}

static HRESULT APIENTRY
t12CreateCommandQueue(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = pArgs ? (PTRITON12_QUEUE)pArgs->hDrvCommandQueue.pDrvPrivate : NULL;
    if (!p || !p->pDev || !q)
        return E_INVALIDARG;
    memset(q, 0, sizeof(*q));
    q->pDev = p;

    D3D12_COMMAND_QUEUE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Type = t12QueueFlagsToApiType(pArgs->QueueFlags);
    HRESULT hr = ID3D12Device_CreateCommandQueue(
        p->pDev, &desc, &IID_ID3D12CommandQueue, (void **)&q->pQueue);
    TR_LOG("12.CreateCommandQueue: flags=0x%x -> 0x%08lx",
           (unsigned)pArgs->QueueFlags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    /* Per-queue kernel context: the runtime binds the context to
     * hRTCommandQueue and runs its kernel fence ops against it.  The
     * KMD accepts a zero-private-data DX12 virtual context (kmprobe). */
    q->hRTCommandQueue = pArgs->hRTCommandQueue;
    if (p->pUMCallbacks && p->pUMCallbacks->pfnCreateContextVirtualCb) {
        D3DDDICB_CREATECONTEXTVIRTUAL ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.NodeOrdinal = 0;
        ctx.EngineAffinity = 1;
        HRESULT chr = p->pUMCallbacks->pfnCreateContextVirtualCb(
            pArgs->hRTCommandQueue, &ctx);
        TR_LOG("12.CreateCommandQueue: CreateContextVirtualCb -> 0x%08lx "
               "hCtx=%p", (unsigned long)chr, ctx.hContext);
        if (FAILED(chr)) {
            ID3D12CommandQueue_Release(q->pQueue);
            q->pQueue = NULL;
            return chr;
        }
        q->hKMContext = ctx.hContext;
    }

    /* Per-queue drain fence for the monitored-fence gates (see
     * t12ExecuteCommandLists).  A queue whose fences can't be gated would
     * complete every app fence at CPU speed -- fail the create loudly
     * instead. */
    hr = ID3D12Device_CreateFence(p->pDev, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void **)&q->pDrainFence);
    if (FAILED(hr)) {
        TR_LOG("12.CreateCommandQueue: drain-fence create FAILED 0x%08lx",
               (unsigned long)hr);
        t12DestroyCommandQueue(hDevice, pArgs->hDrvCommandQueue);
        return hr;
    }
    q->DrainValue = 0;
    /* Event for the present-fence arm in t12Present.  Not fatal if it fails:
     * the arm is simply skipped and flips fall back to today's ungated
     * behaviour rather than the queue failing to create. */
    q->hPresentArmEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!q->hPresentArmEvent)
        TR_LOG("12.CreateCommandQueue: present-arm event create FAILED %lu "
               "(flips not render-gated on this queue)",
               (unsigned long)GetLastError());
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hQueue)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q)
        return;
    if (q->hKMContext && p && p->pUMCallbacks &&
        p->pUMCallbacks->pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT dc;
        memset(&dc, 0, sizeof(dc));
        dc.hContext = q->hKMContext;
        p->pUMCallbacks->pfnDestroyContextCb(q->hRTCommandQueue, &dc);
        q->hKMContext = NULL;
    }
    if (q->pDrainFence) {
        ID3D12Fence_Release(q->pDrainFence);
        q->pDrainFence = NULL;
    }
    if (q->pQueue) {
        ID3D12CommandQueue_Release(q->pQueue);
        q->pQueue = NULL;
    }
    if (q->hPresentArmEvent) {
        CloseHandle(q->hPresentArmEvent);
        q->hPresentArmEvent = NULL;
    }
}

/* ---------- queue table (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001) ---------- */

/*
 * Monitored-fence gate, once per ECL batch.  The runtime never calls
 * pfnSignalFence/pfnWaitForFence on this driver: app fences are serviced
 * by dxgkrnl monitored-fence packets on the queue's kernel context,
 * which complete once the context's prior DMA completes.  A context
 * carrying no DMA therefore retires every fence at CPU speed while the
 * GPU work is still running on the host, and the app reads results that
 * do not exist yet.
 *
 * The gate restores the WDDM contract: (1) Signal the per-queue drain
 * fence on the inner queue -- rides the wire ring FIFO-after the ECL, so
 * it completes on the host GPU timeline after the batch; (2) arm a KMD
 * event-ring fence that fires a gate token when the host retires it at
 * that completion; (3) submit a VIOGPU_CMD_GATE{token} DMA packet on the
 * queue's kernel context.  The KMD parks the packet until the token
 * fires, so dxgkrnl's fence packets (and CPU waiters/GetCompletedValue)
 * only observe values the GPU truly reached.
 */
static void
t12QueueGate(PTRITON12_QUEUE q)
{
    if (!q->pDrainFence || !q->hKMContext || !q->pDev ||
        !q->pDev->KTCallbacks.pfnSubmitCommandCb) {
        static LONG miss;
        if (miss < 4) { InterlockedIncrement(&miss);
            TR_LOG("12.QueueGate: UNGATED queue %p (no fence/ctx/cb)", (void *)q); }
        return;
    }

    const UINT64 v = ++q->DrainValue;
    HRESULT hr = ID3D12CommandQueue_Signal(q->pQueue, q->pDrainFence, v);
    if (FAILED(hr)) {
        TR_LOG("12.QueueGate: drain Signal FAILED 0x%08lx v=%llu",
               (unsigned long)hr, (unsigned long long)v);
        return;
    }

    UINT64 kmdToken = 0;
    if (!npt_d3d12_ecl_gate_arm(q->pDrainFence, v, &kmdToken)) {
        static LONG armFail;
        if (armFail < 8) { InterlockedIncrement(&armFail);
            TR_LOG("12.QueueGate: gate arm FAILED v=%llu -- fences will run "
                   "EARLY for this batch", (unsigned long long)v); }
        return;
    }

    /* GATE packet: VIOGPU_COMMAND_HDR + token, carried by value in the
     * submission's private data (the KMD's mirror path; GPU VAs are not
     * CPU-mappable, and the packet needs no DMA content). */
    VIOGPU_DMA_PRIVATE priv;
    memset(&priv, 0, sizeof(priv));
    priv.magic = VIOGPU_DMA_PRIV_MAGIC;
    priv.bodySize = sizeof(VIOGPU_COMMAND_HDR) + sizeof(UINT64);
    VIOGPU_COMMAND_HDR *hdr = (VIOGPU_COMMAND_HDR *)priv.body;
    hdr->type = VIOGPU_CMD_GATE;
    hdr->size = sizeof(UINT64);
    hdr->flags = 0;
    hdr->ring_idx = 0;
    memcpy(priv.body + sizeof(VIOGPU_COMMAND_HDR), &kmdToken, sizeof(UINT64));

    D3DDDICB_SUBMITCOMMAND sub;
    memset(&sub, 0, sizeof(sub));
    /* A zero-length submission never becomes a real packet on the
     * context queue -- dxgkrnl then sees the context idle at the app's
     * queue::Signal and CPU-writes the monitored-fence value instantly,
     * bypassing the gate entirely.  Point Commands at the KMD's shmem
     * window base -- a GPU VA reserved in every process -- with a
     * nominal length: the KMD never dereferences it. */
    sub.Commands = 0x700000000ull;
    sub.CommandLength = 64;
    sub.BroadcastContextCount = 1;
    sub.BroadcastContext[0] = q->hKMContext;
    sub.pPrivateDriverData = &priv;
    sub.PrivateDriverDataSize = sizeof(priv);

    hr = q->pDev->KTCallbacks.pfnSubmitCommandCb(q->pDev->hRTDevice.handle,
                                                 &sub);
    if (FAILED(hr)) {
        static LONG subFail;
        if (subFail < 8) { InterlockedIncrement(&subFail);
            TR_LOG("12.QueueGate: SubmitCommandCb FAILED 0x%08lx -- fences "
                   "will run EARLY for this batch", (unsigned long)hr); }
    } else {
        static LONG gateOk;
        if (gateOk < 8) { InterlockedIncrement(&gateOk);
            TR_LOG("12.QueueGate: GATED v=%llu kmdToken=%llu",
                   (unsigned long long)v, (unsigned long long)kmdToken); }
    }
}

static VOID APIENTRY
t12ExecuteCommandLists(D3D12DDI_HCOMMANDQUEUE hQueue, UINT Count,
                       const D3D12DDI_HCOMMANDLIST *pCommandLists)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q || !q->pQueue || !Count || !pCommandLists)
        return;

    ID3D12CommandList *stackLists[8];
    ID3D12CommandList **lists = stackLists;
    if (Count > 8) {
        lists = (ID3D12CommandList **)malloc(Count * sizeof(*lists));
        if (!lists)
            return;
    }
    UINT n = 0;
    for (UINT i = 0; i < Count; i++) {
        PTRITON12_LIST l = (PTRITON12_LIST)pCommandLists[i].pDrvPrivate;
        if (l && l->pList)
            lists[n++] = (ID3D12CommandList *)l->pList;
    }
    TR_LOG_HOT("12.ExecuteCommandLists: %u list(s)", n);
    if (n) {
        ID3D12CommandQueue_ExecuteCommandLists(q->pQueue, n, lists);
        t12QueueGate(q);
    }
    if (lists != stackLists)
        free(lists);
}

static void APIENTRY
t12SignalFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;

    /* Forward to the inner queue so the host GPU timeline carries the
     * signal.  No CPU wait here: the runtime services app fences through
     * dxgkrnl monitored-fence packets on the queue's kernel context,
     * which the per-ECL gate packets (t12QueueGate) hold to real GPU
     * completion.  (In practice the runtime never calls this DDI on this
     * driver -- kernel packets carry the fence model -- but the forward
     * keeps the host timeline correct if it ever does.) */
    HRESULT hr = ID3D12CommandQueue_Signal(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.SignalFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr;
}

static void APIENTRY
t12WaitForFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;
    HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.WaitForFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr; /* only consumed by TR_LOG_HOT, which compiles out by default */
}

void
triton12InstallQueueDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue;
    t->pfnDestroyCommandQueue         = t12DestroyCommandQueue;
}

void
triton12InstallQueueFuncs(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t)
{
    t->pfnExecuteCommandLists = t12ExecuteCommandLists;
    t->pfnSignalFence         = t12SignalFence;
    t->pfnWaitForFence        = t12WaitForFence;
    /* UpdateTileMappings / CopyTileMappings stay logged stubs (no tiled
     * resources). */
}
