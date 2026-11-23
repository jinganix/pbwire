/*
 * Google conformance testee driven by the pbwire Kotlin binding (which is
 * the Java binding's runtime plus the Kotlin DSL, so this is the same
 * schema-driven testee as bindings/java/conformance/Testee.java and
 * bindings/scala/conformance/Testee.scala).
 *
 * Speaks the runner's wire protocol (4-byte little-endian length prefix,
 * then a ConformanceRequest / ConformanceResponse) with no per-message
 * code: the request names the message type, the descriptor set answers
 * with the schema.
 *
 * Supported: binary, JSON and text in -> binary, JSON or text out. JSPB
 * answers `skipped`, like every other testee here.
 *
 * No package: the Java runtime lives in the default package (plain javac,
 * no module path), and code in a named package cannot see it. This file
 * declares none, exactly like the Kotlin runtime.
 *
 *   kotlinc ... conformance/Testee.kt && java -cp out/pbwire.jar TesteeKt build/conformance.desc
 */

// ConformanceRequest.requested_output_format values.
private const val Protobuf = 1
private const val Json = 2
private const val Text = 4
private const val CategoryIgnoreUnknown = 3

// ConformanceResponse field numbers: exactly one result field is set.
private const val ParseError = 1
private const val RuntimeError = 2
private const val ProtobufPayload = 3
private const val JsonPayload = 4
private const val Skipped = 5
private const val SerializeError = 6
private const val TextPayload = 8

fun main(args: Array<String>) {
    if (args.isEmpty()) {
        System.err.println("usage: TesteeKt <conformance.desc>")
        kotlin.system.exitProcess(2)
    }
    val testee = Testee(schema(args[0]))
    try {
        val input = System.`in`
        val out = System.out
        while (true) {
            val body = readRequest(input) ?: return
            writeResponse(out, testee.handle(body))
        }
    } finally {
        testee.close()
    }
}

private fun readRequest(input: java.io.InputStream): ByteArray? {
    val header = input.readNBytes(4)
    if (header.size < 4) return null // clean end of input (or a truncated header)
    val size = (header[0].toInt() and 0xff) or ((header[1].toInt() and 0xff) shl 8) or
        ((header[2].toInt() and 0xff) shl 16) or ((header[3].toInt() and 0xff) shl 24)
    val body = input.readNBytes(size)
    if (body.size < size) return null
    return body
}

private fun writeResponse(out: java.io.OutputStream, payload: ByteArray) {
    val len = payload.size
    out.write(byteArrayOf(
        (len and 0xff).toByte(), ((len shr 8) and 0xff).toByte(),
        ((len shr 16) and 0xff).toByte(), ((len shr 24) and 0xff).toByte()
    ))
    out.write(payload)
    out.flush()
}

class Testee(private val schema: Schema) {
    private val requestType = schema.message("conformance.ConformanceRequest")
    private val responseType = schema.message("conformance.ConformanceResponse")
    private val types = HashMap<String, MessageType>()

    fun close() = schema.close()

    private fun messageOf(name: String): MessageType {
        var t = types[name]
        if (t == null) {
            t = schema.message(name)
            types[name] = t
        }
        return t
    }

    fun handle(body: ByteArray): ByteArray {
        val response = responseType.newMessage()
        val request = try {
            requestType.decode(body)
        } catch (e: RuntimeException) { // malformed framing input, never a crash
            return finish(response.set(RuntimeError,
                "cannot parse ConformanceRequest: " + e.message))
        }
        val wanted = request.getLong(3)
        val category = request.getLong(5)
        val name: String? = request.getString(4)
        val payload: ByteArray? = request.getBytes(1)
        val json: String? = request.getString(2)
        val text: String? = request.getString(8)
        val printUnknown = request.getBool(9)
        if (wanted != Protobuf.toLong() && wanted != Json.toLong() && wanted != Text.toLong()) {
            return finish(response.set(Skipped, "output format $wanted is not supported"))
        }
        val ty = try {
            messageOf(name!!)
        } catch (e: RuntimeException) {
            return finish(response.set(RuntimeError, "unknown message type '$name'"))
        }
        val parsed = try {
            if (json != null) {
                ty.parseJson(json, category == CategoryIgnoreUnknown.toLong(), null)
            } else if (text != null) {
                ty.parseText(text)
            } else if (payload != null) {
                ty.decode(payload)
            } else {
                return finish(response.set(Skipped, "unsupported payload"))
            }
        } catch (e: RuntimeException) { // refused input: verdict, not a bug
            return finish(response.set(ParseError, e.message))
        }
        return try {
            if (wanted == Json.toLong()) {
                finish(response.set(JsonPayload, parsed.toJson()))
            } else if (wanted == Text.toLong()) {
                finish(response.set(TextPayload, parsed.toText(printUnknown)))
            } else {
                finish(response.set(ProtobufPayload, parsed.encode()))
            }
        } catch (e: RuntimeException) {
            finish(response.set(SerializeError, e.message))
        }
    }

    private fun finish(response: Message): ByteArray = try {
        response.encode()
    } catch (e: RuntimeException) {
        responseType.newMessage().set(RuntimeError,
            "cannot encode response: " + e.message).encode()
    }
}
