//
// Copyright © 2020-2026 Audinate Pty Ltd ACN 120 828 006 (Audinate). All rights reserved.
//
// See DanteAudio.hpp for licence terms.
//
// DanteAudio.cpp -- Non-template implementations for the JUCE loopback Dante backend.
//

#include <dante/DanteAudio.hpp>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Dante {

// ==============================================================================
// Logging
// ==============================================================================

std::string toString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::NONE:    return "None";
    case LogLevel::ERROR:   return "Error";
    case LogLevel::WARNING: return "Warning";
    case LogLevel::INFO:    return "Info";
    case LogLevel::DEBUG:   return "Debug";
    default:                return "Unknown";
    }
}

LogLevel fromString(const std::string & str)
{
    std::string upper = str;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "NONE")    return LogLevel::NONE;
    if (upper == "ERROR")   return LogLevel::ERROR;
    if (upper == "WARNING") return LogLevel::WARNING;
    if (upper == "INFO")    return LogLevel::INFO;
    if (upper == "DEBUG")   return LogLevel::DEBUG;
    return LogLevel::NONE;
}

void PrintfLogger::log(LogLevel level, const char * format, ...)
{
    if (level > mThreshold)
        return;

    char timeBuffer[32];
    auto now      = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tmNow = *std::localtime(&now_c);

    char prefixedFormat[512];
    if (std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &tmNow) != 0)
    {
        std::snprintf(prefixedFormat, sizeof(prefixedFormat),
                      "[%s] %s: %s\n", timeBuffer, toString(level).c_str(), format);
    }
    va_list args;
    va_start(args, format);
    std::vprintf(prefixedFormat, args);
    va_end(args);
}

// ==============================================================================
// BufferView
// ==============================================================================

BufferView::BufferView(IDanteLogger & log, IDanteBuffers & buffers)
    : mLog(log), mBuffers(buffers)
{
    if (!isDanteConnected())
        return;
    mHeaderPtr = mBuffers.getBufferHeader();
    changeState(State::CONNECTED);
}

BufferView::PeriodTimestamp BufferView::getCurrentTimestamp()
{
    PeriodTimestamp ts;
    ts.mPeriodCount = mHeaderPtr->time.period_count;
    ts.mMonotonicNs = mHeaderPtr->time.monotonic;
    return ts;
}

