/* Original GameActivityEventsInit's observed input-service JNI contract.
 * The observations model real host events; no Java Activity is constructed. */
#include <jni.h>
#include "../source/jni_bridge.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MOTION "android/view/MotionEvent"
#define KEY "android/view/KeyEvent"

static jint call_int_v(JNIEnv *env, jobject object, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    jint result = (*env)->CallIntMethodV(env, object, method, args);
    va_end(args);
    return result;
}

static jlong call_long_v(JNIEnv *env, jobject object, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    jlong result = (*env)->CallLongMethodV(env, object, method, args);
    va_end(args);
    return result;
}

static jfloat call_float_v(JNIEnv *env, jobject object, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    jfloat result = (*env)->CallFloatMethodV(env, object, method, args);
    va_end(args);
    return result;
}

static jmethodID member(JNIEnv *env, jclass type, const char *name,
                         const char *signature) {
    jmethodID id = (*env)->GetMethodID(env, type, name, signature);
    assert(id && !(*env)->ExceptionCheck(env));
    return id;
}

static void check_int(JNIEnv *env, jobject event, jclass type,
                      const char *name, int32_t expected) {
    assert((*env)->CallIntMethod(env, event,
                                member(env, type, name, "()I")) == expected);
    assert(!(*env)->ExceptionCheck(env));
}

static void check_long(JNIEnv *env, jobject event, jclass type,
                       const char *name, int64_t expected) {
    assert((*env)->CallLongMethod(env, event,
                                 member(env, type, name, "()J")) == expected);
    assert(!(*env)->ExceptionCheck(env));
}

