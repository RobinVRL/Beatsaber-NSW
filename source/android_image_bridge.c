/* NDK AImage CPU image objects with lifetime and pointer validation.
 * Unlike the original Quest hardware ImageReader, this never exposes a
 * fabricated Android hardware buffer, GPU surface or synthetic frame.
 * SPDX-License-Identifier: MIT
 */
#include "android_image_bridge.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
static Mutex g_image_lock;
static void image_lock(void) { mutexLock(&g_image_lock); }
static void image_unlock(void) { mutexUnlock(&g_image_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_image_lock = PTHREAD_MUTEX_INITIALIZER;
static void image_lock(void) { pthread_mutex_lock(&g_image_lock); }
static void image_unlock(void) { pthread_mutex_unlock(&g_image_lock); }
#endif

enum { IMAGE_OK = 0, IMAGE_INVALID_OBJECT = -10003,
       IMAGE_INVALID_PARAMETER = -10004 };

struct AImage {
    struct AImage *next;
    uint8_t *pixels;
    size_t pixel_bytes;
    size_t row_stride;
    int32_t width, height;
    int64_t timestamp_ns;
};

static AImage *g_images;

static const AImage *lookup_locked(const AImage *address) {
    for (const AImage *image = g_images; image; image = image->next)
        if (image == address) return image;
    return NULL;
}

AImage *bs_android_image_from_rgba(const void *pixels, int32_t width,
                                   int32_t height, size_t row_stride,
                                   int64_t timestamp_ns) {
    if (!pixels || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4 ||
        row_stride < (size_t)width * 4 ||
        (size_t)height > SIZE_MAX / row_stride ||
        row_stride * (size_t)height > INT_MAX) {
        errno = EINVAL;
        return NULL;
    }
    size_t bytes = row_stride * (size_t)height;
    AImage *image = calloc(1, sizeof *image);
    if (!image) return NULL;
    image->pixels = malloc(bytes);
    if (!image->pixels) { free(image); return NULL; }
    memcpy(image->pixels, pixels, bytes);
    image->pixel_bytes = bytes;
    image->row_stride = row_stride;
    image->width = width;
    image->height = height;
    image->timestamp_ns = timestamp_ns;
    image_lock();
    image->next = g_images;
    g_images = image;
    image_unlock();
    return image;
}

int32_t AImage_getWidth(const AImage *image, int32_t *width) {
    if (!width) return IMAGE_INVALID_PARAMETER;
    image_lock();
    const AImage *found = lookup_locked(image);
    if (found) *width = found->width;
    image_unlock();
    return found ? IMAGE_OK : IMAGE_INVALID_OBJECT;
}

int32_t AImage_getHeight(const AImage *image, int32_t *height) {
    if (!height) return IMAGE_INVALID_PARAMETER;
    image_lock();
    const AImage *found = lookup_locked(image);
    if (found) *height = found->height;
    image_unlock();
    return found ? IMAGE_OK : IMAGE_INVALID_OBJECT;
}

int32_t AImage_getTimestamp(const AImage *image, int64_t *timestamp_ns) {
    if (!timestamp_ns) return IMAGE_INVALID_PARAMETER;
    image_lock();
    const AImage *found = lookup_locked(image);
    if (found) *timestamp_ns = found->timestamp_ns;
    image_unlock();
    return found ? IMAGE_OK : IMAGE_INVALID_OBJECT;
}

int32_t AImage_getPlaneData(const AImage *image, int plane_index,
                            uint8_t **data, int *data_length) {
    if (!data || !data_length) return IMAGE_INVALID_PARAMETER;
    image_lock();
    const AImage *found = lookup_locked(image);
    if (found && plane_index == 0) {
        *data = found->pixels;
        *data_length = (int)found->pixel_bytes;
    }
    image_unlock();
    if (!found) return IMAGE_INVALID_OBJECT;
    return plane_index == 0 ? IMAGE_OK : IMAGE_INVALID_PARAMETER;
}

int32_t AImage_getPlaneRowStride(const AImage *image, int plane_index,
                                 int32_t *row_stride) {
    if (!row_stride) return IMAGE_INVALID_PARAMETER;
    image_lock();
    const AImage *found = lookup_locked(image);
    if (found && plane_index == 0) *row_stride = (int32_t)found->row_stride;
    image_unlock();
    if (!found) return IMAGE_INVALID_OBJECT;
    return plane_index == 0 ? IMAGE_OK : IMAGE_INVALID_PARAMETER;
}

void AImage_delete(AImage *image) {
    image_lock();
    AImage **link = &g_images;
    while (*link && *link != image) link = &(*link)->next;
    if (!*link) { image_unlock(); return; }
    *link = image->next;
    image_unlock();
    free(image->pixels);
    free(image);
}
