#include <jni.h>
#include <stdint.h>

#include "sdl_controller.h"
#include "ns_protocol.h"

// SDL3's Android static library already exports JNI_OnLoad.
// Do not define another JNI_OnLoad here, or the final libnsprotocol.so link
// fails with a duplicate symbol against SDL_android.c. SDL's JNI_OnLoad is
// the one that initializes SDL's Android/JavaVM state before SDL_Init().

// ─── SDL lifecycle ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativeInit(JNIEnv*, jclass) {
    return sdl_controller_init() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativeQuit(JNIEnv*, jclass) {
    sdl_controller_quit();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePoll(JNIEnv*, jclass) {
    sdl_controller_poll();
}

// ─── Gamepad state ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadConnected(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_connected(slot) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_SDLController_nativePadButtons(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).buttons;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadUp(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_up ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadDown(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_down ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadLeft(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_left ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadRight(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_right ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadLX(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).lx;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadLY(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).ly;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadRX(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).rx;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadRY(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).ry;
}

// ─── Controller motion ─────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadHasMotion(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_motion(slot).has_motion ? JNI_TRUE : JNI_FALSE;
}

// Returns motion as [ax, ay, az, gx, gy, gz]
extern "C" JNIEXPORT jshortArray JNICALL
Java_com_nscontrol_SDLController_nativePadMotion(JNIEnv* env, jclass, jint slot) {
    SdlPadMotion m = sdl_controller_pad_motion(slot);
    jshort arr[6] = { (jshort)m.ax, (jshort)m.ay, (jshort)m.az,
                      (jshort)m.gx, (jshort)m.gy, (jshort)m.gz };
    jshortArray out = env->NewShortArray(6);
    if (out) env->SetShortArrayRegion(out, 0, 6, arr);
    return out;
}

// ─── Controller rumble ─────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePadRumble(JNIEnv*, jclass,
                                                  jint slot, jint low, jint high, jint duration_ms) {
    sdl_controller_pad_rumble(slot, (uint8_t)low, (uint8_t)high, (uint32_t)duration_ms);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePadStopRumble(JNIEnv*, jclass, jint slot) {
    sdl_controller_pad_stop_rumble(slot);
}

// ─── Phone sensors ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsOpen(JNIEnv*, jclass) {
    return sdl_controller_phone_sensors_open() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsClose(JNIEnv*, jclass) {
    sdl_controller_phone_sensors_close();
}

extern "C" JNIEXPORT jshortArray JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsRead(JNIEnv* env, jclass) {
    SdlPadMotion m = sdl_controller_phone_sensors_read();
    if (!m.has_motion) return NULL;
    jshort arr[6] = { (jshort)m.ax, (jshort)m.ay, (jshort)m.az,
                      (jshort)m.gx, (jshort)m.gy, (jshort)m.gz };
    jshortArray out = env->NewShortArray(6);
    if (out) env->SetShortArrayRegion(out, 0, 6, arr);
    return out;
}

// ─── Phone haptics ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticOpen(JNIEnv*, jclass) {
    return sdl_controller_phone_haptic_open() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticClose(JNIEnv*, jclass) {
    sdl_controller_phone_haptic_close();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticRumble(JNIEnv*, jclass,
                                                          jint low, jint high) {
    sdl_controller_phone_haptic_rumble((uint8_t)low, (uint8_t)high);
}
