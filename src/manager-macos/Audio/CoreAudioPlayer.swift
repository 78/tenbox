import AVFoundation
import AudioToolbox

class CoreAudioPlayer {
    private var audioQueue: AudioQueueRef?
    private var buffers: [AudioQueueBufferRef?] = []
    private let bufferCount = 3
    private let bufferSize: UInt32 = 4096
    private var isRunning = false

    private var pendingData = Data()
    private let dataLock = NSLock()

    private var currentSampleRate: Double = 48000.0
    private var currentChannelCount: UInt32 = 2
    private let bitsPerSample: UInt32 = 16

    // Max buffered audio: ~200ms at 48kHz stereo S16 = 48000*2*2*0.2 = 38400 bytes
    private let maxPendingBytes = 38400

    private var audioThread: Thread?
    private var audioRunLoop: CFRunLoop?
    private let runLoopReady = DispatchSemaphore(value: 0)

    private var queueStopped = false
    private var emptyBufferCount = 0
    private let emptyBufferThreshold = 3

    // Signalled to wake the audio thread when it should rebuild the queue
    private let wakeUp = DispatchSemaphore(value: 0)
    private var needsReconfigure = false

    func start() {
        guard !isRunning else { return }
        isRunning = true
        startAudioThread()
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        wakeUp.signal()
        stopAudioThread()
    }

    func enqueuePcmData(_ data: Data, sampleRate: UInt32, channels: UInt32) {
        let rate = Double(sampleRate)
        let ch = UInt32(channels)

        dataLock.lock()
        let reconfigure = (rate != currentSampleRate || ch != currentChannelCount)
        if reconfigure {
            currentSampleRate = rate
            currentChannelCount = ch
            pendingData.removeAll(keepingCapacity: true)
            needsReconfigure = true
        }

        pendingData.append(data)

        // Drop oldest data if buffer exceeds the limit to prevent unbounded growth
        if pendingData.count > maxPendingBytes {
            let excess = pendingData.count - maxPendingBytes
            pendingData.removeFirst(excess)
        }
        let stopped = queueStopped
        dataLock.unlock()

        if stopped || reconfigure {
            wakeUp.signal()
        }
    }

    // MARK: - Dedicated audio thread

    private func startAudioThread() {
        let thread = Thread { [weak self] in
            guard let self = self else { return }
            self.audioRunLoop = CFRunLoopGetCurrent()
            self.runLoopReady.signal()

            while self.isRunning {
                self.setupQueue()
                self.runAudioLoop()
                self.teardownQueue()

                // Sleep until new data arrives or we're told to stop
                if self.isRunning {
                    self.wakeUp.wait()
                }
            }
        }
        thread.name = "CoreAudioPlayer"
        thread.qualityOfService = .userInteractive
        self.audioThread = thread
        thread.start()
        runLoopReady.wait()
    }

    // Spin the RunLoop while AudioQueue is active
    private func runAudioLoop() {
        while self.isRunning && !self.queueStopped {
            dataLock.lock()
            let recfg = needsReconfigure
            dataLock.unlock()
            if recfg { break }
            CFRunLoopRunInMode(.defaultMode, 0.25, true)
        }
    }

    private func stopAudioThread() {
        if let rl = audioRunLoop {
            CFRunLoopStop(rl)
            audioRunLoop = nil
        }
        audioThread = nil
    }

    // MARK: - AudioQueue management

    private func setupQueue() {
        dataLock.lock()
        let rate = currentSampleRate
        let ch = currentChannelCount
        needsReconfigure = false
        queueStopped = false
        emptyBufferCount = 0
        dataLock.unlock()

        var format = AudioStreamBasicDescription(
            mSampleRate: rate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            mBytesPerPacket: ch * (bitsPerSample / 8),
            mFramesPerPacket: 1,
            mBytesPerFrame: ch * (bitsPerSample / 8),
            mChannelsPerFrame: ch,
            mBitsPerChannel: bitsPerSample,
            mReserved: 0
        )

        guard let rl = audioRunLoop else { return }
        let selfPtr = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())

        var queue: AudioQueueRef?
        let status = AudioQueueNewOutput(
            &format,
            audioQueueCallback,
            selfPtr,
            rl,
            CFRunLoopMode.defaultMode.rawValue,
            0,
            &queue
        )

        guard status == noErr, let q = queue else {
            NSLog("CoreAudioPlayer: Failed to create audio queue: %d", status)
            return
        }

        audioQueue = q
        buffers = Array(repeating: nil, count: bufferCount)
        for i in 0..<bufferCount {
            AudioQueueAllocateBuffer(q, bufferSize, &buffers[i])
            if let buffer = buffers[i] {
                fillBuffer(buffer)
                AudioQueueEnqueueBuffer(q, buffer, 0, nil)
            }
        }

        AudioQueueStart(q, nil)
    }

    private func teardownQueue() {
        guard let q = audioQueue else { return }
        AudioQueueStop(q, true)
        AudioQueueDispose(q, true)
        audioQueue = nil
        buffers = []
    }

    // MARK: - Buffer filling

    fileprivate func fillNextBuffer(_ buffer: AudioQueueBufferRef) {
        guard let q = audioQueue else { return }

        dataLock.lock()
        let hasData = !pendingData.isEmpty
        dataLock.unlock()

        if hasData {
            emptyBufferCount = 0
            fillBuffer(buffer)
            AudioQueueEnqueueBuffer(q, buffer, 0, nil)
        } else {
            emptyBufferCount += 1
            if emptyBufferCount >= emptyBufferThreshold && !queueStopped {
                // Mark stopped — runAudioLoop will exit, teardown the queue,
                // and the thread will sleep on the semaphore.
                dataLock.lock()
                queueStopped = true
                dataLock.unlock()
            } else {
                memset(buffer.pointee.mAudioData, 0, Int(bufferSize))
                buffer.pointee.mAudioDataByteSize = bufferSize
                AudioQueueEnqueueBuffer(q, buffer, 0, nil)
            }
        }
    }

    private func fillBuffer(_ buffer: AudioQueueBufferRef) {
        dataLock.lock()
        let bytesToCopy = min(Int(bufferSize), pendingData.count)
        if bytesToCopy > 0 {
            pendingData.withUnsafeBytes { rawPtr in
                buffer.pointee.mAudioData.copyMemory(
                    from: rawPtr.baseAddress!,
                    byteCount: bytesToCopy
                )
            }
            pendingData.removeFirst(bytesToCopy)
            buffer.pointee.mAudioDataByteSize = UInt32(bytesToCopy)
        } else {
            memset(buffer.pointee.mAudioData, 0, Int(bufferSize))
            buffer.pointee.mAudioDataByteSize = bufferSize
        }
        dataLock.unlock()
    }

    deinit {
        stop()
    }
}

private func audioQueueCallback(
    userData: UnsafeMutableRawPointer?,
    queue: AudioQueueRef,
    buffer: AudioQueueBufferRef
) {
    guard let userData = userData else { return }
    let player = Unmanaged<CoreAudioPlayer>.fromOpaque(userData).takeUnretainedValue()
    player.fillNextBuffer(buffer)
}
