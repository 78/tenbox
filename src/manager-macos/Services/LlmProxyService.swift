import Foundation
import Network

final class LlmProxyService {
    private var mappings: [LlmModelMapping] = []
    private let mappingsLock = NSLock()
    private var listener: NWListener?
    private var connections: [ObjectIdentifier: NWConnection] = [:]
    private let queue = DispatchQueue(label: "com.tenbox.llm-proxy", qos: .userInitiated)
    private(set) var listeningPort: UInt16 = 0

    func updateMappings(_ newMappings: [LlmModelMapping]) {
        mappingsLock.lock()
        mappings = newMappings
        mappingsLock.unlock()
    }

    @discardableResult
    func start() -> Bool {
        if listener != nil { return true }

        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: .any)
        (params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options)?.noDelay = true

        guard let nwListener = try? NWListener(using: params) else {
            NSLog("[llm-proxy] Failed to create NWListener")
            return false
        }

        let ready = DispatchSemaphore(value: 0)
        var started = false

        nwListener.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                if let port = nwListener.port {
                    self?.listeningPort = port.rawValue
                    NSLog("[llm-proxy] Started on 127.0.0.1:%u", port.rawValue)
                }
                started = true
                ready.signal()
            case .failed(let error):
                NSLog("[llm-proxy] Listener failed: %@", error.localizedDescription)
                ready.signal()
            default:
                break
            }
        }

        nwListener.newConnectionHandler = { [weak self] conn in
            self?.acceptConnection(conn)
        }

        nwListener.start(queue: queue)
        ready.wait()
        if started {
            listener = nwListener
        }
        return started
    }

    func stop() {
        listener?.cancel()
        listener = nil
        for conn in connections.values {
            conn.cancel()
        }
        connections.removeAll()
        listeningPort = 0
        NSLog("[llm-proxy] Stopped")
    }

    // MARK: - Connection handling

    private func acceptConnection(_ conn: NWConnection) {
        let id = ObjectIdentifier(conn)
        connections[id] = conn

        conn.stateUpdateHandler = { [weak self] state in
            if case .cancelled = state { self?.connections.removeValue(forKey: id) }
            if case .failed = state { self?.connections.removeValue(forKey: id) }
        }
        conn.start(queue: queue)
        readRequest(conn)
    }

    private func readRequest(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, isComplete, error in
            guard let self = self, let data = data, !data.isEmpty else {
                conn.cancel()
                return
            }
            self.handleRawData(conn, initialData: data)
        }
    }

    private func handleRawData(_ conn: NWConnection, initialData: Data) {
        var buffer = initialData

        func tryParse() {
            if let range = buffer.range(of: Data("\r\n\r\n".utf8)) {
                let headerEnd = range.upperBound
                let headerData = buffer[buffer.startIndex..<range.lowerBound]
                guard let headerStr = String(data: headerData, encoding: .utf8) else {
                    conn.cancel(); return
                }

                let lines = headerStr.components(separatedBy: "\r\n")
                guard let requestLine = lines.first else { conn.cancel(); return }
                let parts = requestLine.split(separator: " ", maxSplits: 2)
                guard parts.count >= 2 else { conn.cancel(); return }
                let method = String(parts[0])
                let path = String(parts[1])

                var contentLength = 0
                var keepAlive = true
                for line in lines.dropFirst() {
                    let lower = line.lowercased()
                    if lower.hasPrefix("content-length:") {
                        contentLength = Int(line.dropFirst("content-length:".count)
                            .trimmingCharacters(in: .whitespaces)) ?? 0
                    } else if lower.hasPrefix("connection:") {
                        if line.dropFirst("connection:".count)
                            .trimmingCharacters(in: .whitespaces).lowercased() == "close" {
                            keepAlive = false
                        }
                    }
                }

                let bodyAlreadyRead = buffer.count - headerEnd
                let remaining = contentLength - bodyAlreadyRead

                if remaining <= 0 {
                    let body = buffer[headerEnd..<(headerEnd + contentLength)]
                    buffer.removeSubrange(buffer.startIndex..<(headerEnd + contentLength))
                    self.dispatchRequest(conn, method: method, path: path,
                                         body: Data(body), keepAlive: keepAlive)
                } else {
                    readRemainingBody(conn, buffer: buffer, headerEnd: headerEnd,
                                      contentLength: contentLength, method: method,
                                      path: path, keepAlive: keepAlive)
                }
            } else {
                conn.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, _, _ in
                    guard self != nil, let data = data, !data.isEmpty else {
                        conn.cancel(); return
                    }
                    buffer.append(data)
                    tryParse()
                }
            }
        }

        tryParse()
    }

    private func readRemainingBody(_ conn: NWConnection, buffer: Data, headerEnd: Int,
                                   contentLength: Int, method: String, path: String,
                                   keepAlive: Bool) {
        var buf = buffer
        let needed = contentLength - (buf.count - headerEnd)
        if needed <= 0 {
            let body = buf[headerEnd..<(headerEnd + contentLength)]
            dispatchRequest(conn, method: method, path: path, body: Data(body), keepAlive: keepAlive)
            return
        }

        conn.receive(minimumIncompleteLength: 1, maximumLength: max(needed, 8192)) { [weak self] data, _, _, _ in
            guard let self = self, let data = data, !data.isEmpty else {
                conn.cancel(); return
            }
            buf.append(data)
            self.readRemainingBody(conn, buffer: buf, headerEnd: headerEnd,
                                   contentLength: contentLength, method: method,
                                   path: path, keepAlive: keepAlive)
        }
    }

    // MARK: - Request dispatch

    private func dispatchRequest(_ conn: NWConnection, method: String, path: String,
                                 body: Data, keepAlive: Bool) {
        if method == "POST" && (path == "/v1/chat/completions" || path == "/chat/completions") {
            handleChatCompletions(conn, body: body, keepAlive: keepAlive)
        } else if method == "GET" && (path == "/v1/models" || path == "/models") {
            handleModels(conn, keepAlive: keepAlive)
        } else {
            sendError(conn, status: 404, statusText: "Not Found",
                      message: "Unknown endpoint: \(path)", keepAlive: keepAlive)
        }
    }

    // MARK: - Route handlers

    private func handleChatCompletions(_ conn: NWConnection, body: Data, keepAlive: Bool) {
        guard var json = try? JSONSerialization.jsonObject(with: body) as? [String: Any] else {
            sendError(conn, status: 400, statusText: "Bad Request",
                      message: "Invalid JSON body", keepAlive: keepAlive)
            return
        }
        guard let modelName = json["model"] as? String, !modelName.isEmpty else {
            sendError(conn, status: 400, statusText: "Bad Request",
                      message: "Missing 'model' field", keepAlive: keepAlive)
            return
        }
        guard let mapping = findMapping(modelName) else {
            sendError(conn, status: 404, statusText: "Not Found",
                      message: "No mapping configured for model: \(modelName)", keepAlive: keepAlive)
            return
        }

        let isStreaming = json["stream"] as? Bool ?? false
        json["model"] = mapping.model

        guard let modifiedBody = try? JSONSerialization.data(withJSONObject: json) else {
            sendError(conn, status: 500, statusText: "Internal Server Error",
                      message: "Failed to serialize request", keepAlive: keepAlive)
            return
        }

        forwardToUpstream(conn, mapping: mapping, body: modifiedBody,
                          isStreaming: isStreaming, keepAlive: keepAlive)
    }

    private func handleModels(_ conn: NWConnection, keepAlive: Bool) {
        mappingsLock.lock()
        let currentMappings = mappings
        mappingsLock.unlock()

        let modelsList = currentMappings.map { m -> [String: Any] in
            ["id": m.alias, "object": "model", "owned_by": "tenbox-proxy"]
        }
        let response: [String: Any] = ["object": "list", "data": modelsList]
        guard let data = try? JSONSerialization.data(withJSONObject: response) else { return }
        sendResponse(conn, status: 200, statusText: "OK",
                     contentType: "application/json", body: data, keepAlive: keepAlive)
    }

    // MARK: - Upstream forwarding

    private func forwardToUpstream(_ conn: NWConnection, mapping: LlmModelMapping,
                                   body: Data, isStreaming: Bool, keepAlive: Bool) {
        var urlStr = mapping.targetUrl
        if urlStr.hasSuffix("/") { urlStr = String(urlStr.dropLast()) }
        urlStr += "/chat/completions"

        guard let url = URL(string: urlStr) else {
            sendError(conn, status: 502, statusText: "Bad Gateway",
                      message: "Invalid upstream URL", keepAlive: keepAlive)
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if !mapping.apiKey.isEmpty {
            request.setValue("Bearer \(mapping.apiKey)", forHTTPHeaderField: "Authorization")
        }
        if isStreaming {
            request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
        }
        request.httpBody = body

        if isStreaming {
            forwardStreaming(conn, request: request, keepAlive: keepAlive)
        } else {
            forwardNonStreaming(conn, request: request, keepAlive: keepAlive)
        }
    }

    private func forwardNonStreaming(_ conn: NWConnection, request: URLRequest, keepAlive: Bool) {
        let task = URLSession.shared.dataTask(with: request) { [weak self] data, response, _ in
            let statusCode = (response as? HTTPURLResponse)?.statusCode ?? 502
            let body = data ?? Data()
            self?.queue.async {
                self?.sendResponse(conn, status: statusCode, statusText: "OK",
                                   contentType: "application/json", body: body, keepAlive: keepAlive)
            }
        }
        task.resume()
    }

    private func forwardStreaming(_ conn: NWConnection, request: URLRequest, keepAlive: Bool) {
        let delegate = StreamingDelegate(connection: conn, keepAlive: keepAlive, proxyQueue: queue)
        delegate.readNextRequest = { [weak self] in
            self?.readRequest(conn)
        }
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        delegate.session = session
        let task = session.dataTask(with: request)
        task.resume()
    }

    // MARK: - Helpers

    private func findMapping(_ modelName: String) -> LlmModelMapping? {
        mappingsLock.lock()
        defer { mappingsLock.unlock() }
        return mappings.first(where: { $0.alias == modelName })
    }

    private func sendError(_ conn: NWConnection, status: Int, statusText: String,
                           message: String, keepAlive: Bool) {
        let json: [String: Any] = ["error": ["message": message, "type": "proxy_error", "code": status]]
        guard let data = try? JSONSerialization.data(withJSONObject: json) else { return }
        sendResponse(conn, status: status, statusText: statusText,
                     contentType: "application/json", body: data, keepAlive: keepAlive)
    }

    private func sendResponse(_ conn: NWConnection, status: Int, statusText: String,
                              contentType: String, body: Data, keepAlive: Bool) {
        let header = "HTTP/1.1 \(status) \(statusText)\r\n" +
                     "Content-Type: \(contentType)\r\n" +
                     "Content-Length: \(body.count)\r\n" +
                     "Connection: \(keepAlive ? "keep-alive" : "close")\r\n" +
                     "\r\n"
        var payload = Data(header.utf8)
        payload.append(body)
        conn.send(content: payload, completion: .contentProcessed { [weak self] _ in
            if keepAlive {
                self?.readRequest(conn)
            } else {
                conn.cancel()
            }
        })
    }
}

