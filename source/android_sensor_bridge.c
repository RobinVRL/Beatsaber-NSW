/* Nintendo HID six-axis to Android NDK ASensorManager / ASensorEventQueue.
 * Only measured controller acceleration and angular velocity are supplied.
 * SPDX-License-Identifier: MIT
 */
#include "android_sensor_bridge.h"
#include "android_window_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef BS_SENSOR_HOST_TEST
#include <switch.h>
typedef struct BsAndroidSensorPhysicalSample {
    uint64_t sequence;
    int64_t timestamp_ns;
    float acceleration_g[3];
    float angular_velocity_rad_s[3];
    int connected;
} BsAndroidSensorPhysicalSample;
static Mutex g_sensor_mutex;
static void sensor_lock(void) { mutexLock(&g_sensor_mutex); }
static void sensor_unlock(void) { mutexUnlock(&g_sensor_mutex); }
static uint64_t sensor_thread_id(void) {
    uint64_t id = 0;
    if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE))) return 0;
    return id;
}
#else
/* Tests use a deterministic single-threaded looper and HID sample provider. */
static void sensor_lock(void) {}
static void sensor_unlock(void) {}
static uint64_t g_test_thread_id = 1;
static uint64_t sensor_thread_id(void) { return g_test_thread_id; }
void bs_android_sensor_test_set_thread_id(uint64_t id) {
    g_test_thread_id = id;
}
static int (*g_test_available)(void *);
static int (*g_test_read)(void *, BsAndroidSensorPhysicalSample *);
static void *g_test_context;
void bs_android_sensor_test_set_backend(
    int (*available)(void *),
    int (*read_sample)(void *, BsAndroidSensorPhysicalSample *),
    void *context) {
    g_test_available = available;
    g_test_read = read_sample;
    g_test_context = context;
}
#endif

#define BS_SENSOR_QUEUE_COUNT 8
#define BS_SENSOR_RING_COUNT 32
#define BS_SENSOR_TYPES 2
#define BS_SENSOR_MIN_PERIOD_US 5000
#define BS_SENSOR_G_TO_MS2 9.80665f
#define BS_SENSOR_INPUT_EVENT 1
#define BS_SENSOR_SET_NONBLOCK 4
#define BS_SENSOR_NONBLOCK 0x800

struct BsAndroidSensorManager { uint32_t magic; };
struct BsAndroidSensor {
    int type;
    const char *name;
};
struct BsAndroidSensorQueue {
    bool active;
    uint64_t owner;
    void *looper;
    int pipe_read;
    int pipe_write;
    uint8_t enabled[BS_SENSOR_TYPES];
    int32_t period_us[BS_SENSOR_TYPES];
    uint64_t last_sequence[BS_SENSOR_TYPES];
    int64_t last_timestamp[BS_SENSOR_TYPES];
    BsAndroidSensorEvent ring[BS_SENSOR_RING_COUNT];
    unsigned read_offset;
    unsigned used;
};

static struct BsAndroidSensorManager g_manager = {0x53454e53u};
/* The source is the selected physical Nintendo controller, not the console
 * screen or the player's head. Avoid metadata claiming Quest headset IMUs. */
static const struct BsAndroidSensor g_sensors[BS_SENSOR_TYPES] = {
    {BS_ANDROID_SENSOR_ACCELEROMETER, "Nintendo controller accelerometer"},
    {BS_ANDROID_SENSOR_GYROSCOPE, "Nintendo controller gyroscope"}
};
static const BsAndroidSensor *const g_sensor_list[] = {
    &g_sensors[0], &g_sensors[1]
};
static struct BsAndroidSensorQueue g_queues[BS_SENSOR_QUEUE_COUNT];

#ifndef BS_SENSOR_HOST_TEST
static HidSixAxisSensorHandle g_hid_handle;
static HidNpadIdType g_hid_id;
static HidNpadStyleTag g_hid_style;
static bool g_hid_started;

