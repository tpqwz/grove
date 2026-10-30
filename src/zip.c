#include "zip.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>

static int span_ok(const unsigned char *p, const unsigned char *end, size_t n) {
    if (!p || !end || p > end) {
        return 0;
    }
    return n <= (size_t)(end - p);
}

void zip_free(Zip *z) {
    if (!z) {
        return;
    }
    for (size_t i = 0; i < z->n; i++) {
        free(z->files[i].name);
        free(z->files[i].data);
    }
    free(z->files);
    z->files = NULL;
    z->n = 0;
}

ZipFile *zip_find(Zip *z, const char *name) {
    for (size_t i = 0; i < z->n; i++) {
        if (strcmp(z->files[i].name, name) == 0) {
            return &z->files[i];
        }
    }
    return NULL;
}

int zip_name_ends(const ZipFile *f, const char *suffix) {
    size_t n = strlen(f->name);
    size_t s = strlen(suffix);
    if (n < s) {
        return 0;
    }
    return memcmp(f->name + n - s, suffix, s) == 0;
}

static int inflate_raw(const unsigned char *src, size_t n, size_t expect, unsigned char **out, size_t *outn, Err *e) {
    size_t cap;
    if (n > (size_t)UINT_MAX) {
        err_set(e, "zip member is too large");
        return -1;
    }
    if (expect) {
        cap = expect;
    } else if (size_mul(n, 4, &cap) != 0 || size_add(cap, 64, &cap) != 0) {
        err_set(e, "zip member is too large");
        return -1;
    }
    if (cap > 64u * 1024u * 1024u) {
        err_set(e, "zip member is too large");
        return -1;
    }
    if (cap == 0) {
        cap = 1;
    }
    unsigned char *dst = malloc(cap);
    if (!dst) {
        err_set(e, "out of memory");
        return -1;
    }
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)n;
    if (inflateInit2(&strm, -15) != Z_OK) {
        free(dst);
        err_set(e, "inflate init failed");
        return -1;
    }
    int rc;
    for (;;) {
        if (strm.total_out >= cap) {
            if (cap > 32u * 1024u * 1024u) {
                inflateEnd(&strm);
                free(dst);
                err_set(e, "zip member is too large");
                return -1;
            }
            size_t ncap;
            if (size_mul(cap, 2, &ncap) != 0) {
                inflateEnd(&strm);
                free(dst);
                err_set(e, "zip member is too large");
                return -1;
            }
            unsigned char *nd = realloc(dst, ncap);
            if (!nd) {
                inflateEnd(&strm);
                free(dst);
                err_set(e, "out of memory");
                return -1;
            }
            dst = nd;
            cap = ncap;
        }
        strm.next_out = dst + strm.total_out;
        strm.avail_out = (uInt)(cap - strm.total_out);
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            inflateEnd(&strm);
            free(dst);
            err_set(e, "deflate data in zip is corrupt");
            return -1;
        }
    }
    inflateEnd(&strm);
    *out = dst;
    *outn = strm.total_out;
    return 0;
}

