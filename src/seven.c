#include "seven.h"

#include "Lzma2Dec.h"
#include "LzmaDec.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* linux/limits.h, pulled in by zlib, defines MAX_INPUT as 255. */
#undef MAX_INPUT

enum {
    SEVEN_MAX = 32u * 1024u * 1024u,
    SEVEN_DIC = 64u * 1024u * 1024u,
    K_END = 0x00,
    K_HEADER = 0x01,
    K_ARCHIVE_PROPS = 0x02,
    K_MAIN_STREAMS = 0x04,
    K_FILES = 0x05,
    K_PACK = 0x06,
    K_UNPACK = 0x07,
    K_SUBSTREAMS = 0x08,
    K_SIZE = 0x09,
    K_CRC = 0x0A,
    K_FOLDER = 0x0B,
    K_CODER_UNPACK = 0x0C,
    K_NUM_UNPACK = 0x0D,
    K_EMPTY_STREAM = 0x0E,
    K_EMPTY_FILE = 0x0F,
    K_NAME = 0x11,
    K_ENCODED = 0x17
};

enum { CODEC_COPY = 1, CODEC_LZMA = 2, CODEC_LZMA2 = 3 };

typedef struct {
    const unsigned char *p;
    size_t n;
    size_t i;
} Rd;

typedef struct {
    int codec;
    unsigned char props[8];
    unsigned props_n;
    uint32_t pack_index;
    uint64_t unpack;
    uint32_t nsub;
    uint64_t *subs;
} Folder;

typedef struct {
    char *name;
    int empty;
    int is_dir;
    uint32_t folder;
    uint64_t offset;
    uint64_t size;
} FileEnt;

static void *sz_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size ? size : 1);
}

static void sz_free(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}

static int rd_byte(Rd *r, unsigned *b) {
    if (r->i >= r->n) {
        return -1;
    }
    *b = r->p[r->i++];
    return 0;
}

static int rd_num(Rd *r, uint64_t *out) {
    unsigned first;
    unsigned mask = 0x80;
    uint64_t value = 0;
    int i;
    if (rd_byte(r, &first) != 0) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if ((first & mask) == 0) {
            uint64_t high = (unsigned)first & (mask - 1u);
            if (i > 0 && high > (UINT64_MAX >> (8 * i))) {
                return -1;
            }
            *out = value + (high << (8 * i));
            return 0;
        }
        if (r->i >= r->n) {
            return -1;
        }
        value |= (uint64_t)r->p[r->i++] << (8 * i);
        mask >>= 1;
    }
    *out = value;
    return 0;
}

static int rd_u32le(Rd *r, uint32_t *out) {
    if (r->i + 4 > r->n) {
        return -1;
    }
    *out = rd_u32(r->p + r->i);
    r->i += 4;
    return 0;
}

static int rd_skip(Rd *r, uint64_t n) {
    if (n > r->n - r->i) {
        return -1;
    }
    r->i += (size_t)n;
    return 0;
}

static int rd_bits(Rd *r, unsigned char *bits, size_t nbits) {
    unsigned mask = 0;
    unsigned b = 0;
    size_t i;
    for (i = 0; i < nbits; i++) {
        if (mask == 0) {
            if (rd_byte(r, &b) != 0) {
                return -1;
            }
            mask = 0x80;
        }
        bits[i] = (b & mask) ? 1 : 0;
        mask >>= 1;
    }
    return 0;
}

static int utf8_put(char *s, size_t cap, size_t *o, uint32_t cp) {
    unsigned char u[4];
    size_t n = 0;
    size_t i;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return -1;
    }
    if (cp < 0x80) {
        u[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (unsigned char)(0xC0 | (cp >> 6));
        u[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (unsigned char)(0xE0 | (cp >> 12));
        u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (unsigned char)(0xF0 | (cp >> 18));
        u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*o + n >= cap) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        s[(*o)++] = (char)u[i];
    }
    return 0;
}

