// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "android/capyps5_jni.h"

#include "common/common.h"
#include "common/file.h"
#include "common/debugFlags.h"
#include "common/logging/log.h"
#include "common/passInspector.h"
#include "common/stringUtils.h"
#include "common/telemetry.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "graphics/presentation/systemOverlay.h"
#include "libs/controller.h"

#include <array>
#include <mutex>
#include <algorithm>
#include <SDL3/SDL.h>
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
static AndroidGpuDriver   g_gpu_driver;
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

	Log::SetFatalReportPath(g_files_dir + "/last_error.txt");
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

// Frontend settings for this session (see Common::DebugFlags::SetFrontendOption). Called before
// nativeStartGame; `name` is the property name, e.g. "debug.capy.res_scale".
JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetOption(
    JNIEnv* env, jclass /*clazz*/, jstring name, jlong value) {
	if (name == nullptr) {
		return;
	}
	const char* text = env->GetStringUTFChars(name, nullptr);
	Common::DebugFlags::SetFrontendOption(text, static_cast<int64_t>(value));
	ALOGI("option %s = %lld", text, static_cast<long long>(value));
	env->ReleaseStringUTFChars(name, text);
}

// Called before nativeStartGame. An empty `library` keeps the system Vulkan driver;
// `driverDir` must be app-private storage (dlopen refuses world-writable locations).
JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetGpuDriver(
    JNIEnv* env, jclass /*clazz*/, jstring nativeLibraryDir, jstring driverDir, jstring library) {
	const auto read = [env](jstring value) {
		std::string result;
		if (value != nullptr) {
			const char* text = env->GetStringUTFChars(value, nullptr);
			result = text;
			env->ReleaseStringUTFChars(value, text);
		}
		return result;
	};
	g_gpu_driver.native_library_dir = read(nativeLibraryDir);
	g_gpu_driver.driver_dir         = read(driverDir);
	g_gpu_driver.library            = read(library);
	ALOGI("GPU driver: %s", g_gpu_driver.library.empty()
	                           ? "system"
	                           : (g_gpu_driver.driver_dir + "/" + g_gpu_driver.library).c_str());
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

// Last pad state from the frontend, so the on-screen touchpad click can be merged into it (a
// physical controller sends its whole state on every event and would drop the click).
static std::mutex             g_pad_mutex;
static jint                   g_last_buttons = 0;
static std::array<jfloat, 6>  g_last_axes {};
static bool                   g_touchpad_click = false;

static void SubmitPad(jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2);

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSendControllerInput(
    JNIEnv* /*env*/, jclass /*clazz*/, jint buttons, jfloat lx, jfloat ly,
    jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	std::lock_guard lock(g_pad_mutex);
	g_last_buttons = buttons;
	g_last_axes    = {lx, ly, rx, ry, l2, r2};
	SubmitPad(buttons, lx, ly, rx, ry, l2, r2);
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetTouchpadClick(
    JNIEnv* /*env*/, jclass /*clazz*/, jboolean down) {
	std::lock_guard lock(g_pad_mutex);
	g_touchpad_click = down != JNI_FALSE;
	const auto& a = g_last_axes;
	SubmitPad(g_last_buttons, a[0], a[1], a[2], a[3], a[4], a[5]);
}

// Map the frontend pad state onto the SDL gamepad buttons/axes that the system overlay's
// ImGui navigation consumes (d-pad/left stick to move the focus, cross to activate,
// circle to cancel, triangle to backspace). Mirrors the desktop SDL routing.
static void OverlayRoutePad(jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
	namespace Overlay = Libs::Graphics;
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_SOUTH, (buttons & (1 << 14)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_EAST, (buttons & (1 << 13)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_WEST, (buttons & (1 << 15)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_NORTH, (buttons & (1 << 12)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_DPAD_UP, (buttons & (1 << 4)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (buttons & (1 << 5)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN, (buttons & (1 << 6)) != 0);
	Overlay::OverlayPadButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT, (buttons & (1 << 7)) != 0);
	const auto axis = [](float value) { return std::clamp(value, -1.0f, 1.0f) * 32767.0f; };
	Overlay::OverlayPadAxis(SDL_GAMEPAD_AXIS_LEFTX, axis(lx));
	Overlay::OverlayPadAxis(SDL_GAMEPAD_AXIS_LEFTY, axis(ly));
	Overlay::OverlayPadAxis(SDL_GAMEPAD_AXIS_RIGHTX, axis(rx));
	Overlay::OverlayPadAxis(SDL_GAMEPAD_AXIS_RIGHTY, axis(ry));
}

static void SubmitPad(jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	// While a system dialog (IME keyboard, error dialog) is on screen the pad drives that
	// dialog instead of the game, mirroring the desktop SDL event loop. The transition also
	// clears whatever the game still held from the previous input batch.
	static bool overlay_captured = false;
	const bool  captured         = Libs::Graphics::OverlayCapturesController();
	if (captured != overlay_captured) {
		overlay_captured = captured;
		Libs::Controller::ResetInputState();
	}
	if (captured) {
		OverlayRoutePad(buttons, lx, ly, rx, ry);
		return;
	}
	uint32_t pad_buttons = static_cast<uint32_t>(buttons) & 0xFFFE;
	if (g_touchpad_click) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;
	if (buttons & 1) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;
	if (buttons & (1 << 16)) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;

	int axis_lx = std::clamp(static_cast<int>((lx + 1.0f) * 127.5f), 0, 255);
	int axis_ly = std::clamp(static_cast<int>((ly + 1.0f) * 127.5f), 0, 255);
	int axis_rx = std::clamp(static_cast<int>((rx + 1.0f) * 127.5f), 0, 255);
	int axis_ry = std::clamp(static_cast<int>((ry + 1.0f) * 127.5f), 0, 255);
	int axis_l2 = std::clamp(static_cast<int>(l2 * 255.0f), 0, 255);
	int axis_r2 = std::clamp(static_cast<int>(r2 * 255.0f), 0, 255);

	// The d-pad and the left stick stay independent, as on a real DualSense: mirroring one
	// into the other made the stick press d-pad buttons in games that use both.
	Libs::Controller::SetState(Libs::Controller::HOST_INPUT_CONTROLLER_ID,
	                           pad_buttons, axis_lx, axis_ly, axis_rx, axis_ry, axis_l2, axis_r2);
}

// On-screen touchpad: finger 0 or 1, coordinates normalized to the pad (0..1).
JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSendTouch(
    JNIEnv* /*env*/, jclass /*clazz*/, jint finger, jboolean down, jfloat x, jfloat y) {
	Libs::Controller::SetTouchPad(Libs::Controller::HOST_INPUT_CONTROLLER_ID, finger, down != JNI_FALSE, x, y);
}

// Layout must match NativeBridge.STAT_* in Kotlin.
JNIEXPORT jlongArray JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetStats(
    JNIEnv* env, jclass /*clazz*/) {
	using namespace Common::Telemetry;
	const jlong values[] = {
	    static_cast<jlong>(g_game_frames.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_host_presents.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_max_frame_ns.exchange(0, std::memory_order_relaxed)),
	    static_cast<jlong>(g_wait_reg_mem_timeouts.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_wait_reg_mem_unmapped.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_null_buffer_fallbacks.load(std::memory_order_relaxed)),
	    g_is_running.load() ? 1 : 0,
	};
	constexpr auto count = static_cast<jsize>(sizeof(values) / sizeof(values[0]));
	jlongArray     array = env->NewLongArray(count);
	if (array != nullptr) {
		env->SetLongArrayRegion(array, 0, count, values);
	}
	return array;
}

// Five longs per pass: pixel shader address, width << 32 | height, draws, targets, skipped.
JNIEXPORT jlongArray JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetPasses(
    JNIEnv* env, jclass /*clazz*/) {
	const auto         passes = Common::PassInspector::Snapshot();
	std::vector<jlong> values;
	values.reserve(passes.size() * 5);
	for (const auto& [pass, skipped]: passes) {
		values.push_back(static_cast<jlong>(pass.pixel_shader));
		values.push_back(static_cast<jlong>((uint64_t {pass.width} << 32u) | pass.height));
		values.push_back(pass.draws);
		values.push_back(pass.targets);
		values.push_back(skipped ? 1 : 0);
	}
	jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
	if (array != nullptr && !values.empty()) {
		env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
	}
	return array;
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeSetPassSkipped(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong pixel_shader, jboolean skipped) {
	Common::PassInspector::SetSkipped(static_cast<uint64_t>(pixel_shader), skipped == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeClearSkippedPasses(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	Common::PassInspector::ClearSkipped();
}

JNIEXPORT jstring JNICALL Java_org_capyps5_emulator_NativeBridge_nativeGetVersion(
    JNIEnv* env, jclass /*clazz*/) {
	const std::string version = "CapyPS5 v0.0.4-alpha (ARM64 Android)";
	return env->NewStringUTF(version.c_str());
}

// --- System overlay (IME keyboard, system dialogs) -------------------------------------------
// The desktop build feeds the overlay through the SDL event loop; on Android the frontend
// sends touch/gamepad/text directly. Mouse coordinates are pixels in the game surface, which
// is what the overlay's ImGui uses as display size (the swapchain extent).

JNIEXPORT jboolean JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayActive(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	return Libs::Graphics::OverlayIsActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayText(
    JNIEnv* env, jclass /*clazz*/) {
        const std::u16string text = Libs::Graphics::OverlayGetText();
        if (text.empty()) {
                return env->NewStringUTF("");
        }
	return env->NewString(reinterpret_cast<const jchar*>(text.data()),
	                      static_cast<jsize>(text.size()));
}

JNIEXPORT jstring JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayTitle(
    JNIEnv* env, jclass /*clazz*/) {
        const std::u16string title = Libs::Graphics::OverlayGetTitle();
        if (title.empty()) {
                return env->NewStringUTF("");
        }
	return env->NewString(reinterpret_cast<const jchar*>(title.data()),
	                      static_cast<jsize>(title.size()));
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayMouse(
    JNIEnv* /*env*/, jclass /*clazz*/, jfloat x, jfloat y, jboolean down) {
	Libs::Graphics::OverlayMouse(x, y, down == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlaySetText(
    JNIEnv* env, jclass /*clazz*/, jstring text) {
	if (text == nullptr) {
	        return;
	}
	const jchar* chars = env->GetStringChars(text, nullptr);
	const jsize  len   = env->GetStringLength(text);
	Libs::Graphics::OverlaySetText(
	    std::u16string_view(reinterpret_cast<const char16_t*>(chars), static_cast<size_t>(len)));
	env->ReleaseStringChars(text, chars);
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayAccept(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	Libs::Graphics::OverlayAccept();
}

JNIEXPORT void JNICALL Java_org_capyps5_emulator_NativeBridge_nativeOverlayCancel(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	Libs::Graphics::OverlayCancel();
}

ANativeWindow* AndroidGetNativeWindow() {
	return g_native_window;
}

} // extern "C"

const std::string& AndroidGetFilesDir() {
	return g_files_dir;
}

const AndroidGpuDriver& AndroidGetGpuDriver() {
	return g_gpu_driver;
}
