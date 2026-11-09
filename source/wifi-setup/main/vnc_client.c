#include "vnc_client.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"
#include "mbedtls/des.h"

#define MAX_DESKTOP 8192u

static bool read_exact(int fd, void *buffer, size_t size)
{
    uint8_t *p = buffer;
    while (size) {
        const int count = recv(fd, p, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        p += count;
        size -= count;
    }
    return true;
}

static bool write_exact(int fd, const void *buffer, size_t size)
{
    const uint8_t *p = buffer;
    while (size) {
        const int count = send(fd, p, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        p += count;
        size -= count;
    }
    return true;
}

static unsigned u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool discard(int fd, uint32_t count)
{
    uint8_t bytes[128];
    if (count > 65536) return false;
    while (count) {
        const unsigned take = count < sizeof(bytes) ? count : sizeof(bytes);
        if (!read_exact(fd, bytes, take)) return false;
        count -= take;
    }
    return true;
}

static uint8_t reverse_bits(uint8_t byte)
{
    byte = ((byte & 0x55) << 1) | ((byte & 0xaa) >> 1);
    byte = ((byte & 0x33) << 2) | ((byte & 0xcc) >> 2);
    return (byte << 4) | (byte >> 4);
}

static bool authenticate(int fd, const char *password)
{
    uint8_t challenge[16], reply[16], key[8] = {0}, status[4];
    if (!read_exact(fd, challenge, sizeof(challenge))) return false;
    for (unsigned i = 0; i < sizeof(key) && password[i]; ++i) key[i] = reverse_bits(password[i]);
    mbedtls_des_context des;
    mbedtls_des_init(&des);
    const bool ok = mbedtls_des_setkey_enc(&des, key) == 0 &&
        mbedtls_des_crypt_ecb(&des, challenge, reply) == 0 &&
        mbedtls_des_crypt_ecb(&des, challenge + 8, reply + 8) == 0;
    mbedtls_des_free(&des);
    return ok && write_exact(fd, reply, sizeof(reply)) && read_exact(fd, status, sizeof(status)) && u32(status) == 0;
}

bool vnc_handshake(vnc_client_t *vnc, const char *password)
{
    const int fd = vnc->fd;
    char server[13] = {0};
    unsigned major, minor;
    if (!read_exact(fd, server, 12) || server[11] != '\n' ||
        sscanf(server, "RFB %3u.%3u", &major, &minor) != 2 || major != 3 || minor < 3) return false;
    const char *version = minor >= 8 ? "RFB 003.008\n" : minor >= 7 ? "RFB 003.007\n" : "RFB 003.003\n";
    if (!write_exact(fd, version, 12)) return false;
    const uint8_t auth = 2;
    if (minor < 7) {
        uint8_t selected[4];
        if (!read_exact(fd, selected, sizeof(selected)) || u32(selected) != auth) return false;
    } else {
        uint8_t count, types[255];
        if (!read_exact(fd, &count, 1) || !count || !read_exact(fd, types, count) ||
            !memchr(types, auth, count) || !write_exact(fd, &auth, 1)) return false;
    }
    const uint8_t shared = 1;
    uint8_t init[24];
    if (!authenticate(fd, password) || !write_exact(fd, &shared, 1) || !read_exact(fd, init, sizeof(init))) return false;
    vnc->width = u16(init);
    vnc->height = u16(init + 2);
    if (!vnc->width || !vnc->height || vnc->width > MAX_DESKTOP || vnc->height > MAX_DESKTOP ||
        !discard(fd, u32(init + 20))) return false;
    static const uint8_t format[20] = {0,0,0,0, 16,16,0,1, 0,31,0,63,0,31, 11,5,0, 0,0,0};
    static const uint8_t encodings[] = {2,0,0,2, 0,0,0,5, 0,0,0,0};
    return write_exact(fd, format, sizeof(format)) && write_exact(fd, encodings, sizeof(encodings));
}

bool vnc_request(const vnc_client_t *vnc, bool incremental)
{
    const uint8_t msg[] = {3, incremental, 0,0,0,0,
        vnc->width >> 8, vnc->width, vnc->height >> 8, vnc->height};
    return write_exact(vnc->fd, msg, sizeof(msg));
}

bool vnc_pointer(const vnc_client_t *vnc, uint16_t x, uint16_t y, uint8_t mask)
{
    const uint8_t msg[] = {5, mask, x >> 8, x, y >> 8, y};
    return write_exact(vnc->fd, msg, sizeof(msg));
}

bool vnc_key(const vnc_client_t *vnc, uint32_t keysym, bool down)
{
    const uint8_t msg[] = {4, down, 0, 0, keysym >> 24, keysym >> 16, keysym >> 8, keysym};
    return write_exact(vnc->fd, msg, sizeof(msg));
}

static bool hextile(int fd, unsigned x, unsigned y, unsigned w, unsigned h,
                    vnc_row_callback_t row, vnc_band_callback_t band, void *ctx)
{
    uint8_t tile[16 * 16 * 2], bg[2] = {0}, fg[2] = {0};
    for (unsigned ty = 0; ty < h; ty += 16) {
        const unsigned th = h - ty < 16 ? h - ty : 16;
        for (unsigned tx = 0; tx < w; tx += 16) {
            const unsigned tw = w - tx < 16 ? w - tx : 16;
            uint8_t flags;
            if (!read_exact(fd, &flags, 1) || (flags & 0xe0)) return false;
            if (flags & 1) {
                if (!read_exact(fd, tile, tw * th * 2)) return false;
            } else {
                if ((flags & 2) && !read_exact(fd, bg, 2)) return false;
                if ((flags & 4) && !read_exact(fd, fg, 2)) return false;
                for (unsigned pixel = 0; pixel < tw * th; ++pixel) memcpy(tile + pixel * 2, bg, 2);
                if (flags & 8) {
                    uint8_t count;
                    if (!read_exact(fd, &count, 1)) return false;
                    for (unsigned i = 0; i < count; ++i) {
                        uint8_t color[2], subrect[2];
                        if (flags & 16) {
                            if (!read_exact(fd, color, 2)) return false;
                        } else {
                            memcpy(color, fg, 2);
                        }
                        if (!read_exact(fd, subrect, 2)) return false;
                        const unsigned sx = subrect[0] >> 4, sy = subrect[0] & 15;
                        const unsigned sw = (subrect[1] >> 4) + 1, sh = (subrect[1] & 15) + 1;
                        if (sx + sw > tw || sy + sh > th) return false;
                        for (unsigned dy = sy; dy < sy + sh; ++dy) {
                            for (unsigned dx = sx; dx < sx + sw; ++dx) memcpy(tile + (dy * tw + dx) * 2, color, 2);
                        }
                    }
                }
            }
            for (unsigned dy = 0; dy < th; ++dy) {
                if (!row(ctx, x + tx, y + ty + dy, tw, tile + dy * tw * 2)) return false;
            }
        }
        band(ctx, x, y + ty, w, th);
    }
    return true;
}

// 1 means an update was consumed, 0 means another server event, -1 means disconnect.
int vnc_receive(const vnc_client_t *vnc, vnc_row_callback_t row, vnc_band_callback_t band, void *ctx, uint8_t *scratch, uint32_t *encoding)
{
    const int fd = vnc->fd;
    uint8_t type, header[12];
    if (!read_exact(fd, &type, 1)) return -1;
    switch (type) {
        case 0: break;
        case 1:
            return read_exact(fd, header, 5) && discard(fd, u16(header + 3) * 6u) ? 0 : -1;
        case 2: return 0;
        case 3:
            return read_exact(fd, header, 7) && discard(fd, u32(header + 3)) ? 0 : -1;
        default: return -1;
    }
    if (!read_exact(fd, header, 3)) return -1;
    const unsigned count = u16(header + 1);
    for (unsigned i = 0; i < count; ++i) {
        if (!read_exact(fd, header, sizeof(header))) return -1;
        const unsigned x = u16(header), y = u16(header + 2), w = u16(header + 4), h = u16(header + 6);
        const uint32_t kind = u32(header + 8);
        if (!w || !h || x + w > vnc->width || y + h > vnc->height) return -1;
        if (kind == 5) {
            if (!hextile(fd, x, y, w, h, row, band, ctx)) return -1;
        } else if (kind == 0) {
            for (unsigned line = y; line < y + h; ++line) {
                if (!read_exact(fd, scratch, w * 2) || !row(ctx, x, line, w, scratch)) return -1;
                band(ctx, x, line, w, 1);
            }
        } else {
            return -1;
        }
        if (kind == 5) *encoding = kind;
    }
    return 1;
}
