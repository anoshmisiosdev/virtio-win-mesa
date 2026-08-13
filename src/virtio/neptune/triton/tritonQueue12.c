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

#include <stdlib.h>   /* getenv, for the TRITON12_NO_XQUEUE_ORDER A/B switch */

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

    /* Join the device's queue registry so later ECLs on OTHER queues can order
     * themselves behind this one (t12OrderAgainstSiblings).  Not fatal if the
     * registry is full: the queue simply is not ordered against, which is
     * today's behaviour. */
    q->Slot = TRITON12_MAX_QUEUES;
    if (!p->QueueLockInit)
        return S_OK;   /* device create failed to init the lock; stay unordered */
    EnterCriticalSection(&p->QueueLock);
    if (p->QueueCount < TRITON12_MAX_QUEUES) {
        q->Slot = p->QueueCount;
        p->Queues[p->QueueCount++] = (struct TRITON12_QUEUE *)q;
    }
    LeaveCriticalSection(&p->QueueLock);
    if (q->Slot >= TRITON12_MAX_QUEUES)
        TR_LOG("12.CreateCommandQueue: queue registry full (%u); this queue is "
               "NOT cross-queue ordered", TRITON12_MAX_QUEUES);
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hQueue)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q)
        return;

    /* Leave the registry before anything is released: a concurrent ECL on a
     * sibling walks Queues[] and touches pDrainFence.  Compact by moving the
     * tail entry down and fixing its Slot, so slots stay dense and every
     * queue's XQueueSeen[] indices remain valid. */
    if (p && p->QueueLockInit) {
        EnterCriticalSection(&p->QueueLock);
        for (UINT i = 0; i < p->QueueCount; i++) {
            if (p->Queues[i] != (struct TRITON12_QUEUE *)q)
                continue;
            PTRITON12_QUEUE moved = (PTRITON12_QUEUE)p->Queues[p->QueueCount - 1];
            p->Queues[i] = (struct TRITON12_QUEUE *)moved;
            p->Queues[--p->QueueCount] = NULL;
            if (moved && moved != q) {
                moved->Slot = i;
                /* The slot now means a different queue, so every sibling's
                 * high-water mark for it is meaningless -- clear them or a
                 * stale mark would suppress real waits. */
                for (UINT j = 0; j < p->QueueCount; j++) {
                    PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[j];
                    if (s)
                        s->XQueueSeen[i] = 0;
                }
            }
            break;
        }
        LeaveCriticalSection(&p->QueueLock);
    }

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

/*
 * Cross-queue submission ordering.
 *
 * THE PROBLEM.  An app's `ID3D12CommandQueue::Wait(fence, v)` never reaches
 * this driver: the runtime services D3D12 fences through dxgkrnl
 * monitored-fence packets and never calls pfnWaitForFence (established
 * 2026-07-30; the stub above exists only in case it ever does).  On real
 * hardware that is fine, because the GPU work itself travels in kernel DMA
 * packets and dxgkrnl's scheduler holds them until the wait clears.  Here it
 * does not: t12ExecuteCommandLists forwards the batch to the host over the
 * ring in *user mode*, immediately.  dxgkrnl parks only the kernel packets --
 * the gate DMA -- so the host is left running the app's DIRECT and COMPUTE
 * queues on two independent Vulkan queues with no ordering whatsoever.
 *
 * Measured on Time Spy: a control run spends 23-25% of GPU time on the async
 * compute engine, and the resulting read-before-write shows up as the GT1
 * blue-band corruption (a full-screen pass sampled while its producer was
 * still writing).  Two independent levers that remove the concurrency --
 * VKD3D_CONFIG=single_queue on the host, and 3DMark's own
 * disable_async_compute in the app -- each removed the artifact
 * (p=0.020 and p=0.027; Fisher combined p~0.005).  See
 * shared/source/HANDOFF-2026-08-12-ts-gt1-corruption.md §2a.
 *
 * WHAT THIS DOES.  Before forwarding a batch on queue Q, make Q's inner host
 * queue Wait on every sibling queue's drain fence at that sibling's current
 * value.  The drain fences already exist and are already signalled once per
 * ECL batch by t12QueueGate, so this costs no new objects: it just turns
 * "submitted earlier on another queue" into a real GPU-side ordering edge on
 * the host timeline.
 *
 * WHAT THIS DOES NOT DO -- read before trusting it.  This enforces
 * *submission* order, not the app's actual waits, because the app's waits are
 * not observable here.  It is therefore correct only when the producer's
 * ExecuteCommandLists precedes the consumer's, which is the ordinary pattern
 * and the one Time Spy uses.  An app that submits the consumer first and
 * relies on a later Wait would still race.  The real fix is to route
 * execution ordering through the kernel the way the gate already routes
 * completion -- a DMA packet whose KMD handler releases a host-side start
 * fence, so host execution order follows dxgkrnl's scheduling order and every
 * cross-queue *and* cross-process wait becomes real.  That is the
 * "HOLD/RELEASE riding the gate DMA" design in
 * HANDOFF-2026-07-30-gpu-true-fences.md.  This is the cheap, guest-only
 * approximation of it.
 *
 * It also serialises async compute against graphics, which costs the overlap:
 * expect the same ~2-4% Time Spy graphics-score drop that single_queue shows.
 * Set TRITON12_NO_XQUEUE_ORDER=1 in the workload's environment to turn it off
 * for an A/B.
 */