// MARK: - Streaming delegate (upstream → client)

private class StreamingDelegate: NSObject, URLSessionDataDelegate {
    let connection: NWConnection
    let keepAlive: Bool
    let proxyQueue: DispatchQueue
    var readNextRequest: (() -> Void)?
    weak var session: URLSession?
    private var headerSent = false
    private var completed = false
    private var pendingChunks: [Data] = []
    private var isSending = false

    init(connection: NWConnection, keepAlive: Bool, proxyQueue: DispatchQueue) {
        self.connection = connection
        self.keepAlive = keepAlive
        self.proxyQueue = proxyQueue
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask,
                    didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let status = (response as? HTTPURLResponse)?.statusCode ?? 200
        let ct = (response as? HTTPURLResponse)?.value(forHTTPHeaderField: "Content-Type") ?? ""
        let isSSE = ct.lowercased().contains("text/event-stream")

        let respCT = isSSE ? "text/event-stream; charset=utf-8" : ct
        let header = "HTTP/1.1 \(status) OK\r\n" +
                     "Content-Type: \(respCT)\r\n" +
                     (isSSE ? "Cache-Control: no-cache\r\n" : "") +
                     "Transfer-Encoding: chunked\r\n" +
                     "Connection: \(keepAlive ? "keep-alive" : "close")\r\n" +
                     "\r\n"
        enqueueChunk(Data(header.utf8))
        headerSent = true
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard headerSent else { return }
        let chunkHeader = String(data.count, radix: 16) + "\r\n"
        var payload = Data(chunkHeader.utf8)
        payload.append(data)
        payload.append(Data("\r\n".utf8))
        enqueueChunk(payload)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        proxyQueue.async { [self] in
            if headerSent {
                pendingChunks.append(Data("0\r\n\r\n".utf8))
            }
            completed = true
            drainQueue()
        }
    }

    private func enqueueChunk(_ data: Data) {
        proxyQueue.async { [self] in
            pendingChunks.append(data)
            drainQueue()
        }
    }

    private func drainQueue() {
        guard !isSending, !pendingChunks.isEmpty else {
            if !isSending && pendingChunks.isEmpty && completed {
                finish()
            }
            return
        }
        isSending = true
        let data = pendingChunks.removeFirst()
        connection.send(content: data, completion: .contentProcessed { [self] _ in
            proxyQueue.async { [self] in
                isSending = false
                drainQueue()
            }
        })
    }

    private func finish() {
        session?.invalidateAndCancel()
        if keepAlive {
            readNextRequest?()
        } else {
            connection.cancel()
        }
    }
}
