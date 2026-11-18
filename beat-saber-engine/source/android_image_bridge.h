/* Owned CPU-backed Android NDK AImage objects. MIT license.
 * Only images created from actual pixels are accepted by AImage_getWidth.
 */
#ifndef BS_ANDROID_IMAGE_BRIDGE_H
#define BS_ANDROID_IMAGE_BRIDGE_H
#include <stddef.h>
#include <stdint.h>

typedef struct AImage AImage;

/* This internal constructor copies a real RGBA8888 frame supplied by a
 * decoder/reader. It never manufactures a surface or hardware buffer. */
AImage *bs_android_image_from_rgba(const void *pixels, int32_t width,
                                   int32_t height, size_t row_stride,
                                   int64_t timestamp_ns);

/* MediaNDK status codes: 0 OK, -10003 invalid object, -10004 invalid input. */
int32_t AImage_getWidth(const AImage *image, int32_t *width);
int32_t AImage_getHeight(const AImage *image, int32_t *height);
int32_t AImage_getTimestamp(const AImage *image, int64_t *timestamp_ns);
int32_t AImage_getPlaneData(const AImage *image, int plane_index,
                            uint8_t **data, int *data_length);
int32_t AImage_getPlaneRowStride(const AImage *image, int plane_index,
                                 int32_t *row_stride);
void AImage_delete(AImage *image);

#endif
