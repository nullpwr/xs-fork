#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "xs_embed.h"

JNIEXPORT jstring JNICALL
Java_org_xslang_XS_eval(JNIEnv *env, jobject self, jstring jsrc) {
    (void)self;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    char *out = xs_eval_cstr(src);
    (*env)->ReleaseStringUTFChars(env, jsrc, src);
    jstring result = (*env)->NewStringUTF(env, out ? out : "");
    free(out);
    return result;
}

JNIEXPORT jint JNICALL
Java_org_xslang_XS_runBytecode(JNIEnv *env, jobject self, jbyteArray jbytes) {
    (void)self;
    jsize len = (*env)->GetArrayLength(env, jbytes);
    jbyte *body = (*env)->GetByteArrayElements(env, jbytes, NULL);
    int rc = xs_run_bytecode(NULL, (const uint8_t *)body, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, jbytes, body, JNI_ABORT);
    return (jint)rc;
}
