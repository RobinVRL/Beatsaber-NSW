/* Android NDK sensor ABI for Unity's original libunity.so imports.
 * Hardware input is the attached Nintendo controller's six-axis sensor.
 * Controller orientation does not provide head pose or 6DoF tracking.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_ANDROID_SENSOR_BRIDGE_H
#define BS_ANDROID_SENSOR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#if defined(_WIN32) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

typedef struct BsAndroidSensorManager BsAndroidSensorManager;
typedef struct BsAndroidSensor BsAndroidSensor;
typedef struct BsAndroidSensorQueue BsAndroidSensorQueue;
typedef const BsAndroidSensor *const *BsAndroidSensorList;
typedef int (*BsAndroidSensorLooperCallback)(int fd, int events, void *data);

enum {
    BS_ANDROID_SENSOR_ACCELEROMETER = 1,
    BS_ANDROID_SENSOR_GYROSCOPE = 4
};

/* Android NDK ASensorEvent, 64-bit ABI, including the full 16-float union.
 * Vector x/y/z are the first three floats; status is data bytes 12..15.
 */
typedef struct BsAndroidSensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    float data[16];
    uint32_t flags;
    int32_t reserved1[3];
} BsAndroidSensorEvent;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(BsAndroidSensorEvent) == 104,
               "Android ASensorEvent must be 104 bytes");
_Static_assert(offsetof(BsAndroidSensorEvent, timestamp) == 16,
               "Android ASensorEvent timestamp ABI differs");
_Static_assert(offsetof(BsAndroidSensorEvent, data) == 24,
               "Android ASensorEvent data ABI differs");
_Static_assert(offsetof(BsAndroidSensorEvent, flags) == 88,
               "Android ASensorEvent flags ABI differs");
#endif

/* Exact AAPCS64 signatures of the 15 ASensor* imports in original Unity 6. */
BsAndroidSensorManager *bs_sensor_manager_get_instance(void);
int bs_sensor_manager_get_sensor_list(BsAndroidSensorManager *manager,
                                       BsAndroidSensorList *list);
const BsAndroidSensor *bs_sensor_manager_get_default_sensor(
    BsAndroidSensorManager *manager, int type);
BsAndroidSensorQueue *bs_sensor_manager_create_event_queue(
    BsAndroidSensorManager *manager, void *looper, int ident,
    BsAndroidSensorLooperCallback callback, void *data);
int bs_sensor_manager_destroy_event_queue(BsAndroidSensorManager *manager,
                                          BsAndroidSensorQueue *queue);
int bs_sensor_event_queue_enable_sensor(BsAndroidSensorQueue *queue,
                                        const BsAndroidSensor *sensor);
int bs_sensor_event_queue_disable_sensor(BsAndroidSensorQueue *queue,
                                         const BsAndroidSensor *sensor);
int bs_sensor_event_queue_set_event_rate(BsAndroidSensorQueue *queue,
                                         const BsAndroidSensor *sensor,
                                         int32_t usec);
int bs_sensor_event_queue_has_events(BsAndroidSensorQueue *queue);
ssize_t bs_sensor_event_queue_get_events(BsAndroidSensorQueue *queue,
                                         BsAndroidSensorEvent *events,
                                         size_t count);
int bs_sensor_get_type(const BsAndroidSensor *sensor);
float bs_sensor_get_resolution(const BsAndroidSensor *sensor);
int bs_sensor_get_min_delay(const BsAndroidSensor *sensor);
const char *bs_sensor_get_name(const BsAndroidSensor *sensor);
const char *bs_sensor_get_vendor(const BsAndroidSensor *sensor);

/* The owner thread must call poll_current from its ALooper loop, with
 * the ALooper mutex released. Calling has_enabled_current in the same
 * unlocked region tells the loop whether to probe again after 5 ms.
 * The hook samples actual HID state and signals registered pipe FDs.
 */
void bs_android_sensor_poll_current(void);
int bs_android_sensor_has_enabled_current(void);
/* Tear down queues after an ALooper's final release, outside looper lock. */
void bs_android_sensor_release_looper(uint64_t owner);

#ifdef BS_SENSOR_HOST_TEST
/* Injectable PHYSICAL sample provider only for deterministic host tests.
 * No test provider exists in the NRO build.
 */
typedef struct BsAndroidSensorPhysicalSample {
    uint64_t sequence;
    int64_t timestamp_ns;
    float acceleration_g[3];
    float angular_velocity_rad_s[3];
    int connected;
} BsAndroidSensorPhysicalSample;
void bs_android_sensor_test_set_backend(
    int (*available)(void *),
    int (*read_sample)(void *, BsAndroidSensorPhysicalSample *),
    void *context);
void bs_android_sensor_test_set_thread_id(uint64_t id);
#endif

#endif