static BOOL
t12XQueueOrderEnabled(void)
{
    static LONG s_state; /* 0 = unknown, 1 = on, 2 = off */
    LONG v = s_state;
    if (v == 0) {
        const char *e = getenv("TRITON12_NO_XQUEUE_ORDER");
        v = (e && *e && *e != '0') ? 2 : 1;
        InterlockedExchange(&s_state, v);
        TR_LOG("12.XQueueOrder: cross-queue submission ordering %s",
               v == 1 ? "ENABLED" : "DISABLED (TRITON12_NO_XQUEUE_ORDER)");
    }
    return v == 1;
}

static void
t12OrderAgainstSiblings(PTRITON12_QUEUE q)
{
    PTRITON12_DEVICE p = q->pDev;
    if (!p || !p->QueueLockInit || !t12XQueueOrderEnabled())
        return;

    /* Snapshot under the lock, then issue the waits outside it:
     * ID3D12CommandQueue_Wait goes to the host over the wire, and holding a
     * lock across that would serialise every queue in the process on it. */
    ID3D12Fence *fences[TRITON12_MAX_QUEUES];
    UINT64 values[TRITON12_MAX_QUEUES];
    UINT slots[TRITON12_MAX_QUEUES];
    UINT n = 0;

    EnterCriticalSection(&p->QueueLock);
    for (UINT i = 0; i < p->QueueCount; i++) {
        PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[i];
        if (!s || s == q || !s->pDrainFence)
            continue;
        /* Aligned 64-bit read on x64 is atomic; a stale (lower) value only
         * means a weaker edge this time, never a wrong one.  DrainValue is
         * incremented immediately before its Signal is issued, with no
         * blocking call in between, so waiting on it cannot deadlock. */
        const UINT64 v = s->DrainValue;
        if (v == 0 || s->Slot >= TRITON12_MAX_QUEUES ||
            v <= q->XQueueSeen[s->Slot])
            continue;
        q->XQueueSeen[s->Slot] = v;
        fences[n] = s->pDrainFence;
        values[n] = v;
        slots[n]  = s->Slot;
        n++;
    }
    LeaveCriticalSection(&p->QueueLock);

    for (UINT i = 0; i < n; i++) {
        HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, fences[i], values[i]);
        static LONG s_n;
        LONG ln = InterlockedIncrement(&s_n);
        if (ln <= 8 || (ln & 1023) == 0)
            TR_LOG("12.XQueueOrder #%ld: queue %u waits on queue %u drain "
                   "v=%llu -> 0x%08lx", (long)ln, q->Slot, slots[i],
                   (unsigned long long)values[i], (unsigned long)hr);
    }
}

static VOID APIENTRY
t12ExecuteCommandLists(D3D12DDI_HCOMMANDQUEUE hQueue, UINT Count,
                       const D3D12DDI_HCOMMANDLIST *pCommandLists)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q || !q->pQueue || !Count || !pCommandLists)
        return;

    /* Before the batch reaches the host, not after: the wait has to be on the
     * host queue's timeline ahead of this batch's work. */
    t12OrderAgainstSiblings(q);

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