/* Pick an actually attached controller with a six-axis HID handle.
 * Prefer handheld controls, then player 1..8, and skip styles without IMUs.
 * Only claim availability after Horizon accepts hidStartSixAxisSensor. */
static int physical_available(void) {
    if (g_hid_started) {
        if (hidGetNpadStyleSet(g_hid_id) & g_hid_style) return 1;
        hidStopSixAxisSensor(g_hid_handle);
        g_hid_started = false;
    }
    static const HidNpadStyleTag styles[] = {
        HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey,
        HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft,
        HidNpadStyleTag_NpadJoyRight
    };
    for (int id_index = 0; id_index < 9; ++id_index) {
        HidNpadIdType id = id_index == 0 ? HidNpadIdType_Handheld :
            (HidNpadIdType)(id_index - 1);
        uint32_t active_styles = hidGetNpadStyleSet(id);
        for (unsigned style_index = 0;
             style_index < sizeof(styles) / sizeof(styles[0]); ++style_index) {
            HidNpadStyleTag style = styles[style_index];
            if (!(active_styles & style)) continue;
            HidSixAxisSensorHandle handle;
            if (R_FAILED(hidGetSixAxisSensorHandles(&handle, 1, id, style)))
                continue;
            if (R_FAILED(hidStartSixAxisSensor(handle))) continue;
            g_hid_handle = handle;
            g_hid_id = id;
            g_hid_style = style;
            g_hid_started = true;
            return 1;
        }
    }
    return 0;
}

static int64_t sensor_timestamp_ns(void) {
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return -1;
    return (int64_t)stamp.tv_sec * INT64_C(1000000000) + stamp.tv_nsec;
}

static int physical_read(BsAndroidSensorPhysicalSample *sample) {
    if (!g_hid_started || !physical_available()) return 0;
    HidSixAxisSensorState state;
    if (hidGetSixAxisSensorStates(g_hid_handle, &state, 1) != 1 ||
        !(state.attributes & HidSixAxisSensorAttribute_IsConnected))
        return 0;
    int64_t now = sensor_timestamp_ns();
    if (now < 0) return 0;
    sample->sequence = state.sampling_number;
    sample->timestamp_ns = now;
    /* libnx's Nintendo acceleration is measured in G, while Android's
     * ASENSOR_TYPE_ACCELEROMETER reports m/s^2. Angular velocity is rad/s. */
    sample->acceleration_g[0] = state.acceleration.x;
    sample->acceleration_g[1] = state.acceleration.y;
    sample->acceleration_g[2] = state.acceleration.z;
    sample->angular_velocity_rad_s[0] = state.angular_velocity.x;
    sample->angular_velocity_rad_s[1] = state.angular_velocity.y;
    sample->angular_velocity_rad_s[2] = state.angular_velocity.z;
    sample->connected = 1;
    return 1;
}
#else
static int physical_available(void) {
    return g_test_available && g_test_available(g_test_context);
}
static int physical_read(BsAndroidSensorPhysicalSample *sample) {
    return g_test_read && g_test_read(g_test_context, sample);
}
#endif

static int sensor_index(const BsAndroidSensor *sensor) {
    if (sensor == &g_sensors[0]) return 0;
    if (sensor == &g_sensors[1]) return 1;
    return -1;
}

/* The memory of destroyed queue slots is stable, but handles only remain
 * valid until destroy. Validate pointers before dereferencing guest input. */
static BsAndroidSensorQueue *valid_queue(BsAndroidSensorQueue *queue) {
    for (size_t i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i)
        if (queue == &g_queues[i] && g_queues[i].active) return queue;
    return NULL;
}

BsAndroidSensorManager *bs_sensor_manager_get_instance(void) {
    return &g_manager;
}

int bs_sensor_manager_get_sensor_list(BsAndroidSensorManager *manager,
                                      BsAndroidSensorList *list) {
    if (manager != &g_manager || !list) return -EINVAL;
    sensor_lock();
    int available = physical_available();
    *list = available ? g_sensor_list : NULL;
    sensor_unlock();
    return available ? BS_SENSOR_TYPES : 0;
}

