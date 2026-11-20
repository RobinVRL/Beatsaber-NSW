/* Deterministic ASensorManager tests; no fabricated readings in production. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../source/android_sensor_bridge.h"

static int g_attached;
static BsAndroidSensorPhysicalSample g_sample;
static int g_pipe_created;
static int g_notifications;
static int g_pending_notification;
static int g_wake_calls;
static int g_pipe_closed;
static int g_watch_removed;
static void *const g_looper = (void *)(uintptr_t)0x5555;

static int available(void *context) {
    (void)context;
    return g_attached;
}
static int read_sample(void *context, BsAndroidSensorPhysicalSample *sample) {
    (void)context;
    if (!g_attached) return 0;
    *sample = g_sample;
    return 1;
}
void *bs_looper_for_thread(void) { return g_looper; }
int bs_pipe(int fds[2]) {
    fds[0] = 0x4000 + g_pipe_created * 2;
    fds[1] = fds[0] + 1;
    ++g_pipe_created;
    return 0;
}
int bs_pipe_fcntl(int fd, int command, ...) {
    (void)fd;
    (void)command;
    return 0;
}
int bs_looper_add_fd(void *looper, int fd, int ident,
                     int events, void *callback, void *data) {
    (void)fd;
    (void)ident;
    (void)callback;
    (void)data;
    assert(looper == g_looper && events == 1);
    return 1;
}
int bs_looper_remove_fd(void *looper, int fd) {
    (void)fd;
    assert(looper == g_looper);
    ++g_watch_removed;
    return 1;
}
int bs_pipe_close(int fd) {
    (void)fd;
    ++g_pipe_closed;
    return 0;
}
void bs_looper_wake(void *looper) {
    assert(looper == g_looper);
    ++g_wake_calls;
}
ssize_t bs_pipe_write(int fd, const void *buffer, size_t count) {
    (void)fd;
    (void)buffer;
    assert(count == 1 && !g_pending_notification);
    ++g_notifications;
    g_pending_notification = 1;
    return 1;
}
ssize_t bs_pipe_read(int fd, void *buffer, size_t count) {
    (void)fd;
    if (!g_pending_notification) {
        errno = EAGAIN;
        return -1;
    }
    assert(count == 1);
    *(char *)buffer = 1;
    g_pending_notification = 0;
    return 1;
}

static void test_real_sample_conversion_and_queue(void) {
    BsAndroidSensorManager *manager = bs_sensor_manager_get_instance();
    BsAndroidSensorList list = (BsAndroidSensorList)(uintptr_t)0x42;
    assert(bs_sensor_manager_get_sensor_list(manager, &list) == 0);
    assert(list == NULL);
    assert(bs_sensor_manager_get_default_sensor(
        manager, BS_ANDROID_SENSOR_ACCELEROMETER) == NULL);
    g_attached = 1;
    assert(bs_sensor_manager_get_sensor_list(manager, &list) == 2);
    assert(list && list[0] && list[1]);
    assert(bs_sensor_get_type(list[0]) == BS_ANDROID_SENSOR_ACCELEROMETER);
    assert(bs_sensor_get_type(list[1]) == BS_ANDROID_SENSOR_GYROSCOPE);
    assert(bs_sensor_get_type(NULL) == -EINVAL);
    assert(bs_sensor_get_min_delay(list[0]) == 5000);
    assert(strcmp(bs_sensor_get_vendor(list[0]), "Nintendo") == 0);
    assert(bs_sensor_get_name(list[1]) != NULL);
    assert(bs_sensor_get_resolution(list[1]) == 0.0f);
    assert(bs_sensor_manager_get_default_sensor(manager, 999) == NULL);
    BsAndroidSensorQueue *queue = bs_sensor_manager_create_event_queue(
        manager, g_looper, 7, NULL, NULL);
    assert(queue);
    assert(bs_sensor_event_queue_enable_sensor(queue, list[0]) == 0);
    assert(bs_sensor_event_queue_enable_sensor(queue, list[1]) == 0);
    assert(bs_android_sensor_has_enabled_current());
    assert(bs_sensor_event_queue_set_event_rate(queue, list[1], 20000) == 0);
    assert(bs_sensor_event_queue_set_event_rate(queue, list[1], -1) == -EINVAL);
    g_sample.sequence = 123;
    g_sample.timestamp_ns = 12300000000LL;
    g_sample.connected = 1;
    g_sample.acceleration_g[0] = 1.0f;
    g_sample.acceleration_g[1] = -0.5f;
    g_sample.angular_velocity_rad_s[2] = 2.5f;
    assert(bs_sensor_event_queue_has_events(queue) == 1);
    assert(g_notifications == 1 && g_pending_notification == 1);
    assert(bs_sensor_event_queue_has_events(queue) == 1);
    assert(g_notifications == 1);
    BsAndroidSensorEvent events[4] = {{0}};
    assert(bs_sensor_event_queue_get_events(queue, events, 4) == 2);
    assert(events[0].version == (int)sizeof(events[0]));
    assert(events[0].timestamp == g_sample.timestamp_ns);
    assert(events[0].type == 1 && events[1].type == 4);
    assert(events[0].data[0] > 9.806f && events[0].data[0] < 9.807f);
    assert(events[0].data[1] < -4.903f && events[0].data[1] > -4.904f);
    assert(events[1].data[2] == 2.5f);
    assert(g_pending_notification == 0);
    assert(bs_sensor_event_queue_has_events(queue) == 0);
    g_sample.sequence = 124;
    g_sample.timestamp_ns += 10000000;
    assert(bs_sensor_event_queue_has_events(queue) == 1);
    assert(bs_sensor_event_queue_get_events(queue, events, 4) == 1);
    assert(events[0].type == 1); /* gyro rate constrained to >=20ms */
    g_sample.sequence = 125;
    g_sample.timestamp_ns += 10000000;
    assert(bs_sensor_event_queue_get_events(queue, events, 4) == 2);
    assert(events[1].type == 4);
    g_sample.sequence = 126;
    g_sample.timestamp_ns += 10000000;
    assert(bs_sensor_event_queue_has_events(queue) == 1);
    assert(bs_sensor_event_queue_disable_sensor(queue, list[0]) == 0);
    assert(bs_sensor_event_queue_has_events(queue) == 0);
    assert(g_pending_notification == 0);
    assert(bs_sensor_event_queue_disable_sensor(queue, list[1]) == 0);
    assert(bs_android_sensor_has_enabled_current() == 0);
    assert(bs_sensor_manager_destroy_event_queue(manager, queue) == 0);
    assert(bs_sensor_manager_destroy_event_queue(manager, queue) == -EINVAL);
}

