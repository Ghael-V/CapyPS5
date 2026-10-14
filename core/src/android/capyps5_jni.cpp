#include "android/capyps5_jni.h"

#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "libs/controller.h"

#include <algorithm>
#include <android/log.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#define LOG_TAG "CapyPS5-JNI"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static JavaVM*            g_jvm           = nullptr;
static ANativeWindow*     g_native_window = nullptr;
static std::thread        g_emulator_thread;
static std::atomic<bool>  g_is_running{false};
static std::string        g_files_dir;
static std::string        g_cache_dir;
#if defined(__ANDROID__) && defined(__aarch64__)
#include <malloc.h>
#endif

extern "C" {

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeInit(
    JNIEnv* env, jclass /*clazz*/, jstring filesDir, jstring cacheDir) {
#if defined(__ANDROID__) && defined(__aarch64__)
#if defined(M_BIONIC_SET_HEAP_TAGGING_LEVEL) && defined(M_HEAP_TAGGING_LEVEL_NONE)
	mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#endif
#endif
	if (g_jvm == nullptr && env != nullptr) {
		env->GetJavaVM(&g_jvm);
	}
	if (filesDir != nullptr) {
		const char* cFilesDir = env->GetStringUTFChars(filesDir, nullptr);
		g_files_dir = cFilesDir;
		env->ReleaseStringUTFChars(filesDir, cFilesDir);
	}
	if (cacheDir != nullptr) {
		const char* cCacheDir = env->GetStringUTFChars(cacheDir, nullptr);
		g_cache_dir = cCacheDir;
		env->ReleaseStringUTFChars(cacheDir, cCacheDir);
	}

	Common::VirtualMemory::Init();
	Common::InitializeThreads();

	ALOGI("nativeInit complete. FilesDir: %s, CacheDir: %s",
	      g_files_dir.c_str(), g_cache_dir.c_str());
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetSurface(
    JNIEnv* env, jclass /*clazz*/, jobject surface) {
	if (g_native_window != nullptr) {
		ANativeWindow_release(g_native_window);
		g_native_window = nullptr;
	}
	if (surface != nullptr) {
		g_native_window = ANativeWindow_fromSurface(env, surface);
		ALOGI("nativeSetSurface: Native window attached %p", g_native_window);
	} else {
		ALOGI("nativeSetSurface: Surface detached");
	}
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeDestroySurface(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	if (g_native_window != nullptr) {
		ANativeWindow_release(g_native_window);
		g_native_window = nullptr;
		ALOGI("nativeDestroySurface: Native window released");
	}
}

JNIEXPORT jboolean JNICALL Java_org_capyps5_emulator_NativeBridge_nativeStartGame(
    JNIEnv* env, jclass /*clazz*/, jstring gamePath, jstring elfPath) {
	if (g_is_running.load()) {
		ALOGW("nativeStartGame: Emulator already running");
		return JNI_FALSE;
	}

	std::string game_str;
	std::string elf_str;

	if (gamePath != nullptr) {
		const char* cGame = env->GetStringUTFChars(gamePath, nullptr);
		game_str = cGame;
		env->ReleaseStringUTFChars(gamePath, cGame);
	}

	if (elfPath != nullptr) {
		const char* cElf = env->GetStringUTFChars(elfPath, nullptr);
		elf_str = cElf;
		env->ReleaseStringUTFChars(elfPath, cElf);
	}

	ALOGI("nativeStartGame: game=%s, elf=%s", game_str.c_str(), elf_str.c_str());

	Emulator::RunOptions options;
	options.app0_dir = game_str;
	options.elf      = elf_str.empty() ? (game_str + "/eboot.bin") : elf_str;

	if (!std::filesystem::exists(options.elf)) {
		ALOGW("nativeStartGame: Executable does not exist on disk: %s", options.elf.c_str());
		return JNI_FALSE;
	}

	// Mobile defaults
	options.config.screen_width       = 1280;
	options.config.screen_height      = 720;
	options.config.fullscreen_enabled = true;

	g_is_running.store(true);
	g_emulator_thread = std::thread([options]() {
		ALOGI("Emulator thread started");
		Common::InitializeThreads();
		Emulator::Run(options);
		g_is_running.store(false);
		ALOGI("Emulator thread finished");
	});

	return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeStopGame(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	if (!g_is_running.load()) {
		return;
	}
	ALOGI("nativeStopGame requested");
	g_is_running.store(false);
	if (g_emulator_thread.joinable()) {
		g_emulator_thread.detach();
	}
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSendControllerInput(
    JNIEnv* /*env*/, jclass /*clazz*/, jint buttons, jfloat lx, jfloat ly,
    jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	uint32_t pad_buttons = static_cast<uint32_t>(buttons) & 0xFFFE;
	if (buttons & 1) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;
	if (buttons & (1 << 16)) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;

	int axis_lx = std::clamp(static_cast<int>((lx + 1.0f) * 127.5f), 0, 255);
	int axis_ly = std::clamp(static_cast<int>((ly + 1.0f) * 127.5f), 0, 255);
	int axis_rx = std::clamp(static_cast<int>((rx + 1.0f) * 127.5f), 0, 255);
	int axis_ry = std::clamp(static_cast<int>((ry + 1.0f) * 127.5f), 0, 255);
	int axis_l2 = std::clamp(static_cast<int>(l2 * 255.0f), 0, 255);
	int axis_r2 = std::clamp(static_cast<int>(r2 * 255.0f), 0, 255);

	// Synchronize D-pad and left analog stick so games reading either or both work seamlessly
	if (pad_buttons & Libs::Controller::PAD_BUTTON_RIGHT) {
		axis_lx = 255;
	} else if (pad_buttons & Libs::Controller::PAD_BUTTON_LEFT) {
		axis_lx = 0;
	}
	if (pad_buttons & Libs::Controller::PAD_BUTTON_DOWN) {
		axis_ly = 255;
	} else if (pad_buttons & Libs::Controller::PAD_BUTTON_UP) {
		axis_ly = 0;
	}

	if (axis_lx > 192) pad_buttons |= Libs::Controller::PAD_BUTTON_RIGHT;
	if (axis_lx < 64)  pad_buttons |= Libs::Controller::PAD_BUTTON_LEFT;
	if (axis_ly < 64)  pad_buttons |= Libs::Controller::PAD_BUTTON_UP;
	if (axis_ly > 192) pad_buttons |= Libs::Controller::PAD_BUTTON_DOWN;

	if (pad_buttons != 0) {
		ALOGI("nativeSendControllerInput: pad_buttons=0x%08x (Cross=%d, Circle=%d, Options=%d)",
		      pad_buttons,
		      (pad_buttons & Libs::Controller::PAD_BUTTON_CROSS) ? 1 : 0,
		      (pad_buttons & Libs::Controller::PAD_BUTTON_CIRCLE) ? 1 : 0,
		      (pad_buttons & Libs::Controller::PAD_BUTTON_OPTIONS) ? 1 : 0);
	}

	Libs::Controller::SetState(Libs::Controller::HOST_INPUT_CONTROLLER_ID,
	                           pad_buttons, axis_lx, axis_ly, axis_rx, axis_ry, axis_l2, axis_r2);
}

JNIEXPORT jstring JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetVersion(
    JNIEnv* env, jclass /*clazz*/) {
	const std::string version = "CapyPS5 v0.1.0-alpha (ARM64 Android)";
	return env->NewStringUTF(version.c_str());
}

ANativeWindow* AndroidGetNativeWindow() {
	return g_native_window;
}

} // extern "C"

const std::string& AndroidGetFilesDir() {
	return g_files_dir;
}