const BsAndroidSensor *bs_sensor_manager_get_default_sensor(
    BsAndroidSensorManager *manager, int type) {
    if (manager != &g_manager) return NULL;
    int index = type == BS_ANDROID_SENSOR_ACCELEROMETER ? 0 :
                type == BS_ANDROID_SENSOR_GYROSCOPE ? 1 : -1;
    if (index < 0) return NULL;
    sensor_lock();
    int available = physical_available();
    sensor_unlock();
    return available ? &g_sensors[index] : NULL;
}

BsAndroidSensorQueue *bs_sensor_manager_create_event_queue(
    BsAndroidSensorManager *manager, void *looper, int ident,
    BsAndroidSensorLooperCallback callback, void *data) {
    if (manager != &g_manager || !looper ||
        looper != bs_looper_for_thread() || (!callback && ident < 0))
        return NULL;
    uint64_t owner = sensor_thread_id();
    if (!owner) return NULL;
    sensor_lock();
    BsAndroidSensorQueue *queue = NULL;
    for (unsigned i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i)
        if (!g_queues[i].active) { queue = &g_queues[i]; break; }
    if (!queue) {
        sensor_unlock();
        return NULL;
    }
    int descriptors[2];
    if (bs_pipe(descriptors) < 0) {
        sensor_unlock();
        return NULL;
    }
    if (bs_pipe_fcntl(descriptors[0], BS_SENSOR_SET_NONBLOCK,
                      BS_SENSOR_NONBLOCK) < 0 ||
        bs_looper_add_fd(looper, descriptors[0], ident, BS_SENSOR_INPUT_EVENT,
                         (void *)(uintptr_t)callback, data) != 1) {
        bs_pipe_close(descriptors[0]);
        bs_pipe_close(descriptors[1]);
        sensor_unlock();
        return NULL;
    }
    memset(queue, 0, sizeof(*queue));
    queue->active = true;
    queue->owner = owner;
    queue->looper = looper;
    queue->pipe_read = descriptors[0];
    queue->pipe_write = descriptors[1];
    for (unsigned i = 0; i < BS_SENSOR_TYPES; ++i) {
        queue->period_us[i] = BS_SENSOR_MIN_PERIOD_US;
        queue->last_sequence[i] = UINT64_MAX;
    }
    sensor_unlock();
    return queue;
}

int bs_sensor_manager_destroy_event_queue(BsAndroidSensorManager *manager,
                                           BsAndroidSensorQueue *queue) {
    if (manager != &g_manager) return -EINVAL;
    sensor_lock();
    if (!valid_queue(queue)) {
        sensor_unlock();
        return -EINVAL;
    }
    /* The virtual FDs are unregistered before the storage is reused. */
    bs_looper_remove_fd(queue->looper, queue->pipe_read);
    bs_pipe_close(queue->pipe_read);
    bs_pipe_close(queue->pipe_write);
    memset(queue, 0, sizeof(*queue));
    sensor_unlock();
    return 0;
}

void bs_android_sensor_release_looper(uint64_t owner) {
    if (!owner) return;
    /* The enclosing bs_looper_release MUST already have dropped its mutex.
     * Close the pipe and discard all pending sensor events for that owner.
     * A later recreated thread/looper must never inherit these queues. */
    sensor_lock();
    for (unsigned i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i) {
        BsAndroidSensorQueue *queue = &g_queues[i];
        if (!queue->active || queue->owner != owner) continue;
        bs_looper_remove_fd(queue->looper, queue->pipe_read);
        bs_pipe_close(queue->pipe_read);
        bs_pipe_close(queue->pipe_write);
        memset(queue, 0, sizeof(*queue));
    }
    sensor_unlock();
}

