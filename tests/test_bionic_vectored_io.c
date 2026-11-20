#include "../source/bionic_vectored_io.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static char storage[16];
static size_t position;
static size_t capacity;
static int failures;

ssize_t bs_pipe_write(int fd, const void *data, size_t len) {
    (void)fd;
    if (failures) { errno = EIO; return -1; }
    if (len > capacity - position) len = capacity - position;
    memcpy(storage + position, data, len);
    position += len;
    return (ssize_t)len;
}
ssize_t bs_pipe_read(int fd, void *data, size_t len) {
    (void)fd;
    if (failures) { errno = EIO; return -1; }
    if (len > capacity - position) len = capacity - position;
    memcpy(data, storage + position, len);
    position += len;
    return (ssize_t)len;
}

int main(void) {
    char first[] = "AB", second[] = "CDE";
    const BsBionicIovec input[] = {{first, 2}, {second, 3}};
    capacity = sizeof storage;
    assert(bs_bionic_writev(7, input, 2) == 5);
    assert(position == 5 && memcmp(storage, "ABCDE", 5) == 0);

    char a[2] = {0}, b[3] = {0};
    BsBionicIovec output[] = {{a, 2}, {b, 3}};
    position = 0;
    capacity = 4;
    assert(bs_bionic_readv(7, output, 2) == 4);
    assert(memcmp(a, "AB", 2) == 0 && memcmp(b, "CD", 2) == 0);
    assert(bs_bionic_readv(7, output, -1) == -1 && errno == EINVAL);
    assert(bs_bionic_writev(7, NULL, 1) == -1 && errno == EINVAL);

    failures = 1;
    assert(bs_bionic_writev(7, input, 2) == -1 && errno == EIO);
    assert(bs_bionic_writev(7, input, 0) == 0);
    return 0;
}
