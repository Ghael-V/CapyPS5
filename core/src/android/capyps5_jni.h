// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef CAPYPS5_ANDROID_JNI_H_
#define CAPYPS5_ANDROID_JNI_H_

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#ifdef __cplusplus
extern "C" {
#endif


JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeInit(
    JNIEnv* env, jclass clazz, jstring filesDir, jstring cacheDir);

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetSurface(
    JNIEnv* env, jclass clazz, jobject surface);

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeDestroySurface(
    JNIEnv* env, jclass clazz);

JNIEXPORT jboolean JNICALL Java_org_capyps5_emulator_NativeBridge_nativeStartGame(
    JNIEnv* env, jclass clazz, jstring gamePath, jstring elfPath);

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeStopGame(
    JNIEnv* env, jclass clazz);

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSendControllerInput(
    JNIEnv* env, jclass clazz, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry,
    jfloat l2, jfloat r2);

JNIEXPORT jlongArray JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetStats(
    JNIEnv* env, jclass clazz);

JNIEXPORT jstring JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetVersion(
    JNIEnv* env, jclass clazz);

ANativeWindow* AndroidGetNativeWindow();

#ifdef __cplusplus
}

#include <string>
const std::string& AndroidGetFilesDir();

// GPU driver chosen by the frontend (nativeSetGpuDriver): an empty library loads the system
// driver. The library is a file name inside driver_dir, loaded through libadrenotools.
struct AndroidGpuDriver {
	std::string native_library_dir;
	std::string driver_dir;
	std::string library;
};
const AndroidGpuDriver& AndroidGetGpuDriver();
#endif

#endif // CAPYPS5_ANDROID_JNI_H_