static void expect_index_exception(JNIEnv *env) {
    assert((*env)->ExceptionCheck(env));
    jthrowable pending = (*env)->ExceptionOccurred(env);
    assert(pending);
    jclass actual = (*env)->GetObjectClass(env, pending);
    assert(actual);
    (*env)->ExceptionClear(env);
    jclass expected = (*env)->FindClass(env,
                                        "java/lang/IndexOutOfBoundsException");
    assert(expected && actual == expected);
    (*env)->DeleteLocalRef(env, pending);
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == JNI_OK);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env);
    jclass motion_class = (*env)->FindClass(env, MOTION);
    jclass key_class = (*env)->FindClass(env, KEY);
    assert(motion_class && key_class);

    BSJNIObservedMotionEvent sample = {0};
    sample.device_id = 41;
    sample.source = 0x1002;
    sample.action = 261;
    sample.flags = 2;
    sample.meta_state = 16;
    sample.action_button = 3;
    sample.button_state = 5;
    sample.classification = 1;
    sample.edge_flags = 9;
    sample.down_time_ms = INT64_C(5500000000);
    sample.event_time_ms = INT64_C(5500000120);
    sample.x_precision = 0.5f;
    sample.y_precision = 1.25f;
    sample.pointer_count = 2;
    sample.pointers[0] = (BSJNIObservedMotionPointer){7, 1, 20.5f, -30.25f};
    sample.pointers[1] = (BSJNIObservedMotionPointer){8, 2, 100.0f, 90.5f};
    sample.history_count = 2;
    sample.historical_event_times_ms[0] = INT64_C(5500000100);
    sample.historical_event_times_ms[1] = INT64_C(5500000110);
    sample.axis_count = 2;
    sample.axes[0] = (BSJNIObservedMotionAxis){0, 0, 4.125f};
    sample.axes[1] = (BSJNIObservedMotionAxis){1, 1, -7.75f};
    sample.historical_axis_count = 2;
    sample.historical_axes[0] =
        (BSJNIObservedHistoricalMotionAxis){0, 0, 0, 1.5f};
    sample.historical_axes[1] =
        (BSJNIObservedHistoricalMotionAxis){1, 1, 1, -2.75f};

    /* The snapshot is copied, not borrowed from the producer's stack. */
    assert(bs_jni_motion_event_from_observation(NULL) == NULL);
    jobject motion = bs_jni_motion_event_from_observation(&sample);
    assert(motion);
    assert((*env)->GetObjectClass(env, motion) == motion_class);
    sample.device_id = 99;
    check_int(env, motion, motion_class, "getDeviceId", 41);

    const char *int_names[] = {"getSource", "getAction", "getFlags",
        "getMetaState", "getActionButton", "getButtonState",
        "getClassification", "getEdgeFlags", "getHistorySize",
        "getPointerCount"};
    const int32_t int_values[] = {0x1002, 261, 2, 16, 3, 5, 1, 9, 2, 2};
    for (size_t i = 0; i < sizeof int_values / sizeof int_values[0]; ++i)
        check_int(env, motion, motion_class, int_names[i], int_values[i]);
    check_long(env, motion, motion_class, "getEventTime",
               INT64_C(5500000120));
    check_long(env, motion, motion_class, "getDownTime",
               INT64_C(5500000000));

    jmethodID pointer_id = member(env, motion_class, "getPointerId", "(I)I");
    jmethodID tool_type = member(env, motion_class, "getToolType", "(I)I");
    jmethodID historical_time = member(env, motion_class,
                                        "getHistoricalEventTime", "(I)J");
    jmethodID raw_x = member(env, motion_class, "getRawX", "(I)F");
    jmethodID raw_y = member(env, motion_class, "getRawY", "(I)F");
    jmethodID axis = member(env, motion_class, "getAxisValue", "(II)F");
    jmethodID historical_axis = member(env, motion_class,
                                       "getHistoricalAxisValue", "(III)F");
    assert((*env)->CallIntMethod(env, motion, pointer_id, 1) == 8);
    assert(call_int_v(env, motion, tool_type, 1) == 2);
    jvalue one[] = {{.i = 0}};
    assert((*env)->CallIntMethodA(env, motion, pointer_id, one) == 7);
    assert((*env)->CallLongMethod(env, motion, historical_time, 0) ==
           INT64_C(5500000100));
    assert(call_long_v(env, motion, historical_time, 1) ==
           INT64_C(5500000110));
    assert((*env)->CallLongMethodA(env, motion, historical_time, one) ==
           INT64_C(5500000100));
    assert((*env)->CallFloatMethod(env, motion, raw_x, 0) == 20.5f);
    assert(call_float_v(env, motion, raw_y, 1) == 90.5f);
    assert((*env)->CallFloatMethodA(env, motion, raw_y, one) == -30.25f);
    assert((*env)->CallFloatMethod(env, motion,
        member(env, motion_class, "getXPrecision", "()F")) == 0.5f);
    assert((*env)->CallFloatMethod(env, motion,
        member(env, motion_class, "getYPrecision", "()F")) == 1.25f);
    assert((*env)->CallFloatMethod(env, motion, axis, 0, 0) == 4.125f);
    assert(call_float_v(env, motion, axis, 1, 1) == -7.75f);
    jvalue axis_args[] = {{.i = 0}, {.i = 1}};
    assert((*env)->CallFloatMethodA(env, motion, axis, axis_args) == 0.0f);
    assert(!(*env)->ExceptionCheck(env));
    assert((*env)->CallFloatMethod(env, motion, historical_axis, 0, 0, 0) ==
           1.5f);
    assert(call_float_v(env, motion, historical_axis, 1, 1, 1) == -2.75f);
    jvalue historical_args[] = {{.i = 1}, {.i = 1}, {.i = 0}};
    assert((*env)->CallFloatMethodA(env, motion, historical_axis,
                                   historical_args) == 0.0f);
    assert(!(*env)->ExceptionCheck(env));

    /* Invalid pointer/history indices raise a typed JNI exception; they
     * must never read outside an observed input event. */
    assert((*env)->CallIntMethod(env, motion, pointer_id, 2) == 0);
    expect_index_exception(env);
    assert((*env)->CallLongMethod(env, motion, historical_time, -1) == 0);
    expect_index_exception(env);
    assert((*env)->CallFloatMethod(env, motion, historical_axis, 1, 1, 3) == 0);
    expect_index_exception(env);

    /* Reject failed refresh atomically; last good observations survive. */
    BSJNIObservedMotionEvent invalid = sample;
    invalid.pointer_count = 2;
    invalid.pointers[1].pointer_id = invalid.pointers[0].pointer_id;
    assert(bs_jni_motion_event_update(motion, &invalid) == JNI_ERR);
    check_int(env, motion, motion_class, "getDeviceId", 41);
    invalid = sample;
    invalid.historical_axes[0].history_index = 20;
    assert(bs_jni_motion_event_from_observation(&invalid) == NULL);
    invalid = sample;
    invalid.pointers[0].raw_x = __builtin_nanf("");
    assert(bs_jni_motion_event_from_observation(&invalid) == NULL);
    sample.device_id = 99;
    assert(bs_jni_motion_event_update(motion, &sample) == JNI_OK);
    check_int(env, motion, motion_class, "getDeviceId", 99);

    BSJNIObservedKeyEvent keyboard = {0};
    keyboard.device_id = 84;
    keyboard.source = 0x101;
    keyboard.action = 1;
    keyboard.flags = 4;
    keyboard.meta_state = 8;
    keyboard.modifiers = 3;
    keyboard.repeat_count = 2;
    keyboard.key_code = 29;
    keyboard.scan_code = 30;
    keyboard.unicode_char = 97;
    keyboard.down_time_ms = INT64_C(6100000000);
    keyboard.event_time_ms = INT64_C(6100000321);
    jobject key = bs_jni_key_event_from_observation(&keyboard);
    assert(key);
    assert((*env)->GetObjectClass(env, key) == key_class);
    const char *key_names[] = {"getDeviceId", "getSource", "getAction",
        "getFlags", "getMetaState", "getModifiers", "getRepeatCount",
        "getKeyCode", "getScanCode", "getUnicodeChar"};
    const int32_t key_values[] = {84, 0x101, 1, 4, 8, 3, 2, 29, 30, 97};
    for (size_t i = 0; i < sizeof key_values / sizeof key_values[0]; ++i)
        check_int(env, key, key_class, key_names[i], key_values[i]);
    check_long(env, key, key_class, "getEventTime", INT64_C(6100000321));
    check_long(env, key, key_class, "getDownTime", INT64_C(6100000000));

    keyboard.event_time_ms = keyboard.down_time_ms - 1;
    assert(bs_jni_key_event_update(key, &keyboard) == JNI_ERR);
    check_int(env, key, key_class, "getRepeatCount", 2);
    keyboard.event_time_ms = keyboard.down_time_ms + 50;
    keyboard.repeat_count = 3;
    assert(bs_jni_key_event_update(key, &keyboard) == JNI_OK);
    check_int(env, key, key_class, "getRepeatCount", 3);

    /* Wrong event class/method pairing raises unsupported operation, while
     * both original classes remain ordinary JNI-visible object identities. */
    assert((*env)->CallIntMethod(env, key, pointer_id, 0) == 0);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    assert((*env)->IsInstanceOf(env, key, key_class));
    assert(!(*env)->IsInstanceOf(env, key, motion_class));

    BSJNIGameActivityEvidence evidence = {0};
    assert(bs_jni_gameactivity_readiness(&evidence) == JNI_OK);
    assert(evidence.observed_motion_event_count == 1);
    assert(evidence.observed_key_event_count == 1);
    assert(evidence.most_complete_host_service_mask == 0);
    assert(!evidence.actual_java_activity_ready);
    assert(evidence.blocker_flags & BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED);
    assert(!bs_jni_gameactivity_java_ready());
    puts("PASS: original MotionEvent/KeyEvent JNI I/J/F methods, V/A ABI,"
         " timestamps, axes, history, validation, reuse, exception/readiness");
    return 0;
}