static void test_detached_hardware(void) {
    BsAndroidSensorManager *manager = bs_sensor_manager_get_instance();
    BsAndroidSensorQueue *queue = bs_sensor_manager_create_event_queue(
        manager, g_looper, 7, NULL, NULL);
    assert(queue);
    g_attached = 0;
    /* Retain a known metadata handle across a physical disconnect. */
    BsAndroidSensorList list;
    assert(bs_sensor_manager_get_sensor_list(manager, &list) == 0);
    assert(bs_sensor_event_queue_enable_sensor(
        queue, bs_sensor_manager_get_default_sensor(manager, 1)) == -EINVAL);
    assert(bs_sensor_event_queue_has_events(queue) == 0);
    assert(bs_sensor_manager_destroy_event_queue(manager, queue) == 0);
}

static void test_cross_thread_enable_and_looper_release(void) {
    BsAndroidSensorManager *manager = bs_sensor_manager_get_instance();
    g_attached = 1;
    BsAndroidSensorList sensors = NULL;
    assert(bs_sensor_manager_get_sensor_list(manager, &sensors) == 2);
    BsAndroidSensorQueue *queue = bs_sensor_manager_create_event_queue(
        manager, g_looper, 1, NULL, NULL);
    assert(queue);
    int initial_wakes = g_wake_calls;
    /* Simulate a second thread enabling a queue whose owner is idle. */
    bs_android_sensor_test_set_thread_id(2);
    assert(bs_sensor_event_queue_enable_sensor(queue, sensors[0]) == 0);
    assert(g_wake_calls == initial_wakes + 1);
    assert(bs_sensor_event_queue_enable_sensor(queue, sensors[0]) == 0);
    assert(g_wake_calls == initial_wakes + 1);
    bs_android_sensor_test_set_thread_id(1);
    assert(bs_android_sensor_has_enabled_current() == 1);
    int initial_closes = g_pipe_closed;
    int initial_removes = g_watch_removed;
    bs_android_sensor_release_looper(1);
    assert(g_pipe_closed == initial_closes + 2);
    assert(g_watch_removed == initial_removes + 1);
    assert(bs_android_sensor_has_enabled_current() == 0);
    assert(bs_sensor_event_queue_has_events(queue) == -EINVAL);
    assert(bs_sensor_manager_destroy_event_queue(manager, queue) == -EINVAL);
    /* Reclaimed queue slots must remain reusable after looper teardown. */
    queue = bs_sensor_manager_create_event_queue(
        manager, g_looper, 1, NULL, NULL);
    assert(queue);
    assert(bs_sensor_manager_destroy_event_queue(manager, queue) == 0);
}

int main(void) {
    bs_android_sensor_test_set_backend(available, read_sample, NULL);
    test_real_sample_conversion_and_queue();
    test_detached_hardware();
    test_cross_thread_enable_and_looper_release();
    puts("Android NDK sensor queue tests passed");
    return 0;
}