int zip_read(const unsigned char *bytes, size_t len, Zip *out, Err *e) {
    memset(out, 0, sizeof *out);
    if (len < 22) {
        err_set(e, "not a zip file");
        return -1;
    }
    ssize_t eocd = -1;
    size_t back = len - 22;
    if (back > 65535) {
        back = 65535;
    }
    for (size_t off = 0; off <= back; off++) {
        size_t i = len - 22 - off;
        if (memcmp(bytes + i, "PK\x05\x06", 4) != 0) {
            continue;
        }
        uint16_t comment = rd_u16(bytes + i + 20);
        if (i + 22 + comment == len) {
            eocd = (ssize_t)i;
            break;
        }
    }
    if (eocd < 0) {
        err_set(e, "zip directory not found");
        return -1;
    }
    uint32_t entries = rd_u16(bytes + (size_t)eocd + 10);
    uint64_t cd_size = rd_u32(bytes + (size_t)eocd + 12);
    uint64_t cd_off = rd_u32(bytes + (size_t)eocd + 16);
    if (entries == 0xffff || cd_off == 0xffffffffu || cd_size == 0xffffffffu) {
        if ((size_t)eocd < 20 || memcmp(bytes + (size_t)eocd - 20, "PK\x06\x07", 4) != 0) {
            err_set(e, "zip64 archive is missing its locator");
            return -1;
        }
        uint64_t zoff = rd_u64(bytes + (size_t)eocd - 20 + 8);
        if (zoff > len || len - zoff < 56 || memcmp(bytes + zoff, "PK\x06\x06", 4) != 0) {
            err_set(e, "zip64 directory is truncated");
            return -1;
        }
        uint64_t ent64 = rd_u64(bytes + zoff + 32);
        if (ent64 > 20000) {
            err_set(e, "zip has too many members");
            return -1;
        }
        entries = (uint32_t)ent64;
        cd_size = rd_u64(bytes + zoff + 40);
        cd_off = rd_u64(bytes + zoff + 48);
    }
    if (cd_off > len || cd_size > len - cd_off) {
        err_set(e, "zip directory is out of range");
        return -1;
    }
    if (entries > 20000) {
        err_set(e, "zip has too many members");
        return -1;
    }
    const unsigned char *p = bytes + cd_off;
    const unsigned char *end = p + cd_size;
    size_t total = 0;
    for (uint32_t ei = 0; ei < entries; ei++) {
        if (!span_ok(p, end, 46) || memcmp(p, "PK\x01\x02", 4) != 0) {
            zip_free(out);
            err_set(e, "zip directory entry is corrupt");
            return -1;
        }
        uint16_t flags = rd_u16(p + 8);
        uint16_t method = rd_u16(p + 10);
        uint32_t comp = rd_u32(p + 20);
        uint32_t uncomp = rd_u32(p + 24);
        uint16_t namelen = rd_u16(p + 28);
        uint16_t extralen = rd_u16(p + 30);
        uint16_t commentlen = rd_u16(p + 32);
        uint32_t local = rd_u32(p + 42);
        size_t rec = 46;
        if (size_add(rec, namelen, &rec) != 0 || size_add(rec, extralen, &rec) != 0 ||
            size_add(rec, commentlen, &rec) != 0 || !span_ok(p, end, rec)) {
            zip_free(out);
            err_set(e, "zip directory entry is truncated");
            return -1;
        }
        const unsigned char *extra = p + 46 + namelen;
        if (comp == 0xffffffffu || uncomp == 0xffffffffu || local == 0xffffffffu) {
            const unsigned char *ep = extra;
            const unsigned char *ee = extra + extralen;
            int found = 0;
            while (span_ok(ep, ee, 4)) {
                uint16_t id = rd_u16(ep);
                uint16_t sz = rd_u16(ep + 2);
                ep += 4;
                if (!span_ok(ep, ee, sz)) {
                    break;
                }
                if (id == 1) {
                    const unsigned char *zp = ep;
                    if (uncomp == 0xffffffffu) {
                        if (!span_ok(zp, ep + sz, 8)) {
                            break;
                        }
                        uint64_t v = rd_u64(zp);
                        zp += 8;
                        if (v > 0xffffffffu) {
                            zip_free(out);
                            err_set(e, "a zip member is too large");
                            return -1;
                        }
                        uncomp = (uint32_t)v;
                    }
                    if (comp == 0xffffffffu) {
                        if (!span_ok(zp, ep + sz, 8)) {
                            break;
                        }
                        uint64_t v = rd_u64(zp);
                        zp += 8;
                        if (v > 0xffffffffu) {
                            zip_free(out);
                            err_set(e, "a zip member is too large");
                            return -1;
                        }
                        comp = (uint32_t)v;
                    }
                    if (local == 0xffffffffu) {
                        if (!span_ok(zp, ep + sz, 8)) {
                            break;
                        }
                        uint64_t v = rd_u64(zp);
                        if (v > 0xffffffffu) {
                            zip_free(out);
                            err_set(e, "zip64 local header is too far");
                            return -1;
                        }
                        local = (uint32_t)v;
                    }
                    found = 1;
                    break;
                }
                ep += sz;
            }
            if (!found) {
                zip_free(out);
                err_set(e, "zip64 extra field is missing");
                return -1;
            }
        }
        char *name = xstrndup((const char *)(p + 46), namelen);
        p += rec;
        if (!name) {
            zip_free(out);
            err_set(e, "out of memory");
            return -1;
        }
        if (namelen == 0 || name[namelen - 1] == '/') {
            free(name);
            continue;
        }
        if (flags & 1) {
            free(name);
            zip_free(out);
            err_set(e, "encrypted zip members are not supported");
            return -1;
        }
        size_t data_off;
        if (!size_fits(len, local, 30)) {
            free(name);
            zip_free(out);
            err_set(e, "zip local header is out of range");
            return -1;
        }
        const unsigned char *lh = bytes + local;
        if (memcmp(lh, "PK\x03\x04", 4) != 0) {
            free(name);
            zip_free(out);
            err_set(e, "zip local header is missing");
            return -1;
        }
        uint16_t lname = rd_u16(lh + 26);
        uint16_t lextra = rd_u16(lh + 28);
        if (size_add(local, 30, &data_off) != 0 || size_add(data_off, lname, &data_off) != 0 ||
            size_add(data_off, lextra, &data_off) != 0 || !size_fits(len, data_off, comp)) {
            free(name);
            zip_free(out);
            err_set(e, "zip member data is truncated");
            return -1;
        }
        unsigned char *data = NULL;
        size_t dlen = 0;
        if (method == 0) {
            size_t alloc;
            if (size_add(comp, 1, &alloc) != 0) {
                free(name);
                zip_free(out);
                err_set(e, "a zip member is too large");
                return -1;
            }
            data = malloc(alloc);
            if (!data) {
                free(name);
                zip_free(out);
                err_set(e, "out of memory");
                return -1;
            }
            memcpy(data, bytes + data_off, comp);
            data[comp] = 0;
            dlen = comp;
        } else if (method == 8) {
            if (inflate_raw(bytes + data_off, comp, uncomp, &data, &dlen, e) != 0) {
                free(name);
                zip_free(out);
                return -1;
            }
            size_t alloc;
            if (size_add(dlen, 1, &alloc) != 0) {
                free(name);
                free(data);
                zip_free(out);
                err_set(e, "a zip member is too large");
                return -1;
            }
            unsigned char *nd = realloc(data, alloc);
            if (!nd) {
                free(name);
                free(data);
                zip_free(out);
                err_set(e, "out of memory");
                return -1;
            }
            data = nd;
            data[dlen] = 0;
        } else {
            free(name);
            continue;
        }
        size_t sum;
        if (size_add(total, dlen, &sum) != 0 || sum > 128u * 1024u * 1024u) {
            free(name);
            free(data);
            zip_free(out);
            err_set(e, "uncompressed zip is too large");
            return -1;
        }
        total = sum;
        size_t nn;
        if (size_add(out->n, 1, &nn) != 0 || grow_alloc((void **)&out->files, nn, sizeof(ZipFile)) != 0) {
            free(name);
            free(data);
            zip_free(out);
            err_set(e, "out of memory");
            return -1;
        }
        out->files[out->n].name = name;
        out->files[out->n].data = data;
        out->files[out->n].len = dlen;
        out->n++;
    }
    return 0;
}
