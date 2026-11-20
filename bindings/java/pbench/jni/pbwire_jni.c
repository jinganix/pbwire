/*
 * Native side of the Java pbench: four JNI methods that drive the C core.
 * This is the "C core through the host FFI" candidate, not a JNI binding --
 * it never translates a Java tree into a C tree, so it is the ceiling such a
 * binding would sit under. See README.md for the contract and the numbers.
 *
 * Built by pbench.sh against build/libpbwire.a; nothing links this in the
 * shipped binding.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"

#define TIERS 4

static pbw_schema *g_schema = NULL;
static const pbw_descriptor *g_desc = NULL;
/* One prebuilt tree per tier: the encode op re-encodes a tree that already
 * exists, which is the same shape the pure-Java side measures. */
static pbw_message *g_msg[TIERS] = {0, 0, 0, 0};

static void
throw_illegal(JNIEnv *env, const char *what)
{
    jclass ex = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (ex)
        (*env)->ThrowNew(env, ex, what);
}

JNIEXPORT void JNICALL
Java_JniCandidate_nativeInit(JNIEnv *env, jclass cls,
                             jbyteArray descSet)
{
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, descSet);
    jbyte *p = (*env)->GetByteArrayElements(env, descSet, NULL);
    if (!p)
        return;
    pbw_schema *s = NULL;
    pbw_status st = pbw_descriptor_set_load(p, (size_t)n, &s);
    (*env)->ReleaseByteArrayElements(env, descSet, p, JNI_ABORT);
    if (st != PBW_OK) {
        throw_illegal(env, "descriptor_set_load failed");
        return;
    }
    g_schema = s;
    g_desc = pbw_schema_find(s, "structtest.Scalars3");
    if (!g_desc)
        throw_illegal(env, "structtest.Scalars3 not found");
}

JNIEXPORT void JNICALL
Java_JniCandidate_nativeSetRef(JNIEnv *env, jclass cls,
                               jint tier, jbyteArray wire)
{
    (void)cls;
    if (tier < 0 || tier >= TIERS || !g_desc)
        return;
    jsize n = (*env)->GetArrayLength(env, wire);
    jbyte *p = (*env)->GetByteArrayElements(env, wire, NULL);
    if (!p)
        return;
    pbw_message *m = NULL;
    pbw_status st = pbw_decode(g_desc, p, (size_t)n, &m);
    (*env)->ReleaseByteArrayElements(env, wire, p, JNI_ABORT);
    if (st != PBW_OK) {
        throw_illegal(env, "decode failed");
        return;
    }
    if (g_msg[tier])
        pbw_message_free(g_msg[tier]);
    g_msg[tier] = m;
}

JNIEXPORT jbyteArray JNICALL
Java_JniCandidate_nativeEncode(JNIEnv *env, jclass cls,
                               jint tier)
{
    (void)cls;
    pbw_message *m = (tier >= 0 && tier < TIERS) ? g_msg[tier] : NULL;
    if (!m)
        return NULL;
    uint8_t *out = NULL;
    size_t len = 0;
    if (pbw_encode(m, &out, &len) != PBW_OK)
        return NULL;
    jbyteArray r = (*env)->NewByteArray(env, (jsize)len);
    if (r)
        (*env)->SetByteArrayRegion(env, r, 0, (jsize)len, (const jbyte *)out);
    pbw_free(out);
    return r;
}

/* Decode + touch + free, all inside the call, so no Java object is created:
 * the number returned only proves the tree was live and readable. */
JNIEXPORT jint JNICALL
Java_JniCandidate_nativeDecode(JNIEnv *env, jclass cls,
                               jbyteArray wire)
{
    (void)cls;
    if (!g_desc)
        return -1;
    jsize n = (*env)->GetArrayLength(env, wire);
    jbyte *p = (*env)->GetByteArrayElements(env, wire, NULL);
    if (!p)
        return -1;
    pbw_message *m = NULL;
    pbw_status st = pbw_decode(g_desc, p, (size_t)n, &m);
    (*env)->ReleaseByteArrayElements(env, wire, p, JNI_ABORT);
    if (st != PBW_OK)
        return -1;
    size_t count = pbw_message_field_count(m);
    uint32_t fnums[32];
    const pbw_value *vals[32];
    size_t np = pbw_message_present_list(m, fnums, vals, 32);
    jint sum = (jint)(count * 31 + np + pbw_message_unknown_len(m));
    pbw_message_free(m);
    return sum;
}