BufferView::PeriodTimestamp BufferView::getTimestampAtomicSafe()
{
    PeriodTimestamp ts, backup;
    uint32_t retries = 0;
    ts = getCurrentTimestamp();
    memory_barrier_acquire();
    bool updating = (mHeaderPtr->metadata.flags & DANTE_BUFFERS_FLAG__TIMING_UPDATE_SYNC) != 0;
    memory_barrier_acquire();
    backup = getCurrentTimestamp();

    while (updating || ts != backup)
    {
        ++retries;
        ts = backup;
        memory_barrier_acquire();
        updating = (mHeaderPtr->metadata.flags & DANTE_BUFFERS_FLAG__TIMING_UPDATE_SYNC) != 0;
        memory_barrier_acquire();
        backup = getCurrentTimestamp();
        if (retries % 100 == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    return ts;
}

bool BufferView::isDanteConnected() const
{
    if (!mBuffers.isConnected())
        return false;
    if (mBuffers.getBufferHeader()->metadata.magic_marker != DANTE_BUFFERS_HDR_MAGIC)
        return false;
    return true;
}

void BufferView::changeState(State newState)
{
    if (newState == State::UNAVAILABLE) { clear(); return; }
    if (newState != State::READY)       mSamplerate = 0;
    mState = newState;
}

void BufferView::clear()
{
    mHeaderPtr         = nullptr;
    mPeriodSizeFrames  = 0;
    mBufferSizeFrames  = 0;
    mNumTxChannels     = 0;
    mNumRxChannels     = 0;
    mTxChannels.clear();
    mRxChannels.clear();
    mSamplerate          = 0;
    mLastPeriodTimestamp = {0, 0};
    mLastSampleTimestamp = {0, 0};
    mState = State::UNAVAILABLE;
}

void BufferView::reset()
{
    mResetCount = mHeaderPtr->metadata.reset_count;
    if (mResetCount & 0x1) { changeState(State::CONNECTED); return; }

    memory_barrier_acquire();

    mSamplerate = mHeaderPtr->audio.sample_rate;
    if (mSamplerate == 0) { changeState(State::CONNECTED); return; }

    changeState(State::READY);

    if (mHeaderPtr->audio.encoding != DANTE_ENCODING__PCM32)
    {
        mLog.log(LogLevel::ERROR, "%s: Unsupported Dante encoding: %u", __func__,
                 mHeaderPtr->audio.encoding);
        changeState(State::UNAVAILABLE);
        return;
    }

    mPeriodSizeFrames = mHeaderPtr->time.samples_per_period;
    mBufferSizeFrames = mHeaderPtr->audio.samples_per_channel;
    mNumTxChannels    = mHeaderPtr->audio.num_tx_channels;
    mNumRxChannels    = mHeaderPtr->audio.num_rx_channels;
    createChannelDescriptions();

    mLastPeriodTimestamp = getTimestampAtomicSafe();
    mLastSampleTimestamp = {mLastPeriodTimestamp.mMonotonicNs,
                            mLastPeriodTimestamp.mPeriodCount * mPeriodSizeFrames};
    memory_barrier_acquire();
}

void BufferView::poll(PollInfo & info)
{
    info.mPeriodTimestamp        = {0, 0};
    info.mSampleTimestamp        = {0, 0};
    info.mSamplesSinceLastUpdate = 0;
    info.mSamplerate             = 0;

    if (!isDanteConnected())
    {
        changeState(State::UNAVAILABLE);
        info.mState = mState;
        info.mReset = true;
        return;
    }
    else if (mState == State::UNAVAILABLE)
    {
        mHeaderPtr = mBuffers.getBufferHeader();
    }

    if (mHeaderPtr->metadata.reset_count != mResetCount || mState != State::READY)
    {
        info.mReset           = true;
        reset();
        info.mPeriodTimestamp = mLastPeriodTimestamp;
        info.mSampleTimestamp = mLastSampleTimestamp;
        info.mState           = mState;
        info.mSamplerate      = mSamplerate;
        return;
    }

    info.mReset      = false;
    info.mState      = mState;
    info.mSamplerate = mSamplerate;

    PeriodTimestamp newTs = getTimestampAtomicSafe();
    info.mPeriodTimestamp = newTs;

    uint64_t curPeriod = newTs.mPeriodCount;
    info.mPeriodsSinceLastUpdate =
        static_cast<unsigned>(curPeriod - mLastPeriodTimestamp.mPeriodCount);

    uint64_t curSample = curPeriod * mPeriodSizeFrames;
    info.mSamplesSinceLastUpdate =
        static_cast<unsigned>(curSample - mLastSampleTimestamp.mSampleCount);

    mLastPeriodTimestamp = newTs;
    mLastSampleTimestamp = {newTs.mMonotonicNs, curSample};
    info.mSampleTimestamp = mLastSampleTimestamp;

    memory_barrier_acquire();
}

unsigned BufferView::calcHeadPosition(PeriodTimestamp periodStamp, unsigned offset)
{
    return static_cast<unsigned>(
        (periodStamp.mPeriodCount * mPeriodSizeFrames + offset) % mBufferSizeFrames);
}

unsigned BufferView::calcHeadPosition(SampleTimestamp sampleStamp, unsigned offset)
{
    return static_cast<unsigned>((sampleStamp.mSampleCount + offset) % mBufferSizeFrames);
}

unsigned BufferView::progressHead(unsigned head, unsigned frames, unsigned bufSize)
{
    unsigned n = head + frames;
    if (n >= bufSize) n -= bufSize;
    return n;
}

unsigned BufferView::progressHead(unsigned head, unsigned frames) const
{
    return progressHead(head, frames, mBufferSizeFrames);
}

void BufferView::createChannelDescriptions()
{
    mTxChannels.clear();
    mRxChannels.clear();
    for (unsigned i = 0; i < mNumTxChannels; ++i)
        mTxChannels.push_back({reinterpret_cast<uint8_t *>(mBuffers.getDanteTxChannel(i)),
                               0, sizeof(int32_t), mBufferSizeFrames});
    for (unsigned i = 0; i < mNumRxChannels; ++i)
        mRxChannels.push_back({reinterpret_cast<uint8_t *>(mBuffers.getDanteRxChannel(i)),
                               0, sizeof(int32_t), mBufferSizeFrames});
}

// ==============================================================================
// BufferBlockAccessor
// ==============================================================================

BufferBlockAccessor::BufferBlockAccessor(BufferView & bufferView, const BlockAccessorConfig & config)
    : mBufferView(bufferView),
      mTxLatencyUs(config.mTxLatencyUs),
      mConfiguredTxLatencySamples(config.mTxLatencySamples)
{
    if (mBufferView.getState() == BufferView::State::READY)
        reset(mBufferView.getCurrentSamplerate());
}

void BufferBlockAccessor::setChannels(const ChannelBlockDescription & tx,
                                       const ChannelBlockDescription & rx)
{
    if (mBufferView.getState() != BufferView::State::READY)
        throw std::runtime_error("Cannot set channels until BufferView is READY");
    if (tx.mNumChannels > 0 &&
        tx.mFirstChannel + tx.mNumChannels > mBufferView.getTxChannelCount())
        throw std::out_of_range("TX channel block is out of range");
    if (rx.mNumChannels > 0 &&
        rx.mFirstChannel + rx.mNumChannels > mBufferView.getRxChannelCount())
        throw std::out_of_range("RX channel block is out of range");

    if (mTxChannels.mNumChannels == 0 && tx.mNumChannels > 0) resetTxClientHead();
    if (mRxChannels.mNumChannels == 0 && rx.mNumChannels > 0) resetRxClientHead();

    mTxChannels = tx;
    mRxChannels = rx;

    if (mTxChannels.mNumChannels == 0) mTxFramesToWrite = 0;
    if (mRxChannels.mNumChannels == 0) mRxAvailable     = 0;
}

void BufferBlockAccessor::setChannels(bool symmetric)
{
    if (mBufferView.getState() != BufferView::State::READY)
        throw std::runtime_error("Cannot set channels until BufferView is READY");

    unsigned numTx = mBufferView.getTxChannelCount();
    unsigned numRx = mBufferView.getRxChannelCount();
    if (symmetric)
    {
        if (numTx < numRx) numRx = numTx;
        else if (numRx < numTx) numTx = numRx;
    }
    setChannels({0, numTx}, {0, numRx});
}

int BufferBlockAccessor::updateAvailable(const BufferView::PollInfo & info)
{
    if (info.mState != BufferView::State::READY)
    {
        mTxFramesToWrite = 0;
        mRxAvailable     = 0;
        return 0;
    }
    if (info.mReset) { reset(info.mSamplerate); return 0; }

    if (info.mSamplesSinceLastUpdate >= mBufferView.getBufferSizeFrames())
    {
        resetClientHeads();
        return -1;
    }

    unsigned lateFrames = 0;
    if (mTxChannels.mNumChannels > 0)
    {
        mTxFramesToWrite += static_cast<int>(info.mSamplesSinceLastUpdate);
        if (mTxFramesToWrite > static_cast<int>(mTxLatencyFrames))
        {
            lateFrames = static_cast<unsigned>(mTxFramesToWrite) - mTxLatencyFrames;
            if (lateFrames > mBufferView.getBufferSizeFrames())
                { resetClientHeads(); return -1; }
        }
    }
    if (mRxChannels.mNumChannels > 0)
    {
        mRxAvailable += info.mSamplesSinceLastUpdate;
        if (mRxAvailable > mBufferView.getBufferSizeFrames())
            { resetClientHeads(); return -1; }
    }
    return lateFrames;
}

void BufferBlockAccessor::resetTxClientHead()
{
    mClientTxHead    = mBufferView.calcHeadPosition(mBufferView.getLatestSampleTimestamp(),
                                                     mTxLatencyFrames);
    mTxFramesToWrite = 0;
}

void BufferBlockAccessor::resetRxClientHead()
{
    mClientRxHead = mBufferView.calcHeadPosition(mBufferView.getLatestSampleTimestamp(), 0);
    mRxAvailable  = 0;
}

void BufferBlockAccessor::resetClientHeads()
{
    if (mTxChannels.mNumChannels > 0) resetTxClientHead();
    if (mRxChannels.mNumChannels > 0) resetRxClientHead();
}

void BufferBlockAccessor::reset(unsigned sampleRate)
{
    mTxFramesToWrite = 0;
    mRxAvailable     = 0;

    mTxLatencyFrames = (mConfiguredTxLatencySamples > 0)
        ? mConfiguredTxLatencySamples
        : static_cast<unsigned>((static_cast<uint64_t>(mTxLatencyUs) * sampleRate) / 1000000);

    resetClientHeads();
}

unsigned BufferBlockAccessor::clampTxFramesToTransfer(unsigned n) const
{
    unsigned maxBuffered  = mBufferView.getBufferSizeFrames()
                          - mTxLatencyFrames
                          - mBufferView.getPeriodSizeFrames();
    int afterTransfer = static_cast<int>(n) - mTxFramesToWrite;
    if (afterTransfer > static_cast<int>(maxBuffered))
        n -= static_cast<unsigned>(afterTransfer) - maxBuffered;
    return n;
}

// ==============================================================================
// DefaultBufferContext
// ==============================================================================

// Waits for the next DEP period via FUTEX_WAIT on period_count.
// dep_sync_fanoutd is the sole sem_wait consumer and issues FUTEX_WAKE after each post.
// FUTEX_WAIT without FUTEX_PRIVATE_FLAG uses the physical page as key, enabling
// cross-process wakeup. The 100 ms timeout keeps the inactive-timeout check responsive.
static void futex_wait_period(const volatile uint64_t * period_count)
{
    uint32_t seen = static_cast<uint32_t>(*period_count);
    struct timespec ts { 0, 100 * 1000000L };  // 100 ms
    syscall(SYS_futex,
            // cast away volatile and const — FUTEX_WAIT only reads the word
            reinterpret_cast<uint32_t *>(const_cast<uint64_t *>(
                const_cast<const uint64_t *>(period_count))),
            FUTEX_WAIT, seen, &ts, nullptr, 0);
    // EAGAIN:    period_count already changed — fall through and process immediately
    // ETIMEDOUT: no wake in 100 ms — fall through and retry
    // EINTR:     signal — fall through and retry
}

DefaultBufferContext::DefaultBufferContext(int inactiveTimeoutMs, bool /*ddhiTelemetry*/)
    : mLog(std::make_shared<PrintfLogger>(LogLevel::WARNING)),
      mBufferAdapter(mBuffers),
      mBufferView(*mLog, mBufferAdapter),
      mInactiveTimeoutMs(inactiveTimeoutMs)
{}

DefaultBufferContext::DefaultBufferContext(std::shared_ptr<IDanteLogger> log,
                                           int  inactiveTimeoutMs,
                                           bool /*ddhiTelemetry*/)
    : mLog(log ? log : std::make_shared<PrintfLogger>(LogLevel::WARNING)),
      mBufferAdapter(mBuffers),
      mBufferView(*mLog, mBufferAdapter),
      mInactiveTimeoutMs(inactiveTimeoutMs)
{}

int DefaultBufferContext::connect(const std::string & shmName, bool globalNamespace, int timeoutSecs)
{
    return connect([this, shmName, globalNamespace]()
        { return mBuffers.connect(shmName, globalNamespace); },
        timeoutSecs);
}

BufferView::PollInfo DefaultBufferContext::disconnect()
{
    mBuffers.disconnect();
    BufferView::PollInfo info;
    mBufferView.poll(info);
    for (auto & acc : mBlockAccessors)
        acc->updateAvailable(info);
    return info;
}

DefaultBufferContext::WaitResult DefaultBufferContext::wait()
{
    WaitResult result;
    if (!isConnected())
        throw std::runtime_error("Dante buffers not connected");

    const volatile uint64_t * period_count = &mBuffers.getHeader()->time.period_count;
    auto endTime = mLastActiveTime + std::chrono::milliseconds(mInactiveTimeoutMs);
    do
    {
        futex_wait_period(period_count);
        mBufferView.poll(result.pollInfo);
    }
    while (result.pollInfo.mState == BufferView::State::CONNECTED
           && (mInactiveTimeoutMs < 0 || std::chrono::steady_clock::now() < endTime));

    if (result.pollInfo.mState == BufferView::State::UNAVAILABLE)
        { result.pollInfo = disconnect(); return result; }

    if (result.pollInfo.mState == BufferView::State::CONNECTED)
    {
        mLog->log(LogLevel::WARNING,
                  "Dante buffers did not become ready within %d ms, disconnecting...",
                  mInactiveTimeoutMs);
        result.pollInfo = disconnect();
        return result;
    }

    if (result.pollInfo.mSamplesSinceLastUpdate == 0)
    {
        if (mInactiveTimeoutMs >= 0 && std::chrono::steady_clock::now() >= endTime)
        {
            mLog->log(LogLevel::WARNING,
                      "Dante buffers inactive for %d ms, disconnecting...",
                      mInactiveTimeoutMs);
            result.pollInfo = disconnect();
            return result;
        }
        if (!result.pollInfo.mReset)
            return result;
    }

    mLastActiveTime = std::chrono::steady_clock::now();

    for (auto & acc : mBlockAccessors)
    {
        int late = acc->updateAvailable(result.pollInfo);
        if (late != 0)
            result.accessorLateValues.push_back({acc, late});
    }
    return result;
}

int DefaultBufferContext::connect(std::function<int()> connectFn, int timeoutSecs)
{
    int err = 0;
    const auto start = std::chrono::steady_clock::now();
    while (true)
    {
        err = connectFn();
        if (err == 0) break;

        mLog->log(LogLevel::WARNING,
                  "Failed to connect to Dante buffers: %s. Retrying...",
                  SharedMemory::getErrorMessage(err).c_str());

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (timeoutSecs >= 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeoutSecs)
        {
            mLog->log(LogLevel::ERROR, "Timed out trying to connect to Dante buffers");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    mLastActiveTime = std::chrono::steady_clock::now();
    return 0;
}

} // namespace Dante
