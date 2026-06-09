#include <jni.h>
#include <ns_protocol.h>
#include <string.h>

static uint8_t* jni_get_bytes(JNIEnv* env, jbyteArray arr, jboolean* is_copy) {
    return (uint8_t*)(*env)->GetByteArrayElements(env, arr, is_copy);
}

static void jni_release_bytes(JNIEnv* env, jbyteArray arr, uint8_t* data, int mode) {
    (*env)->ReleaseByteArrayElements(env, arr, (jbyte*)data, mode);
}

static jbyteArray jni_new_byte_array(JNIEnv* env, int size, const uint8_t* data) {
    jbyteArray result = (*env)->NewByteArray(env, size);
    if (result) {
        (*env)->SetByteArrayRegion(env, result, 0, size, (jbyte*)data);
    }
    return result;
}

jint JNICALL
Java_com_nscontrol_NativeProtocol_nativeNormalizeShortcuts(JNIEnv* env, jclass clazz, jint buttons) {
    return (jint)ns_normalize_system_shortcuts((uint16_t)(buttons & 0xFFFF));
}

jshort JNICALL
Java_com_nscontrol_NativeProtocol_nativeGyroDeadzone(JNIEnv* env, jclass clazz, jshort v) {
    return (jshort)ns_gyro_deadzone((int16_t)v);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeNeutralHid(JNIEnv* env, jclass clazz) {
    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    ns_hid_write_neutral(hid);
    return jni_new_byte_array(env, NS_PROTOCOL_HID_SIZE, hid);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeNeutralMotion(JNIEnv* env, jclass clazz) {
    uint8_t motion[NS_PROTOCOL_MOTION_SIZE];
    memset(motion, 0, NS_PROTOCOL_MOTION_SIZE);
    return jni_new_byte_array(env, NS_PROTOCOL_MOTION_SIZE, motion);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeInitFrame(JNIEnv* env, jclass clazz,
                                                  jint flags, jint seq, jlong timestampUs) {
    uint8_t frame[NS_PROTOCOL_WEB_FRAME_SIZE];
    ns_web_frame_init(frame, (uint8_t)(flags & 0xFF), (uint32_t)seq, (uint64_t)timestampUs);
    return jni_new_byte_array(env, NS_PROTOCOL_WEB_FRAME_SIZE, frame);
}

void JNICALL
Java_com_nscontrol_NativeProtocol_nativeSetFrameHid(JNIEnv* env, jclass clazz,
                                                    jbyteArray frame, jint padIndex,
                                                    jbyteArray hid) {
    uint8_t* f = jni_get_bytes(env, frame, NULL);
    uint8_t* h = jni_get_bytes(env, hid, NULL);
    if (f && h) {
        ns_pad_set_hid(f + 20 + (padIndex * NS_PROTOCOL_EXT_PAD_SIZE), h);
    }
    jni_release_bytes(env, hid, h, JNI_ABORT);
    jni_release_bytes(env, frame, f, 0);
}

void JNICALL
Java_com_nscontrol_NativeProtocol_nativeSetFrameMotionSamples(JNIEnv* env, jclass clazz,
                                                              jbyteArray frame, jint padIndex,
                                                              jbyteArray m0, jbyteArray m1,
                                                              jbyteArray m2) {
    uint8_t* f = jni_get_bytes(env, frame, NULL);
    uint8_t* b0 = jni_get_bytes(env, m0, NULL);
    uint8_t* b1 = jni_get_bytes(env, m1, NULL);
    uint8_t* b2 = jni_get_bytes(env, m2, NULL);
    if (f && b0 && b1 && b2) {
        ns_pad_set_motion_samples(f + 20 + (padIndex * NS_PROTOCOL_EXT_PAD_SIZE), b0, b1, b2);
    }
    jni_release_bytes(env, m0, b0, JNI_ABORT);
    jni_release_bytes(env, m1, b1, JNI_ABORT);
    jni_release_bytes(env, m2, b2, JNI_ABORT);
    jni_release_bytes(env, frame, f, 0);
}

void JNICALL
Java_com_nscontrol_NativeProtocol_nativeSetFrameMotion(JNIEnv* env, jclass clazz,
                                                       jbyteArray frame, jint padIndex,
                                                       jbyteArray motion) {
    uint8_t* f = jni_get_bytes(env, frame, NULL);
    uint8_t* m = jni_get_bytes(env, motion, NULL);
    if (f && m) {
        ns_pad_set_motion(f + 20 + (padIndex * NS_PROTOCOL_EXT_PAD_SIZE), m);
    }
    jni_release_bytes(env, motion, m, JNI_ABORT);
    jni_release_bytes(env, frame, f, 0);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeHid(JNIEnv* env, jclass clazz,
                                            jint buttons, jint hat,
                                            jint lx, jint ly, jint rx, jint ry,
                                            jboolean present) {
    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    ns_hid_write(hid, (uint16_t)(buttons & 0xFFFF), (uint8_t)(hat & 0xFF),
                 (uint8_t)(lx & 0xFF), (uint8_t)(ly & 0xFF),
                 (uint8_t)(rx & 0xFF), (uint8_t)(ry & 0xFF),
                 present ? 1 : 0);
    return jni_new_byte_array(env, NS_PROTOCOL_HID_SIZE, hid);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeControllerHid(JNIEnv* env, jclass clazz,
                                                      jint buttons,
                                                      jboolean dpadUp, jboolean dpadDown,
                                                      jboolean dpadLeft, jboolean dpadRight,
                                                      jfloat lx, jfloat ly,
                                                      jfloat rx, jfloat ry,
                                                      jboolean present) {
    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    ns_hid_write_controller(hid, (uint16_t)(buttons & 0xFFFF),
                            dpadUp ? 1 : 0, dpadDown ? 1 : 0,
                            dpadLeft ? 1 : 0, dpadRight ? 1 : 0,
                            lx, ly, rx, ry,
                            present ? 1 : 0);
    return jni_new_byte_array(env, NS_PROTOCOL_HID_SIZE, hid);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeMotionFromValues(JNIEnv* env, jclass clazz,
                                                         jshort ax, jshort ay, jshort az,
                                                         jshort gx, jshort gy, jshort gz,
                                                         jboolean hasMotion) {
    uint8_t motion[NS_PROTOCOL_MOTION_SIZE];
    ns_motion_write_values(motion, (int16_t)ax, (int16_t)ay, (int16_t)az,
                           (int16_t)gx, (int16_t)gy, (int16_t)gz,
                           hasMotion ? 1 : 0);
    return jni_new_byte_array(env, NS_PROTOCOL_MOTION_SIZE, motion);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativePhoneMotion(JNIEnv* env, jclass clazz,
                                                    jfloat accelX, jfloat accelY, jfloat accelZ,
                                                    jfloat gyroX, jfloat gyroY, jfloat gyroZ) {
    uint8_t motion[NS_PROTOCOL_MOTION_SIZE];
    ns_motion_from_android(motion, accelX, accelY, accelZ, gyroX, gyroY, gyroZ);
    return jni_new_byte_array(env, NS_PROTOCOL_MOTION_SIZE, motion);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeMotionFromAndroid(JNIEnv* env, jclass clazz,
                                                          jfloat accelX, jfloat accelY, jfloat accelZ,
                                                          jfloat gyroX, jfloat gyroY, jfloat gyroZ) {
    uint8_t motion[NS_PROTOCOL_MOTION_SIZE];
    ns_motion_from_android(motion, accelX, accelY, accelZ, gyroX, gyroY, gyroZ);
    return jni_new_byte_array(env, NS_PROTOCOL_MOTION_SIZE, motion);
}

void JNICALL
Java_com_nscontrol_NativeProtocol_nativeSetMotionRemap(JNIEnv* env, jclass clazz,
                                                       jint outputAxis, jint inputAxis, jint sign) {
    ns_set_motion_remap((int)outputAxis, (int)inputAxis, (int)sign);
}

jbyteArray JNICALL
Java_com_nscontrol_NativeProtocol_nativeExtractPadHid(JNIEnv* env, jclass clazz,
                                                      jbyteArray frame) {
    jsize len = (*env)->GetArrayLength(env, frame);
    if (len < 20 + NS_PROTOCOL_HID_SIZE) return NULL;

    uint8_t* f = jni_get_bytes(env, frame, NULL);
    if (!f) return NULL;

    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    memcpy(hid, f + 20, NS_PROTOCOL_HID_SIZE);
    jni_release_bytes(env, frame, f, JNI_ABORT);

    return jni_new_byte_array(env, NS_PROTOCOL_HID_SIZE, hid);
}
