#include "zip.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>

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
    size_t cap = expect ? expect : (n * 4 + 64);
    if (cap > 64u * 1024u * 1024u) {
        err_set(e, "zip member is too large");
        return -1;
    }
    unsigned char *dst = malloc(cap ? cap : 1);
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
            size_t ncap = cap * 2;
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
        if (zoff + 56 > len || memcmp(bytes + zoff, "PK\x06\x06", 4) != 0) {
            err_set(e, "zip64 directory is truncated");
            return -1;
        }
        entries = (uint32_t)rd_u64(bytes + zoff + 32);
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
        if (p + 46 > end || memcmp(p, "PK\x01\x02", 4) != 0) {
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
        if (p + 46 + namelen + extralen + commentlen > end) {
            zip_free(out);
            err_set(e, "zip directory entry is truncated");
            return -1;
        }
        const unsigned char *extra = p + 46 + namelen;
        if (comp == 0xffffffffu || uncomp == 0xffffffffu || local == 0xffffffffu) {
            const unsigned char *ep = extra;
            const unsigned char *ee = extra + extralen;
            int found = 0;
            while (ep + 4 <= ee) {
                uint16_t id = rd_u16(ep);
                uint16_t sz = rd_u16(ep + 2);
                ep += 4;
                if (ep + sz > ee) {
                    break;
                }
                if (id == 1) {
                    const unsigned char *zp = ep;
                    if (uncomp == 0xffffffffu) {
                        if (zp + 8 > ep + sz) {
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
                        if (zp + 8 > ep + sz) {
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
                        if (zp + 8 > ep + sz) {
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
        p += 46 + namelen + extralen + commentlen;
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
        if (local + 30 > len) {
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
        size_t data_off = (size_t)local + 30u + lname + lextra;
        if (data_off + comp > len) {
            free(name);
            zip_free(out);
            err_set(e, "zip member data is truncated");
            return -1;
        }
        unsigned char *data = NULL;
        size_t dlen = 0;
        if (method == 0) {
            data = malloc(comp + 1);
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
            unsigned char *nd = realloc(data, dlen + 1);
            if (nd) {
                data = nd;
            }
            data[dlen] = 0;
        } else {
            free(name);
            continue;
        }
        if (total + dlen > 128u * 1024u * 1024u) {
            free(name);
            free(data);
            zip_free(out);
            err_set(e, "uncompressed zip is too large");
            return -1;
        }
        total += dlen;
        ZipFile *nf = realloc(out->files, (out->n + 1) * sizeof(ZipFile));
        if (!nf) {
            free(name);
            free(data);
            zip_free(out);
            err_set(e, "out of memory");
            return -1;
        }
        out->files = nf;
        out->files[out->n].name = name;
        out->files[out->n].data = data;
        out->files[out->n].len = dlen;
        out->n++;
    }
    return 0;
}
