#include "runtime/ee_scheduler.h"

#include "Stubs/Pad.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" void ps2_guest_trace_dump(const char *reason);

namespace
{
    uint32_t g_lastDispatchedPc = 0u;

    // The EE's execution stack belongs to the most recently scheduled real
    // guest thread.  Synthetic "invocation" threads (negative ids) only run
    // queued callbacks and interrupt handlers; they have no stack of their own,
    // so interrupt delivery has to fall back to this thread's live $sp.
    int g_lastGuestStackThreadId = 1;

    uint32_t threadStackSp(const GuestThread *thread)
    {
        if (!thread)
        {
            return 0u;
        }
        const uint32_t sp = getRegU32(&thread->activeContext(), 29) & ~0xFu;
        return (sp >= 0x00100000u && sp <= PS2_RAM_SIZE) ? sp : 0u;
    }
}

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    // Watchdog: reports the scheduler's loop stage and state every few
    // seconds, keyed on guest DISPATCH progress (eeCycle can advance even
    // when the loop is wedged, because checkpointDue accounts cycles).
    // Only started under PS2_TRACE=1; it is pure diagnostics.
    std::thread watchdog;
    if (::ps2_log::debug_trace_enabled())
    {
        watchdog = std::thread([this]()
                               {
        std::fprintf(stderr, "[sched-watchdog] started\n");
        uint64_t lastDispatch = 0;
        int reports = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!m_running.load(std::memory_order_acquire))
            {
                std::fprintf(stderr, "[sched-watchdog] scheduler not running, exiting\n");
                return;
            }
            const uint64_t dispatch = m_dispatchCount.load(std::memory_order_relaxed);
            const bool stalled = dispatch == lastDispatch;
            lastDispatch = dispatch;
            ++reports;
            if (reports > 30)
            {
                continue;
            }
            std::fprintf(stderr,
                         "[sched-watchdog] dispatch=%llu stalled=%d stage=%u eeCycle=%llu vsyncTick=%llu "
                         "currentThread=%d deadlines=%zu pendingInv=%zu "
                         "checkpointPending=%d nextDeadline=%llu\n",
                         static_cast<unsigned long long>(dispatch),
                         stalled ? 1 : 0,
                         m_loopStage.load(),
                         static_cast<unsigned long long>(m_eeCycle),
                         static_cast<unsigned long long>(m_vsyncTick),
                         m_currentThreadId,
                         m_deadlines.size(),
                         m_pendingInvocations.size(),
                         m_checkpointPending.load() ? 1 : 0,
                         static_cast<unsigned long long>(m_nextDeadlineCycle.load()));
            {
                std::lock_guard lock(m_eventMutex);
                for (const ScheduledEvent &item : m_deadlines)
                {
                    const auto hostInMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              item.hostDeadline - std::chrono::steady_clock::now()).count();
                    std::fprintf(stderr,
                                 "[sched-watchdog]   event type=%u dueCycle=%llu (ee-%lld) hostInMs=%lld seq=%llu\n",
                                 static_cast<unsigned>(item.event.type),
                                 static_cast<unsigned long long>(item.deadlineCycle),
                                 static_cast<long long>(item.deadlineCycle - m_eeCycle),
                                 static_cast<long long>(hostInMs),
                                 static_cast<unsigned long long>(item.sequence));
                }
            }
        } });
    }
    m_watchdogThread = std::move(watchdog);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        m_loopStage.store(1, std::memory_order_relaxed);
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        // DMA completions raise DMAC causes that are normally consumed when
        // the guest performs its next 32-bit store.  If every thread blocks on
        // a semaphore/event that a DMA-completion handler would signal (e.g.
        // the GIF channel-done handler signaling the game's frame-handshake
        // sema), nothing ever drains them and the machine deadlocks.  Drain
        // them here every iteration so completions are always delivered.
        m_runtime.drainCompletedDmacHandlers(m_rdram);

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                m_loopStage.store(0, std::memory_order_relaxed);
                // Diagnostic: when nothing is runnable, dump every guest
                // thread's wait state so boot stalls can be diagnosed.
                PS2_IF_TRACE({
                    static int idleDumps = 0;
                    if (idleDumps < 24)
                    {
                        ++idleDumps;
                        std::fprintf(stderr, "[idle-dump] no runnable threads (dump %d) intcMask=%08x dmacMask=%08x vsyncTick=%llu\n",
                                     idleDumps, m_enabledIntcMask, m_enabledDmacMask,
                                     static_cast<unsigned long long>(m_vsyncTick));
                        for (const auto &[tid, t] : m_threads)
                        {
                            std::fprintf(stderr,
                                         "[idle-dump] thread=%d status=%u pc=%08x sp=%08x reason=%u",
                                         tid, static_cast<unsigned>(t.status),
                                         t.context.pc, getRegU32(&t.context, 29),
                                         static_cast<unsigned>(t.wait.reason));
                            if (t.wait.reason == EeWaitReason::Semaphore)
                            {
                                std::fprintf(stderr, " sema=%d",
                                             std::get<EeSemaphoreWait>(t.wait.payload).id);
                            }
                            else if (t.wait.reason == EeWaitReason::EventFlag)
                            {
                                const auto &w = std::get<EeEventFlagWait>(t.wait.payload);
                                std::fprintf(stderr, " ef=%d bits=%08x mode=%u",
                                             w.id, w.bits, w.mode);
                            }
                            else if (t.wait.reason == EeWaitReason::VSync)
                            {
                                std::fprintf(stderr, " afterTick=%llu",
                                             static_cast<unsigned long long>(
                                                 std::get<EeVSyncWait>(t.wait.payload).afterTick));
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                });
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                prepareInvocationStack(*owner, invocation);
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        // Temporary diagnostic: report whenever the guest word at 0x01FFFF20
        // changes, together with the function that was dispatched last.  The
        // boot thread's saved $ra lives there and something is replacing it
        // with 1, which makes FUN_002d52a0 return to address 1.
        if (m_rdram)
        {
            uint64_t watched = 0u;
            std::memcpy(&watched, m_rdram + 0x01FFFF20u, sizeof(watched));
            static uint64_t previousWatched = 0u;
            static int watchLogs = 0;
            if (watched != previousWatched)
            {
                if (watchLogs++ < 40)
                {
                    std::fprintf(stderr,
                                 "[w1ffff20] value=0x%016llx prev=0x%016llx lastDispatch=0x%08x "
                                 "nextPc=0x%08x thread=%u sp=0x%08x ra=0x%08x depth=%zu\n",
                                 static_cast<unsigned long long>(watched),
                                 static_cast<unsigned long long>(previousWatched),
                                 g_lastDispatchedPc, context.pc, running->id,
                                 getRegU32(&context, 29), getRegU32(&context, 31),
                                 running->invocations.size());
                }
                previousWatched = watched;
            }
        }

        // Temporary diagnostic: watch the cDir stream state bytes so we can see
        // which guest function advances 1 -> 2 -> 3 for the boot file reads.
        if (m_rdram)
        {
            PS2_IF_TRACE({
            static uint8_t previousState[2] = {0xffu, 0xffu};
            static int stateLogs = 0;
            const uint32_t watchedAddrs[2] = {0x3EAEC9u, 0x3EAF0Du};
            for (int i = 0; i < 2; ++i)
            {
                const uint8_t value = m_rdram[watchedAddrs[i]];
                if (value != previousState[i])
                {
                    if (stateLogs++ < 60)
                    {
                        std::fprintf(stderr,
                                     "[streamstate] addr=%08x %02x -> %02x lastDispatch=%08x "
                                     "nextPc=%08x thread=%u sp=0x%08x ra=0x%08x\n",
                                     watchedAddrs[i], previousState[i], value,
                                     g_lastDispatchedPc, context.pc, running->id,
                                     getRegU32(&context, 29), getRegU32(&context, 31));
                    }
                    previousState[i] = value;
                }
            }
            });
        }

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            {
                static bool traceDumped = false;
                if (!traceDumped)
                {
                    traceDumped = true;
                    std::fprintf(stderr,
                                 "[ee-sched] pc==0 after dispatch pc=0x%08x sp=0x%08x ra=0x%08x gp=0x%08x\n",
                                 g_lastDispatchedPc,
                                 getRegU32(&running->activeContext(), 29),
                                 getRegU32(&running->activeContext(), 31),
                                 getRegU32(&running->activeContext(), 28));
                    ps2_guest_trace_dump("EE thread reached pc==0");
                }
            }
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        m_loopStage.store(3, std::memory_order_relaxed);
        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            prepareInvocationStack(*running, invocation);
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        // Deliver already-attached invocations (IRQ/VSYNC handlers) even when
        // a checkpoint is due: interrupt delivery must never be starved by
        // event pacing, otherwise the guest waits on a semaphore that only an
        // undeliverable handler can signal.
        m_loopStage.store(4, std::memory_order_relaxed);
        const bool hasAttachedInvocation = !running->invocations.empty();
        if (!hasAttachedInvocation && checkpointDue(kGuestDispatchCycles))
        {
            // Host-spin detector: if we keep skipping dispatches the guest is
            // either paced correctly (VBlank cadence) or wedged.  Report long
            // spins with full state so the difference is visible.
            static uint64_t skipCount = 0;
            static uint64_t lastReport = 0;
            ++skipCount;
            if (skipCount - lastReport > 50000000ull)
            {
                lastReport = skipCount;
                std::fprintf(stderr,
                             "[sched-spin] skips=%llu eeCycle=%llu nextDeadline=%llu "
                             "sliceEnd=%llu checkpointPending=%d vsyncTick=%llu "
                             "deadlines=%zu currentThread=%d\n",
                             static_cast<unsigned long long>(skipCount),
                             static_cast<unsigned long long>(m_eeCycle),
                             static_cast<unsigned long long>(m_nextDeadlineCycle.load()),
                             static_cast<unsigned long long>(m_sliceEndCycle),
                             m_checkpointPending.load() ? 1 : 0,
                             static_cast<unsigned long long>(m_vsyncTick),
                             m_deadlines.size(), m_currentThreadId);
            }
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            if (context.pc == 0x002D5320u)
            {
                PS2_IF_TRACE({
                static int resumeLogs = 0;
                if (resumeLogs++ < 4)
                {
                    std::fprintf(stderr,
                                 "[resume-2d5320] thread=%u sp=0x%08x ra=0x%08x depth=%zu prevDispatch=0x%08x\n",
                                 running->id, getRegU32(&context, 29), getRegU32(&context, 31),
                                 running->invocations.size(), g_lastDispatchedPc);
                }
                });
            }
            g_lastDispatchedPc = context.pc;
            PS2_IF_TRACE({
                static int dbgInvocationPrints = 0;
                if (!running->invocations.empty() && dbgInvocationPrints < 120)
                {
                    ++dbgInvocationPrints;
                    const GuestInvocation &inv = running->invocations.back();
                    std::fprintf(stderr,
                                 "[dbg-inv] kind=%u invpc=%08x invsp=%08x invra=%08x threadpc=%08x threadsp=%08x disp=%08x depth=%zu\n",
                                 static_cast<unsigned>(inv.kind),
                                 inv.context.pc, getRegU32(&inv.context, 29), getRegU32(&inv.context, 31),
                                 running->context.pc,
                                 getRegU32(&running->context, 29),
                                 context.pc, running->invocations.size());
                }
            });
            function(m_rdram, &context, &m_runtime);
            m_dispatchCount.fetch_add(1, std::memory_order_relaxed);
            m_loopStage.store(5, std::memory_order_relaxed);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        // Watchdog: 0x242608/0x2425f0 is the game's pure-branch "wait N
        // vsyncs" spin loop.  If it spins forever the VBlank delivery pipeline
        // is broken somewhere; dump the relevant scheduler state once.
        if (context.pc == 0x00242608u || context.pc == 0x002425F0u)
        {
            static uint64_t spinCount = 0;
            static bool spinDumped = false;
            ++spinCount;
            if (spinCount > 2000000u && !spinDumped)
            {
                spinDumped = true;
                std::fprintf(stderr,
                             "[spin-watchdog] spins=%llu eeCycle=%llu nextDeadline=%llu "
                             "checkpointPending=%d vsyncTick=%llu pendingInvocations=%zu "
                             "threadInvocations=%zu currentThread=%d\n",
                             static_cast<unsigned long long>(spinCount),
                             static_cast<unsigned long long>(m_eeCycle),
                             static_cast<unsigned long long>(m_nextDeadlineCycle.load()),
                             m_checkpointPending.load() ? 1 : 0,
                             static_cast<unsigned long long>(m_vsyncTick),
                             m_pendingInvocations.size(),
                             running->invocations.size(),
                             m_currentThreadId);
            }
        }

        // Generic watchdog: periodically report where the guest is spending
        // its dispatches, so unknown wedges can be located.
        {
            PS2_IF_TRACE({
            static uint64_t dispatchCount = 0;
            ++dispatchCount;
            if ((dispatchCount & 0x1FFFFu) == 0u)   // every ~130k dispatches
            {
                static int hbLogs = 0;
                if (hbLogs < 240)
                {
                    ++hbLogs;
                    std::fprintf(stderr,
                                 "[dispatch-heartbeat] n=%llu pc=0x%08x thread=%d eeCycle=%llu "
                                 "vsyncTick=%llu pendingInv=%zu threadInv=%zu nextDeadline=%llu\n",
                                 static_cast<unsigned long long>(dispatchCount),
                                 context.pc, m_currentThreadId,
                                 static_cast<unsigned long long>(m_eeCycle),
                                 static_cast<unsigned long long>(m_vsyncTick),
                                 m_pendingInvocations.size(),
                                 running->invocations.size(),
                                 static_cast<unsigned long long>(m_nextDeadlineCycle.load()));
                }
            }
            });
        }

        // Diagnostic: a dispatch that leaves the program counter outside the
        // code segment means the guest executed `jr`/`j` with a corrupted
        // target.  Log the entry point so the offending routine can be found.
        if (context.pc != 0u && context.pc < 0x00100000u)
        {
            static int badPcLogs = 0;
            if (badPcLogs++ < 16)
            {
                std::fprintf(stderr,
                             "[bad-pc] from=0x%08x to=0x%08x sp=0x%08x ra=0x%08x gp=0x%08x thread=%u depth=%zu\n",
                             g_lastDispatchedPc, context.pc,
                             getRegU32(&context, 29), getRegU32(&context, 31), getRegU32(&context, 28),
                             running->id, running->invocations.size());
            }
        }

        processPendingEvents();
        m_loopStage.store(2, std::memory_order_relaxed);
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    // Real-time gate: fire the checkpoint when the earliest scheduled event's
    // HOST deadline passes, even if its guest-cycle deadline is still far
    // away (the guest can run much slower than real time in wait loops).
    const int64_t nextHostDeadlineMs = m_nextHostDeadlineMs.load(std::memory_order_acquire);
    if (nextHostDeadlineMs != 0)
    {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs >= nextHostDeadlineMs)
        {
            m_checkpointPending.store(true, std::memory_order_release);
            return true;
        }
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    prepareInvocationStack(*owner, invocation);
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        prepareInvocationStack(*owner, *it);
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

void EeScheduler::prepareInvocationStack(GuestThread &owner, GuestInvocation &invocation)
{
    uint32_t sp = getRegU32(&invocation.context, 29) & ~0xFu;

    // Synthetic invocation threads have no stack: their context is all zeroes.
    // The EE execution stack still belongs to the last real guest thread, so
    // clamp against that thread instead of trusting the recorded $sp.
    const GuestThread *stackOwner = &owner;
    if (owner.id < 0)
    {
        const auto it = m_threads.find(g_lastGuestStackThreadId);
        if (it != m_threads.end())
        {
            stackOwner = &it->second;
        }
    }
    const uint32_t liveSp = threadStackSp(stackOwner);
    const bool liveSpValid = liveSp != 0u;

    // A guest stack grows downwards: every live frame sits above $sp.  A
    // handler that starts at or above $sp therefore executes on top of the
    // interrupted code's frames and overwrites them (observed: a VBlank/timer
    // interrupt ran the game's per-frame worker at $sp=0x01fffff0 while the
    // loader was 0x2300 bytes deep, zeroing the saved $ra at 0x01ffff40).
    if (liveSpValid && (sp == 0u || sp > liveSp || sp > PS2_RAM_SIZE))
    {
        sp = liveSp;
    }

    if (sp < 0x00100000u || sp > PS2_RAM_SIZE)
    {
        sp = invocationStackTop();
    }

    SET_GPR_U32(&invocation.context, 29, sp);
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }

    // Run callbacks/interrupt handlers on the interrupted thread's stack, the
    // same way the EE kernel does.  Every live guest frame sits above the
    // current $sp, so a handler that allocates downwards cannot clobber it.
    //
    // This is also required for correctness: PS2 threads receive their stack
    // from the top of RDRAM (crt0 asks SetupThread for a block ending at
    // PS2_RAM_SIZE), so a dedicated "async callback" stack carved from the top
    // of RDRAM would overlap the main thread's stack and silently zero out its
    // saved return addresses.
    const GuestThread *stackOwner = owner;
    if (owner->id < 0)
    {
        // Synthetic invocation thread: borrow the last real guest thread's
        // execution stack.
        const auto it = m_threads.find(g_lastGuestStackThreadId);
        if (it != m_threads.end())
        {
            stackOwner = &it->second;
        }
    }
    const uint32_t currentSp = threadStackSp(stackOwner);
    if (currentSp != 0u)
    {
        return currentSp;
    }

    const size_t depth = owner->invocations.size();
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        if (dmac)
        {
            PS2_IF_TRACE({
            static int enableLogs = 0;
            if (enableLogs < 40)
            {
                ++enableLogs;
                std::fprintf(stderr,
                             "[irq-en] dmac handler id=%d cause=%u enabled=%d pc=%08x\n",
                             id, it->second.cause, enabled ? 1 : 0, it->second.handler);
            }
            });
        }
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (dmac)
        {
            PS2_IF_TRACE({
            static int maskLogs = 0;
            const bool changing = ((mask >> cause) & 1u) != (enabled ? 1u : 0u);
            if (changing && maskLogs < 40)
            {
                ++maskLogs;
                std::fprintf(stderr,
                             "[irq-en] dmac cause=%u enabled=%d (mask %08x",
                             cause, enabled ? 1 : 0, mask);
            }
            });
        }
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
        if (dmac)
        {
            PS2_IF_TRACE({
            static int maskLogs2 = 0;
            if (maskLogs2 < 40 && maskLogs2 < maskLogs2 + 1)
            {
                // complement the [irq-en] line with the new mask value
            }
            if (maskLogs2 < 40)
            {
                std::fprintf(stderr, " -> %08x)\n", mask);
                ++maskLogs2;
            }
            });
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        PS2_IF_TRACE({
            static int droppedLogs = 0;
            if (droppedLogs < 30)
            {
                ++droppedLogs;
                std::fprintf(stderr,
                             "[irq-drop] %s cause=%u mask=%08x (bit disabled)\n",
                             dmac ? "dmac" : "intc", cause, mask);
            }
        });
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    if (dmac)
    {
        PS2_IF_TRACE({
        static int dmacDispatchLogs = 0;
        if (dmacDispatchLogs < 40)
        {
            ++dmacDispatchLogs;
            std::fprintf(stderr,
                         "[irq-dmac] cause=%u matched=%zu registered=%zu mask=%08x\n",
                         cause, matching.size(), handlers.size(), mask);
        }
        });
    }
    else
    {
        PS2_IF_TRACE({
        static int intcDispatchLogs = 0;
        if (intcDispatchLogs < 40)
        {
            ++intcDispatchLogs;
            std::fprintf(stderr,
                         "[irq-intc] cause=%u matched=%zu registered=%zu mask=%08x\n",
                         cause, matching.size(), handlers.size(), mask);
        }
        });
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        // EE interrupts execute on the interrupted thread's stack, exactly like
        // the real kernel.  Use the scheduler's live context: m_runtime's copy
        // is only refreshed periodically, so reading it here hands the handler
        // a stale $sp (sometimes the initial top-of-RAM value), which makes it
        // run on top of the interrupted code's live frames.
        uint32_t interruptedSp = 0u;
        if (GuestThread *owner = currentThread())
        {
            if (owner->id >= 0)
            {
                interruptedSp = threadStackSp(owner);
            }
        }
        if (interruptedSp == 0u)
        {
            // Idle/synthetic context: interrupts are delivered on the stack of
            // the last real guest thread (the one that is about to resume).
            const auto it = m_threads.find(g_lastGuestStackThreadId);
            if (it != m_threads.end())
            {
                interruptedSp = threadStackSp(&it->second);
            }
        }
        if (interruptedSp == 0u)
        {
            const uint32_t cachedSp = getRegU32(&m_runtime.m_cpuContext, 29) & ~0xFu;
            if (cachedSp >= 0x00100000u && cachedSp <= PS2_RAM_SIZE)
            {
                interruptedSp = cachedSp;
            }
        }
        if (interruptedSp == 0u)
        {
            // Last resort: run the handler on a dedicated interrupt stack at
            // the top of RAM instead of a zeroed $sp.  A handler started with
            // $sp=0 saves its $ra to guest address 0 and returns to pc==0,
            // killing the invocation (observed with the game's VBlank INTC
            // handler).  1 KiB is plenty for the IRQ handlers in this game and
            // stays clear of the boot-thread scratch value at 0x01FFFF20.
            interruptedSp = PS2_RAM_SIZE - 0x400u;
        }
        SET_GPR_U32(&invocation.context, 29, interruptedSp);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();

    // The EE kernel publishes the flag at the next vertical blank, and guest
    // code usually passes the address of a local variable that it polls right
    // afterwards.  Deferring the store is only safe while the EE is paced to
    // real time: this runtime lets the recompiled EE run many guest frames
    // ahead of the host display, so a deferred store lands in a stack frame
    // that has long been reused (observed: the literal 1 replaced the saved
    // $ra of FUN_002d52a0 at 0x01ffff20 and the guest returned to address 1).
    // Publish the values immediately and never keep a dangling pointer.
    m_vsyncFlagAddress = 0u;
    m_vsyncTickAddress = 0u;

    if (flagAddress != 0u)
    {
        writeGuestU32(flagAddress, 1u);
    }
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    if (item.id >= 0)
    {
        g_lastGuestStackThreadId = item.id;
    }
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                // Fire an event when EITHER its guest-cycle deadline or its
                // real-time host deadline passes.  The old AND logic starved
                // VBlanks whenever the guest ran slower than real time (the
                // host deadline expired seconds before the cycle deadline),
                // freezing the game's vsync counters.
                if ((item.deadlineCycle <= m_eeCycle ||
                     item.hostDeadline <= now) &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                {
                    const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            pacingDeadline - now).count();
                    if (waitMs > 500)
                    {
                        static int slowPaceLogs = 0;
                        if (slowPaceLogs < 12)
                        {
                            ++slowPaceLogs;
                            std::fprintf(stderr,
                                         "[pace-wait] waiting %lld ms (deadlines=%zu events=%zu eeCycle=%llu nextDeadline=%llu)\n",
                                         static_cast<long long>(waitMs), m_deadlines.size(),
                                         m_events.size(),
                                         static_cast<unsigned long long>(m_eeCycle),
                                         static_cast<unsigned long long>(m_nextDeadlineCycle.load()));
                            for (const ScheduledEvent &item : m_deadlines)
                            {
                                const auto itemMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        item.hostDeadline - now).count();
                                std::fprintf(stderr,
                                             "[pace-wait]   event type=%u id=%u dueCycle=%llu (ee- deadline=%lld) hostInMs=%lld\n",
                                             static_cast<unsigned>(item.event.type),
                                             item.event.id,
                                             static_cast<unsigned long long>(item.deadlineCycle),
                                             static_cast<long long>(item.deadlineCycle - m_eeCycle),
                                             static_cast<long long>(itemMs));
                            }
                        }
                    }
                }
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                // NOTE: deliberately fall through instead of returning when
                // woken early.  The partition below only processes events
                // whose cycle AND host deadlines have passed, so an early
                // wake is harmless — but returning here starves cycle-due
                // events: checkpointPending then stays set forever and the
                // scheduler never dispatches guest code again (observed as
                // VBlank starvation while the host render thread queues
                // events at a high rate).

                // Advance the emulated clock by the real time spent waiting:
                // on real hardware the EE keeps running (spinning on vsync
                // waits) while our scheduler sleeps here, so RCNT2 timers and
                // game timeouts must advance at the real-time rate.
                {
                    const auto waitedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - now).count();
                    if (waitedUs > 0)
                    {
                        const uint64_t waitCycles =
                            (static_cast<uint64_t>(waitedUs) * EeScheduler::kEeClockHz + 999999ull) /
                            1000000ull;
                        uint64_t remaining = waitCycles;
                        while (remaining > 0u)
                        {
                            const uint32_t step = static_cast<uint32_t>(
                                std::min<uint64_t>(remaining,
                                                   std::numeric_limits<uint32_t>::max()));
                            accountCycles(step);
                            remaining -= step;
                        }
                    }
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle ||
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // Re-base the follow-up event's cycle deadline to the CURRENT
                // m_eeCycle: the guest can outrun real time by millions of
                // cycles per VBlank period, and a deadline computed from the
                // old schedule would already be in the past — making the event
                // permanently cycle-due, starving all guest dispatches behind
                // the checkpoint gate (observed as a total wedge at ~128
                // frames with the game's vsync spin loop).
                scheduleEvent(std::max(scheduled.deadlineCycle + kVBlankDurationCycles,
                                       m_eeCycle + kVBlankDurationCycles),
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(std::max(scheduled.deadlineCycle + kVBlankPeriodCycles,
                                       m_eeCycle + kVBlankPeriodCycles),
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        if (m_vsyncFlagAddress != 0u)
        {
            writeGuestU32(m_vsyncFlagAddress, 1u);
        }
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        ps2_stubs::updatePadDma(m_rdram, &m_runtime, m_vsyncTick);
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        m_nextHostDeadlineMs.store(0, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);

    // Also track the earliest HOST deadline: the guest can run far slower
    // than real time inside wait loops, so a scheduled event's host deadline
    // can pass long before its guest-cycle deadline.  checkpointDue must
    // yield the guest when that happens, otherwise real-time events (VBlank
    // pacing) never fire and the game waits forever.
    auto hostMin = m_deadlines.begin()->hostDeadline;
    for (const auto &item : m_deadlines)
    {
        if (item.hostDeadline < hostMin)
        {
            hostMin = item.hostDeadline;
        }
    }
    m_nextHostDeadlineMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            hostMin.time_since_epoch()).count(),
        std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