static char *utf16_name(const unsigned char *p, size_t n) {
    size_t cap;
    size_t o = 0;
    size_t i = 0;
    char *s;
    if ((n & 1) || size_mul(n / 2, 3, &cap) != 0 || size_add(cap, 1, &cap) != 0) {
        return NULL;
    }
    s = malloc(cap ? cap : 1);
    if (!s) {
        return NULL;
    }
    while (i + 1 < n) {
        uint32_t cp = rd_u16(p + i);
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            uint32_t lo = rd_u16(p + i);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2;
            }
        }
        if (utf8_put(s, cap, &o, cp) != 0) {
            free(s);
            return NULL;
        }
    }
    s[o] = 0;
    return s;
}

static int crc_digests(Rd *r, size_t nitems) {
    unsigned all = 0;
    size_t i;
    size_t defined = nitems;
    if (rd_byte(r, &all) != 0) {
        return -1;
    }
    if (!all) {
        unsigned char *bits = calloc(nitems ? nitems : 1, 1);
        if (!bits || rd_bits(r, bits, nitems) != 0) {
            free(bits);
            return -1;
        }
        defined = 0;
        for (i = 0; i < nitems; i++) {
            defined += bits[i];
        }
        free(bits);
    }
    for (i = 0; i < defined; i++) {
        uint32_t crc;
        if (rd_u32le(r, &crc) != 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_folder(Rd *r, Folder *folder) {
    uint64_t ncoders = 0;
    uint64_t num_in = 0;
    uint64_t num_out = 0;
    uint64_t i;
    if (rd_num(r, &ncoders) != 0 || ncoders == 0 || ncoders > 4) {
        return -1;
    }
    for (i = 0; i < ncoders; i++) {
        unsigned mainb = 0;
        unsigned id_size;
        unsigned char id[8];
        uint64_t nin = 1;
        uint64_t nout = 1;
        unsigned j;
        if (rd_byte(r, &mainb) != 0) {
            return -1;
        }
        id_size = mainb & 0x0Fu;
        if (id_size == 0 || id_size > 8) {
            return -1;
        }
        for (j = 0; j < id_size; j++) {
            unsigned b = 0;
            if (rd_byte(r, &b) != 0) {
                return -1;
            }
            id[j] = (unsigned char)b;
        }
        if (mainb & 0x80u) {
            return -2;
        }
        if (mainb & 0x10u) {
            if (rd_num(r, &nin) != 0 || rd_num(r, &nout) != 0) {
                return -1;
            }
        }
        if (nin > 8 || nout > 8) {
            return -1;
        }
        num_in += nin;
        num_out += nout;
        if (mainb & 0x20u) {
            uint64_t props = 0;
            if (rd_num(r, &props) != 0 || props > 8 || rd_skip(r, props) != 0) {
                return -1;
            }
            if (i == 0 && props <= 8) {
                folder->props_n = (unsigned)props;
                memcpy(folder->props, r->p + r->i - (size_t)props, (size_t)props);
            }
        }
        if (i == 0) {
            /* shortcut: BCJ, PPMd, and BZip2 folders are rejected. */
            if (id_size == 1 && id[0] == 0x00) {
                folder->codec = CODEC_COPY;
            } else if (id_size == 1 && id[0] == 0x21) {
                folder->codec = CODEC_LZMA2;
            } else if (id_size == 3 && id[0] == 0x03 && id[1] == 0x01 && id[2] == 0x01) {
                folder->codec = CODEC_LZMA;
            } else {
                return -2;
            }
        }
    }
    if (ncoders != 1 || num_in != 1 || num_out != 1) {
        return -2;
    }
    return 0;
}

static void folders_free(Folder *folders, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        free(folders[i].subs);
    }
    free(folders);
}

static int parse_pack(Rd *r, uint64_t *pack_pos, uint64_t **sizes, size_t *nsizes) {
    uint64_t nstreams = 0;
    unsigned id = 0;
    size_t i;
    if (rd_num(r, pack_pos) != 0 || rd_num(r, &nstreams) != 0 || nstreams == 0 || nstreams > 100000) {
        return -1;
    }
    *sizes = calloc((size_t)nstreams, sizeof **sizes);
    if (!*sizes) {
        return -1;
    }
    *nsizes = (size_t)nstreams;
    for (;;) {
        if (rd_byte(r, &id) != 0) {
            return -1;
        }
        if (id == K_END) {
            return 0;
        }
        if (id == K_SIZE) {
            for (i = 0; i < *nsizes; i++) {
                if (rd_num(r, &(*sizes)[i]) != 0) {
                    return -1;
                }
            }
        } else if (id == K_CRC) {
            if (crc_digests(r, *nsizes) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
}

static int parse_folders(Rd *r, Folder **folders, size_t *nfolders, size_t npack) {
    uint64_t n = 0;
    unsigned external = 0;
    unsigned id = 0;
    size_t i;
    uint32_t next_pack = 0;
    if (rd_num(r, &n) != 0 || n == 0 || n > 100000 || rd_byte(r, &external) != 0 || external != 0) {
        return -1;
    }
    *folders = calloc((size_t)n, sizeof **folders);
    if (!*folders) {
        return -1;
    }
    *nfolders = (size_t)n;
    for (i = 0; i < *nfolders; i++) {
        int pr = parse_folder(r, &(*folders)[i]);
        if (pr != 0) {
            return pr;
        }
        (*folders)[i].pack_index = next_pack++;
        if ((*folders)[i].pack_index >= npack) {
            return -1;
        }
    }
    for (;;) {
        if (rd_byte(r, &id) != 0) {
            return -1;
        }
        if (id == K_END) {
            return 0;
        }
        if (id == K_CODER_UNPACK) {
            for (i = 0; i < *nfolders; i++) {
                if (rd_num(r, &(*folders)[i].unpack) != 0 || (*folders)[i].unpack > SEVEN_MAX) {
                    return -1;
                }
            }
        } else if (id == K_CRC) {
            if (crc_digests(r, *nfolders) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
}

static int parse_substreams(Rd *r, Folder *folders, size_t nfolders) {
    unsigned id = 0;
    size_t i;
    for (i = 0; i < nfolders; i++) {
        folders[i].nsub = 1;
    }
    for (;;) {
        if (rd_byte(r, &id) != 0) {
            return -1;
        }
        if (id == K_END) {
            break;
        }
        if (id == K_NUM_UNPACK) {
            for (i = 0; i < nfolders; i++) {
                uint64_t ns = 0;
                if (rd_num(r, &ns) != 0 || ns == 0 || ns > 100000) {
                    return -1;
                }
                folders[i].nsub = (uint32_t)ns;
            }
        } else if (id == K_SIZE) {
            for (i = 0; i < nfolders; i++) {
                uint64_t sum = 0;
                uint32_t s;
                if (folders[i].nsub == 0) {
                    return -1;
                }
                folders[i].subs = calloc(folders[i].nsub, sizeof *folders[i].subs);
                if (!folders[i].subs) {
                    return -1;
                }
                for (s = 0; s + 1 < folders[i].nsub; s++) {
                    if (rd_num(r, &folders[i].subs[s]) != 0) {
                        return -1;
                    }
                    if (folders[i].subs[s] > folders[i].unpack - sum) {
                        return -1;
                    }
                    sum += folders[i].subs[s];
                }
                folders[i].subs[folders[i].nsub - 1] = folders[i].unpack - sum;
            }
        } else if (id == K_CRC) {
            size_t total = 0;
            for (i = 0; i < nfolders; i++) {
                total += folders[i].nsub;
            }
            if (crc_digests(r, total) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    for (i = 0; i < nfolders; i++) {
        if (!folders[i].subs) {
            folders[i].subs = calloc(1, sizeof *folders[i].subs);
            if (!folders[i].subs) {
                return -1;
            }
            folders[i].subs[0] = folders[i].unpack;
        }
    }
    return 0;
}

static int parse_streams(Rd *r, uint64_t *pack_pos, uint64_t **sizes, size_t *nsizes, Folder **folders, size_t *nfolders) {
    unsigned id = 0;
    for (;;) {
        if (rd_byte(r, &id) != 0) {
            return -1;
        }
        if (id == K_END) {
            return 0;
        }
        if (id == K_PACK) {
            if (parse_pack(r, pack_pos, sizes, nsizes) != 0) {
                return -1;
            }
        } else if (id == K_UNPACK) {
            unsigned nid = 0;
            if (rd_byte(r, &nid) != 0 || nid != K_FOLDER) {
                return -1;
            }
            {
                int pr = parse_folders(r, folders, nfolders, *nsizes);
                if (pr != 0) {
                    return pr;
                }
            }
        } else if (id == K_SUBSTREAMS) {
            if (parse_substreams(r, *folders, *nfolders) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
}

static int parse_names(const unsigned char *p, size_t n, FileEnt *files, size_t nfiles) {
    size_t i = 0;
    size_t f;
    if (n == 0 || p[0] != 0) {
        return -1;
    }
    i = 1;
    for (f = 0; f < nfiles; f++) {
        size_t start = i;
        int saw = 0;
        while (i + 1 < n) {
            uint16_t ch = rd_u16(p + i);
            i += 2;
            if (ch == 0) {
                saw = 1;
                break;
            }
        }
        if (!saw) {
            return -1;
        }
        files[f].name = utf16_name(p + start, i - start - 2);
        if (!files[f].name) {
            return -1;
        }
        {
            size_t ln = strlen(files[f].name);
            if (ln && (files[f].name[ln - 1] == '/' || files[f].name[ln - 1] == '\\')) {
                files[f].is_dir = 1;
            }
        }
    }
    return 0;
}

static int parse_files(Rd *r, FileEnt **files, size_t *nfiles) {
    uint64_t n = 0;
    unsigned id = 0;
    unsigned char *empty = NULL;
    int rc = -1;
    if (rd_num(r, &n) != 0 || n > 100000) {
        return -1;
    }
    *files = calloc((size_t)n ? (size_t)n : 1, sizeof **files);
    if (!*files) {
        return -1;
    }
    *nfiles = (size_t)n;
    for (;;) {
        uint64_t sz = 0;
        size_t mark;
        if (rd_byte(r, &id) != 0) {
            goto done;
        }
        if (id == K_END) {
            rc = 0;
            goto done;
        }
        if (rd_num(r, &sz) != 0 || sz > r->n - r->i) {
            goto done;
        }
        mark = r->i;
        if (id == K_EMPTY_STREAM) {
            empty = calloc(*nfiles ? *nfiles : 1, 1);
            if (!empty || rd_bits(r, empty, *nfiles) != 0) {
                goto done;
            }
        } else if (id == K_NAME) {
            if (parse_names(r->p + r->i, (size_t)sz, *files, *nfiles) != 0) {
                goto done;
            }
            r->i += (size_t)sz;
        } else if (rd_skip(r, sz) != 0) {
            goto done;
        }
        if (r->i != mark + (size_t)sz && id != K_EMPTY_STREAM) {
            goto done;
        }
        if (id == K_EMPTY_STREAM && r->i > mark + (size_t)sz) {
            goto done;
        }
        if (id == K_EMPTY_STREAM) {
            r->i = mark + (size_t)sz;
        }
    }
done:
    if (rc == 0 && empty) {
        size_t i;
        for (i = 0; i < *nfiles; i++) {
            (*files)[i].empty = empty[i] || (*files)[i].is_dir;
        }
    }
    free(empty);
    return rc;
}

static int pack_slice(const unsigned char *arc, size_t arc_n, uint64_t pack_pos, const uint64_t *sizes, size_t nsizes,
                      uint32_t index, const unsigned char **ptr, size_t *len) {
    uint64_t off;
    size_t i;
    if ((size_t)index >= nsizes || pack_pos > arc_n || arc_n - pack_pos < 32) {
        return -1;
    }
    off = 32 + pack_pos;
    for (i = 0; i < index; i++) {
        if (sizes[i] > arc_n - off) {
            return -1;
        }
        off += sizes[i];
    }
    if (sizes[index] > arc_n - off) {
        return -1;
    }
    *ptr = arc + off;
    *len = (size_t)sizes[index];
    return 0;
}

static int lzma2_dic_ok(unsigned prop) {
    uint32_t dic;
    if (prop > 40) {
        return 0;
    }
    if (prop == 40) {
        return 0;
    }
    dic = (2u | (prop & 1u)) << (prop / 2 + 11);
    return dic > 0 && dic <= SEVEN_DIC;
}

static int decode_folder(const Folder *folder, const unsigned char *src, size_t sn, unsigned char **out, Err *e) {
    static const ISzAlloc alloc = {sz_alloc, sz_free};
    unsigned char *dst;
    SizeT dest_len;
    SizeT src_len;
    ELzmaStatus status;
    SRes sr;
    if (folder->unpack > SEVEN_MAX) {
        err_set(e, "7z member is too large");
        return -1;
    }
    dst = malloc(folder->unpack ? (size_t)folder->unpack : 1);
    if (!dst) {
        err_set(e, "out of memory");
        return -1;
    }
    dest_len = (SizeT)folder->unpack;
    src_len = sn;
    if (folder->codec == CODEC_COPY) {
        if (sn < folder->unpack) {
            free(dst);
            err_set(e, "7z data is corrupt");
            return -1;
        }
        if (folder->unpack) {
            memcpy(dst, src, (size_t)folder->unpack);
        }
        *out = dst;
        return 0;
    }
    if (folder->codec == CODEC_LZMA2) {
        CLzma2Dec dec;
        if (folder->props_n != 1 || !lzma2_dic_ok(folder->props[0])) {
            free(dst);
            err_set(e, "7z method is unsupported");
            return -1;
        }
        Lzma2Dec_Construct(&dec);
        if (Lzma2Dec_Allocate(&dec, folder->props[0], &alloc) != SZ_OK) {
            free(dst);
            err_set(e, "out of memory");
            return -1;
        }
        Lzma2Dec_Init(&dec);
        sr = Lzma2Dec_DecodeToBuf(&dec, dst, &dest_len, src, &src_len, LZMA_FINISH_END, &status);
        Lzma2Dec_Free(&dec, &alloc);
    } else if (folder->codec == CODEC_LZMA) {
        uint32_t dic;
        if (folder->props_n != 5) {
            free(dst);
            err_set(e, "7z method is unsupported");
            return -1;
        }
        dic = rd_u32(folder->props + 1);
        if (dic == 0 || dic > SEVEN_DIC) {
            free(dst);
            err_set(e, "7z method is unsupported");
            return -1;
        }
        sr = LzmaDecode(dst, &dest_len, src, &src_len, folder->props, 5, LZMA_FINISH_ANY, &status, &alloc);
    } else {
        free(dst);
        err_set(e, "7z method is unsupported");
        return -1;
    }
    if (sr != SZ_OK || dest_len != folder->unpack) {
        free(dst);
        err_set(e, "7z data is corrupt");
        return -1;
    }
    *out = dst;
    return 0;
}

static int assign_files(FileEnt *files, size_t nfiles, Folder *folders, size_t nfolders, Err *e) {
    size_t fi = 0;
    size_t fo;
    for (fo = 0; fo < nfolders; fo++) {
        uint64_t off = 0;
        uint32_t s;
        for (s = 0; s < folders[fo].nsub; s++) {
            while (fi < nfiles && files[fi].empty) {
                fi++;
            }
            if (fi >= nfiles) {
                err_set(e, "7z header is corrupt");
                return -1;
            }
            files[fi].folder = (uint32_t)fo;
            files[fi].offset = off;
            files[fi].size = folders[fo].subs[s];
            off += folders[fo].subs[s];
            fi++;
        }
        if (off != folders[fo].unpack) {
            err_set(e, "7z header is corrupt");
            return -1;
        }
    }
    while (fi < nfiles) {
        if (!files[fi].empty) {
            err_set(e, "7z header is corrupt");
            return -1;
        }
        fi++;
    }
    return 0;
}

static int fill_files(const unsigned char *arc, size_t arc_n, uint64_t pack_pos, const uint64_t *sizes, size_t nsizes,
                      Folder *folders, size_t nfolders, FileEnt *files, size_t nfiles, Seven *out, Err *e) {
    unsigned char **unpacked;
    size_t i;
    size_t kept = 0;
    if (assign_files(files, nfiles, folders, nfolders, e) != 0) {
        return -1;
    }
    unpacked = calloc(nfolders ? nfolders : 1, sizeof *unpacked);
    if (!unpacked) {
        err_set(e, "out of memory");
        return -1;
    }
    for (i = 0; i < nfolders; i++) {
        const unsigned char *src = NULL;
        size_t sn = 0;
        if (pack_slice(arc, arc_n, pack_pos, sizes, nsizes, folders[i].pack_index, &src, &sn) != 0 ||
            decode_folder(&folders[i], src, sn, &unpacked[i], e) != 0) {
            for (size_t j = 0; j < i; j++) {
                free(unpacked[j]);
            }
            free(unpacked);
            return -1;
        }
    }
    out->files = calloc(nfiles ? nfiles : 1, sizeof *out->files);
    if (!out->files) {
        for (i = 0; i < nfolders; i++) {
            free(unpacked[i]);
        }
        free(unpacked);
        err_set(e, "out of memory");
        return -1;
    }
    for (i = 0; i < nfiles; i++) {
        unsigned char *data;
        if (files[i].empty || files[i].is_dir || !files[i].name || !files[i].name[0]) {
            continue;
        }
        if (files[i].folder >= nfolders || files[i].offset > folders[files[i].folder].unpack ||
            files[i].size > folders[files[i].folder].unpack - files[i].offset) {
            seven_free(out);
            for (size_t j = 0; j < nfolders; j++) {
                free(unpacked[j]);
            }
            free(unpacked);
            err_set(e, "7z header is corrupt");
            return -1;
        }
        data = malloc(files[i].size ? (size_t)files[i].size : 1);
        if (!data) {
            seven_free(out);
            for (size_t j = 0; j < nfolders; j++) {
                free(unpacked[j]);
            }
            free(unpacked);
            err_set(e, "out of memory");
            return -1;
        }
        if (files[i].size) {
            memcpy(data, unpacked[files[i].folder] + files[i].offset, (size_t)files[i].size);
        }
        out->files[kept].name = files[i].name;
        files[i].name = NULL;
        out->files[kept].data = data;
        out->files[kept].len = (size_t)files[i].size;
        kept++;
    }
    out->n = kept;
    for (i = 0; i < nfolders; i++) {
        free(unpacked[i]);
    }
    free(unpacked);
    return 0;
}

static int parse_header(const unsigned char *arc, size_t arc_n, Rd *r, Seven *out, Err *e, int depth);

static int read_encoded_header(const unsigned char *arc, size_t arc_n, Rd *r, Seven *out, Err *e, int depth) {
    uint64_t pack_pos = 0;
    uint64_t *sizes = NULL;
    size_t nsizes = 0;
    Folder *folders = NULL;
    size_t nfolders = 0;
    unsigned char *decoded = NULL;
    const unsigned char *src = NULL;
    size_t sn = 0;
    uint64_t unpack;
    Rd inner;
    int rc;
    if (depth > 1) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    if (parse_streams(r, &pack_pos, &sizes, &nsizes, &folders, &nfolders) != 0 || nfolders != 1) {
        folders_free(folders, nfolders);
        free(sizes);
        if (!e->msg[0]) {
            err_set(e, "7z header is corrupt");
        }
        return -1;
    }
    unpack = folders[0].unpack;
    if (pack_slice(arc, arc_n, pack_pos, sizes, nsizes, folders[0].pack_index, &src, &sn) != 0 ||
        decode_folder(&folders[0], src, sn, &decoded, e) != 0) {
        folders_free(folders, nfolders);
        free(sizes);
        return -1;
    }
    folders_free(folders, nfolders);
    free(sizes);
    inner.p = decoded;
    inner.n = (size_t)unpack;
    inner.i = 0;
    rc = parse_header(arc, arc_n, &inner, out, e, depth + 1);
    free(decoded);
    return rc;
}

static int parse_header(const unsigned char *arc, size_t arc_n, Rd *r, Seven *out, Err *e, int depth) {
    unsigned id = 0;
    uint64_t pack_pos = 0;
    uint64_t *sizes = NULL;
    size_t nsizes = 0;
    Folder *folders = NULL;
    size_t nfolders = 0;
    FileEnt *files = NULL;
    size_t nfiles = 0;
    int rc = -1;
    if (rd_byte(r, &id) != 0) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    if (id == K_ENCODED) {
        return read_encoded_header(arc, arc_n, r, out, e, depth);
    }
    if (id != K_HEADER) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    for (;;) {
        if (rd_byte(r, &id) != 0) {
            err_set(e, "7z header is corrupt");
            goto done;
        }
        if (id == K_END) {
            break;
        }
        if (id == K_MAIN_STREAMS) {
            int pr = parse_streams(r, &pack_pos, &sizes, &nsizes, &folders, &nfolders);
            if (pr != 0) {
                err_set(e, pr == -2 ? "7z method is unsupported" : "7z header is corrupt");
                goto done;
            }
        } else if (id == K_FILES) {
            if (parse_files(r, &files, &nfiles) != 0) {
                err_set(e, "7z header is corrupt");
                goto done;
            }
        } else if (id == K_ARCHIVE_PROPS) {
            unsigned pid = 0;
            for (;;) {
                uint64_t sz = 0;
                if (rd_byte(r, &pid) != 0) {
                    err_set(e, "7z header is corrupt");
                    goto done;
                }
                if (pid == K_END) {
                    break;
                }
                if (rd_num(r, &sz) != 0 || rd_skip(r, sz) != 0) {
                    err_set(e, "7z header is corrupt");
                    goto done;
                }
            }
        } else {
            err_set(e, "7z method is unsupported");
            goto done;
        }
    }
    if (fill_files(arc, arc_n, pack_pos, sizes, nsizes, folders, nfolders, files, nfiles, out, e) != 0) {
        goto done;
    }
    rc = 0;
done:
    folders_free(folders, nfolders);
    free(sizes);
    if (files) {
        size_t i;
        for (i = 0; i < nfiles; i++) {
            free(files[i].name);
        }
        free(files);
    }
    return rc;
}

void seven_free(Seven *s) {
    size_t i;
    if (!s) {
        return;
    }
    for (i = 0; i < s->n; i++) {
        free(s->files[i].name);
        free(s->files[i].data);
    }
    free(s->files);
    s->files = NULL;
    s->n = 0;
}

int seven_read(const unsigned char *bytes, size_t len, Seven *out, Err *e) {
    static const unsigned char sig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    uint64_t hoff;
    uint64_t hsz;
    uint32_t hcrc;
    Rd r;
    memset(out, 0, sizeof *out);
    if (!bytes || len < 32 || memcmp(bytes, sig, 6) != 0) {
        err_set(e, "not a 7z archive");
        return -1;
    }
    if ((uint32_t)crc32(0L, bytes + 12, 20) != rd_u32(bytes + 8)) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    hoff = rd_u64(bytes + 12);
    hsz = rd_u64(bytes + 20);
    hcrc = rd_u32(bytes + 28);
    if (hoff > len - 32 || hsz > len - 32 - hoff || hsz > SEVEN_MAX || hsz > 0xFFFFFFFFu) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    if ((uint32_t)crc32(0L, bytes + 32 + (size_t)hoff, (uInt)hsz) != hcrc) {
        err_set(e, "7z header is corrupt");
        return -1;
    }
    r.p = bytes + 32 + (size_t)hoff;
    r.n = (size_t)hsz;
    r.i = 0;
    if (parse_header(bytes, len, &r, out, e, 0) != 0) {
        seven_free(out);
        if (!e->msg[0]) {
            err_set(e, "7z header is corrupt");
        }
        return -1;
    }
    return 0;
}
