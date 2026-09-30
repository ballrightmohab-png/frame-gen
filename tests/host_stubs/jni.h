#pragma once

#include <cstdint>

using jboolean = unsigned char;
using jint = std::int32_t;
using jfloat = float;

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_OK 0
#define JNI_EDETACHED (-2)
#define JNI_VERSION_1_6 0x00010006

struct _jobject {};
struct _jclass : public _jobject {};
struct _jstring : public _jobject {};
struct _jfieldID;
struct _jmethodID;

using jobject = _jobject *;
using jclass = _jclass *;
using jstring = _jstring *;
using jfieldID = _jfieldID *;
using jmethodID = _jmethodID *;

struct JNIEnv {
    jclass FindClass(const char *) { return nullptr; }
    jclass GetObjectClass(jobject) { return nullptr; }
    jobject NewGlobalRef(jobject) { return nullptr; }
    void DeleteLocalRef(jobject) {}
    jboolean ExceptionCheck() { return JNI_FALSE; }
    void ExceptionClear() {}
    jmethodID GetStaticMethodID(jclass, const char *, const char *) { return nullptr; }
    jmethodID GetMethodID(jclass, const char *, const char *) { return nullptr; }
    jfieldID GetFieldID(jclass, const char *, const char *) { return nullptr; }
    jobject CallStaticObjectMethod(jclass, jmethodID, ...) { return nullptr; }
    jobject CallObjectMethod(jobject, jmethodID, ...) { return nullptr; }
    jboolean CallBooleanMethod(jobject, jmethodID, ...) { return JNI_FALSE; }
    jint CallIntMethod(jobject, jmethodID, ...) { return 0; }
    void CallVoidMethod(jobject, jmethodID, ...) {}
    jobject GetObjectField(jobject, jfieldID) { return nullptr; }
    jint GetIntField(jobject, jfieldID) { return 0; }
    jfloat GetFloatField(jobject, jfieldID) { return 0.0f; }
    jstring NewStringUTF(const char *) { return nullptr; }
};

struct _JavaVM {
    jint GetEnv(void **, jint) { return JNI_OK; }
    jint AttachCurrentThread(JNIEnv **, void *) { return JNI_OK; }
    jint DetachCurrentThread() { return JNI_OK; }
};
using JavaVM = _JavaVM;