int bs_sensor_event_queue_enable_sensor(BsAndroidSensorQueue *queue,
                                        const BsAndroidSensor *sensor) {
    int index = sensor_index(sensor);
    if (index < 0) return -EINVAL;
    sensor_lock();
    if (!valid_queue(queue)) {
        sensor_unlock();
        return -EINVAL;
    }
    if (!physical_available()) {
        sensor_unlock();
        return -ENODEV;
    }
    /* A foreign thread may enable a queue while its owning looper is
     * sleeping indefinitely with no active sensors. Wake it explicitly so
     * the 4ms sensor probe begins; do so after releasing the sensor mutex. */
    bool wake_owner = !queue->enabled[index] &&
                      queue->owner != sensor_thread_id();
    void *owner_looper = queue->looper;
    queue->enabled[index] = 1;
    queue->last_sequence[index] = UINT64_MAX;
    queue->last_timestamp[index] = 0;
    sensor_unlock();
    if (wake_owner) bs_looper_wake(owner_looper);
    return 0;
}

int bs_sensor_event_queue_disable_sensor(BsAndroidSensorQueue *queue,
                                         const BsAndroidSensor *sensor) {
    int index = sensor_index(sensor);
    if (index < 0) return -EINVAL;
    sensor_lock();
    if (!valid_queue(queue)) {
        sensor_unlock();
        return -EINVAL;
    }
    queue->enabled[index] = 0;
    /* Drop queued events for the now-disabled sensor, retaining the
     * other sensor's order and the existing notification if nonempty. */
    unsigned kept = 0;
    for (unsigned i = 0; i < queue->used; ++i) {
        unsigned from = (queue->read_offset + i) % BS_SENSOR_RING_COUNT;
        if (queue->ring[from].type == sensor->type) continue;
        unsigned to = (queue->read_offset + kept++) % BS_SENSOR_RING_COUNT;
        queue->ring[to] = queue->ring[from];
    }
    queue->used = kept;
    if (kept == 0) {
        char marker;
        (void)bs_pipe_read(queue->pipe_read, &marker, 1);
    }
    sensor_unlock();
    return 0;
}

int bs_sensor_event_queue_set_event_rate(BsAndroidSensorQueue *queue,
                                         const BsAndroidSensor *sensor,
                                         int32_t usec) {
    int index = sensor_index(sensor);
    if (index < 0 || usec <= 0) return -EINVAL;
    sensor_lock();
    if (!valid_queue(queue)) {
        sensor_unlock();
        return -EINVAL;
    }
    queue->period_us[index] =
        usec < BS_SENSOR_MIN_PERIOD_US ? BS_SENSOR_MIN_PERIOD_US : usec;
    sensor_unlock();
    return 0;
}

static void queue_push(BsAndroidSensorQueue *queue,
                       const BsAndroidSensorEvent *event) {
    if (queue->used == BS_SENSOR_RING_COUNT) {
        queue->read_offset = (queue->read_offset + 1) % BS_SENSOR_RING_COUNT;
        --queue->used;
    }
    unsigned write = (queue->read_offset + queue->used) % BS_SENSOR_RING_COUNT;
    queue->ring[write] = *event;
    if (queue->used++ == 0) {
        const char marker = 1;
        (void)bs_pipe_write(queue->pipe_write, &marker, 1);
    }
}

static void queue_sample(BsAndroidSensorQueue *queue,
                         const BsAndroidSensorPhysicalSample *sample) {
    if (!sample->connected || sample->timestamp_ns < 0) return;
    for (int i = 0; i < BS_SENSOR_TYPES; ++i) {
        if (!queue->enabled[i] ||
            queue->last_sequence[i] == sample->sequence) continue;
        int64_t previous = queue->last_timestamp[i];
        int64_t interval = (int64_t)queue->period_us[i] * 1000;
        if (previous > 0 && sample->timestamp_ns >= previous &&
            sample->timestamp_ns - previous < interval)
            continue;
        BsAndroidSensorEvent event = {0};
        event.version = (int32_t)sizeof(event);
        event.sensor = g_sensors[i].type;
        event.type = g_sensors[i].type;
        event.timestamp = sample->timestamp_ns;
        const float *vector = i == 0 ? sample->acceleration_g :
                                         sample->angular_velocity_rad_s;
        float scale = i == 0 ? BS_SENSOR_G_TO_MS2 : 1.0f;
        for (int axis = 0; axis < 3; ++axis)
            event.data[axis] = vector[axis] * scale;
        queue_push(queue, &event);
        queue->last_sequence[i] = sample->sequence;
        queue->last_timestamp[i] = sample->timestamp_ns;
    }
}

