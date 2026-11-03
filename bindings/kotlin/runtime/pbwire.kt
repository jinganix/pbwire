/*
 * The Kotlin binding: the Java binding's FFI layer (bindings/java/runtime, the
 * FFM downcalls to the C ABI) plus the DSL sugar official Kotlin support also
 * is -- protoc --kotlin_out generates Kotlin over protobuf-java, not a second
 * runtime. So Kotlin adds no wire code and no second schema path: a
 * FileDescriptorSet is loaded at run time and the C core owns the bytes.
 *
 * Kotlin cannot import from Java's default package, but code in the default
 * package can use it directly, which is why this file declares no package.
 */

/** Loads a descriptor set written by `protoc --descriptor_set_out`. */
fun schema(path: String): Schema = Schema.fromFile(path)

/** Loads descriptor-set bytes; use `--include_imports` so every type resolves. */
fun schema(bytes: ByteArray): Schema = Schema.fromBytes(bytes)

/** A new message, then [block] against it. */
inline fun MessageType.build(block: Message.() -> Unit): Message = newMessage().apply(block)

/** Decodes wire bytes; unknown fields are preserved and re-encoded. */
fun MessageType.parse(wire: ByteArray): Message = decode(wire)

/** A nested message as the receiver, e.g. `message("inner") { set("name", "kid") }`. */
inline fun Message.message(name: String, block: Message.() -> Unit): Message =
    newMessage(name).apply(block)
