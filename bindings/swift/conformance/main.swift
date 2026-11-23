// Google conformance testee driven by the pure-Swift core: the same wire
// protocol as the other bindings' testees (4-byte little-endian length prefix,
// then a `ConformanceRequest` / `ConformanceResponse`) and the same
// schema-driven shape -- the request names the message type and the descriptor
// set answers with the schema, so there is no per-message code and no C
// library.
//
// Binary in -> binary out is what this core implements. JSON and text requests
// answer `skipped` (the runner does not count a skip as a failure), so the pass
// set is the binary suite, not the JSON/text mappings.
//
//   swift build -c release --product pbwire-conformance
//   .build/release/pbwire-conformance build/conformance.desc
//
// Driven by `make conformance-swift` from the repo root.
import Foundation
import PbwireCore

/// `WireFormat.PROTOBUF`; the only output this core renders.
let protobufFormat: Int64 = 1

func writeFrame(_ payload: [UInt8]) {
    var frame = [UInt8]()
    frame.reserveCapacity(payload.count + 4)
    let size = UInt32(payload.count)
    frame.append(UInt8(truncatingIfNeeded: size))
    frame.append(UInt8(truncatingIfNeeded: size >> 8))
    frame.append(UInt8(truncatingIfNeeded: size >> 16))
    frame.append(UInt8(truncatingIfNeeded: size >> 24))
    frame.append(contentsOf: payload)
    FileHandle.standardOutput.write(Data(frame))
}

func readExactly(_ n: Int) -> [UInt8]? {
    var out = [UInt8]()
    out.reserveCapacity(n)
    while out.count < n {
        let chunk = FileHandle.standardInput.readData(ofLength: n - out.count)
        if chunk.isEmpty { return nil }
        out.append(contentsOf: chunk)
    }
    return out
}

func readFrame() -> [UInt8]? {
    guard let header = readExactly(4) else { return nil }
    let size = Int(header[0]) | Int(header[1]) << 8 | Int(header[2]) << 16 | Int(header[3]) << 24
    return readExactly(size)
}

/// One request in, one response out. Never throws: a failure is a response
/// field, so the runner reports it instead of the testee exiting.
func handle(_ requestType: MessageType, _ responseType: MessageType, _ body: [UInt8]) -> Message {
    let response = responseType.newMessage()
    let request: Message
    do {
        request = try requestType.decode(body)
    } catch let e as CoreError {
        try? response.setStr("runtime_error", "cannot parse ConformanceRequest: \(e.message)")
        return response
    } catch {
        try? response.setStr("runtime_error", "cannot parse ConformanceRequest: \(error)")
        return response
    }
    let wanted = ((try? request.getI64("requested_output_format")) ?? nil) ?? 0
    if wanted != protobufFormat {
        try? response.setStr("skipped", "output format \(wanted) is not supported")
        return response
    }
    if (try? request.has("json_payload")) == true || (try? request.has("text_payload")) == true {
        try? response.setStr(
            "skipped", "JSON and text input are not supported by the pure-Swift core")
        return response
    }
    if (try? request.has("protobuf_payload")) != true {
        try? response.setStr("skipped", "no binary payload")
        return response
    }
    let name = ((try? request.getStr("message_type")) ?? nil) ?? ""
    let ty: MessageType
    do {
        ty = try requestType.schema.message(name)
    } catch let e as CoreError {
        try? response.setStr("runtime_error", e.message)
        return response
    } catch {
        try? response.setStr("runtime_error", "unknown message type \(name)")
        return response
    }
    let payload = ((try? request.getBytes("protobuf_payload")) ?? nil) ?? []
    let message: Message
    do {
        message = try ty.decode(payload)
    } catch let e as CoreError {
        // The runner only reads the text: the message alone is what the
        // reference reports for a parse failure.
        try? response.setStr("parse_error", e.message)
        return response
    } catch {
        try? response.setStr("parse_error", "\(error)")
        return response
    }
    do {
        let bytes = try ty.encode(message)
        // The response carries the bytes under `protobuf_payload`.
        try response.setBytes("protobuf_payload", bytes)
    } catch let e as CoreError {
        try? response.setStr("serialize_error", e.message)
    } catch {
        try? response.setStr("serialize_error", "\(error)")
    }
    return response
}

guard CommandLine.arguments.count > 1 else {
    FileHandle.standardError.write(Data("usage: pbwire-conformance <conformance.desc>\n".utf8))
    exit(2)
}
let schema: Schema
do {
    schema = try Schema(contentsOfFile: CommandLine.arguments[1])
} catch {
    FileHandle.standardError.write(Data("load \(CommandLine.arguments[1]): \(error)\n".utf8))
    exit(1)
}
let requestType: MessageType
let responseType: MessageType
do {
    requestType = try schema.message("conformance.ConformanceRequest")
    responseType = try schema.message("conformance.ConformanceResponse")
} catch {
    FileHandle.standardError.write(Data("schema: \(error)\n".utf8))
    exit(1)
}
while let body = readFrame() {
    let response = handle(requestType, responseType, body)
    let payload: [UInt8]
    do {
        payload = try responseType.encode(response)
    } catch {
        // A response we cannot encode is a bug in the testee, not in the
        // message under test: say so and stop.
        FileHandle.standardError.write(
            Data("cannot encode ConformanceResponse: \(error)\n".utf8))
        exit(1)
    }
    writeFrame(payload)
}
exit(0)