void bs_android_sensor_poll_current(void) {
    uint64_t owner = sensor_thread_id();
    if (!owner) return;
    sensor_lock();
    bool active = false;
    for (unsigned i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i) {
        BsAndroidSensorQueue *queue = &g_queues[i];
        if (!queue->active || queue->owner != owner) continue;
        if (queue->enabled[0] || queue->enabled[1]) active = true;
    }
    if (active) {
        BsAndroidSensorPhysicalSample sample = {0};
        if (physical_read(&sample))
            for (unsigned i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i) {
                BsAndroidSensorQueue *queue = &g_queues[i];
                if (queue->active && queue->owner == owner)
                    queue_sample(queue, &sample);
            }
    }
    sensor_unlock();
}

int bs_android_sensor_has_enabled_current(void) {
    uint64_t owner = sensor_thread_id();
    if (!owner) return 0;
    sensor_lock();
    int enabled = 0;
    for (unsigned i = 0; i < BS_SENSOR_QUEUE_COUNT; ++i)
        if (g_queues[i].active && g_queues[i].owner == owner &&
            (g_queues[i].enabled[0] || g_queues[i].enabled[1])) {
            enabled = 1;
            break;
        }
    sensor_unlock();
    return enabled;
}

int bs_sensor_event_queue_has_events(BsAndroidSensorQueue *queue) {
    bs_android_sensor_poll_current();
    sensor_lock();
    int result = valid_queue(queue) ? (queue->used != 0) : -EINVAL;
    sensor_unlock();
    return result;
}

ssize_t bs_sensor_event_queue_get_events(BsAndroidSensorQueue *queue,
                                         BsAndroidSensorEvent *events,
                                         size_t count) {
    if (count && !events) return -EINVAL;
    bs_android_sensor_poll_current();
    sensor_lock();
    if (!valid_queue(queue)) {
        sensor_unlock();
        return -EINVAL;
    }
    size_t delivered = count < queue->used ? count : queue->used;
    for (size_t i = 0; i < delivered; ++i) {
        events[i] = queue->ring[queue->read_offset];
        queue->read_offset = (queue->read_offset + 1) % BS_SENSOR_RING_COUNT;
    }
    queue->used -= (unsigned)delivered;
    if (queue->used == 0) {
        char marker;
        (void)bs_pipe_read(queue->pipe_read, &marker, 1);
    }
    sensor_unlock();
    return (ssize_t)delivered;
}

int bs_sensor_get_type(const BsAndroidSensor *sensor) {
    int index = sensor_index(sensor);
    return index >= 0 ? g_sensors[index].type : -EINVAL;
}
float bs_sensor_get_resolution(const BsAndroidSensor *sensor) {
    /* Horizon supplies floats but no documented physical quantization. */
    (void)sensor;
    return 0.0f;
}
int bs_sensor_get_min_delay(const BsAndroidSensor *sensor) {
    return sensor_index(sensor) >= 0 ? BS_SENSOR_MIN_PERIOD_US : -EINVAL;
}
const char *bs_sensor_get_name(const BsAndroidSensor *sensor) {
    int index = sensor_index(sensor);
    return index >= 0 ? g_sensors[index].name : NULL;
}
const char *bs_sensor_get_vendor(const BsAndroidSensor *sensor) {
    return sensor_index(sensor) >= 0 ? "Nintendo" : NULL;
}
