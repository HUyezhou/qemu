/*
 * Command line utility to exercise the QEMU I/O path.
 *
 * Copyright (C) 2009 Red Hat, Inc.
 * Copyright (c) 2003-2005 Silicon Graphics, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu-common.h"
#include "block/block_int.h"
#include "block/qapi.h"
#include "cmd.h"

#define CMD_NOFILE_OK   0x01

int qemuio_misalign;

static cmdinfo_t *cmdtab;
static int ncmds;

static int compare_cmdname(const void *a, const void *b)
{
    return strcmp(((const cmdinfo_t *)a)->name,
                  ((const cmdinfo_t *)b)->name);
}

void qemuio_add_command(const cmdinfo_t *ci)
{
    cmdtab = g_realloc(cmdtab, ++ncmds * sizeof(*cmdtab));
    cmdtab[ncmds - 1] = *ci;
    qsort(cmdtab, ncmds, sizeof(*cmdtab), compare_cmdname);
}

int qemuio_command_usage(const cmdinfo_t *ci)
{
    printf("%s %s -- %s\n", ci->name, ci->args, ci->oneline);
    return 0;
}

static int init_check_command(BlockDriverState *bs, const cmdinfo_t *ct)
{
    if (ct->flags & CMD_FLAG_GLOBAL) {
        return 1;
    }
    if (!(ct->flags & CMD_NOFILE_OK) && !bs) {
        fprintf(stderr, "no file open, try 'help open'\n");
        return 0;
    }
    return 1;
}

static int command(const cmdinfo_t *ct, int argc, char **argv)
{
    char *cmd = argv[0];

    if (!init_check_command(qemuio_bs, ct)) {
        return 0;
    }

    if (argc - 1 < ct->argmin || (ct->argmax != -1 && argc - 1 > ct->argmax)) {
        if (ct->argmax == -1) {
            fprintf(stderr,
                    "bad argument count %d to %s, expected at least %d arguments\n",
                    argc-1, cmd, ct->argmin);
        } else if (ct->argmin == ct->argmax) {
            fprintf(stderr,
                    "bad argument count %d to %s, expected %d arguments\n",
                    argc-1, cmd, ct->argmin);
        } else {
            fprintf(stderr,
                    "bad argument count %d to %s, expected between %d and %d arguments\n",
                    argc-1, cmd, ct->argmin, ct->argmax);
        }
        return 0;
    }
    optind = 0;
    return ct->cfunc(qemuio_bs, argc, argv);
}

static const cmdinfo_t *find_command(const char *cmd)
{
    cmdinfo_t *ct;

    for (ct = cmdtab; ct < &cmdtab[ncmds]; ct++) {
        if (strcmp(ct->name, cmd) == 0 ||
            (ct->altname && strcmp(ct->altname, cmd) == 0))
        {
            return (const cmdinfo_t *)ct;
        }
    }
    return NULL;
}

static char **breakline(char *input, int *count)
{
    int c = 0;
    char *p;
    char **rval = g_malloc0(sizeof(char *));
    char **tmp;

    while (rval && (p = qemu_strsep(&input, " ")) != NULL) {
        if (!*p) {
            continue;
        }
        c++;
        tmp = g_realloc(rval, sizeof(*rval) * (c + 1));
        if (!tmp) {
            g_free(rval);
            rval = NULL;
            c = 0;
            break;
        } else {
            rval = tmp;
        }
        rval[c - 1] = p;
        rval[c] = NULL;
    }
    *count = c;
    return rval;
}

static int64_t cvtnum(const char *s)
{
    char *end;
    return strtosz_suffix(s, &end, STRTOSZ_DEFSUFFIX_B);
}

/*
 * Parse the pattern argument to various sub-commands.
 *
 * Because the pattern is used as an argument to memset it must evaluate
 * to an unsigned integer that fits into a single byte.
 */
static int parse_pattern(const char *arg)
{
    char *endptr = NULL;
    long pattern;

    pattern = strtol(arg, &endptr, 0);
    if (pattern < 0 || pattern > UCHAR_MAX || *endptr != '\0') {
        printf("%s is not a valid pattern byte\n", arg);
        return -1;
    }

    return pattern;
}

/*
 * Memory allocation helpers.
 *
 * Make sure memory is aligned by default, or purposefully misaligned if
 * that is specified on the command line.
 */

#define MISALIGN_OFFSET     16
static void *qemu_io_alloc(BlockDriverState *bs, size_t len, int pattern)
{
    void *buf;

    if (qemuio_misalign) {
        len += MISALIGN_OFFSET;
    }
    buf = qemu_blockalign(bs, len);
    memset(buf, pattern, len);
    if (qemuio_misalign) {
        buf += MISALIGN_OFFSET;
    }
    return buf;
}

static void qemu_io_free(void *p)
{
    if (qemuio_misalign) {
        p -= MISALIGN_OFFSET;
    }
    qemu_vfree(p);
}

static void dump_buffer(const void *buffer, int64_t offset, int len)
{
    int i, j;
    const uint8_t *p;

    for (i = 0, p = buffer; i < len; i += 16) {
        const uint8_t *s = p;

        printf("%08" PRIx64 ":  ", offset + i);
        for (j = 0; j < 16 && i + j < len; j++, p++) {
            printf("%02x ", *p);
        }
        printf(" ");
        for (j = 0; j < 16 && i + j < len; j++, s++) {
            if (isalnum(*s)) {
                printf("%c", *s);
            } else {
                printf(".");
            }
        }
        printf("\n");
    }
}

static void print_report(const char *op, struct timeval *t, int64_t offset,
                         int count, int total, int cnt, int Cflag)
{
    char s1[64], s2[64], ts[64];

    timestr(t, ts, sizeof(ts), Cflag ? VERBOSE_FIXED_TIME : 0);
    if (!Cflag) {
        cvtstr((double)total, s1, sizeof(s1));
        cvtstr(tdiv((double)total, *t), s2, sizeof(s2));
        printf("%s %d/%d bytes at offset %" PRId64 "\n",
               op, total, count, offset);
        printf("%s, %d ops; %s (%s/sec and %.4f ops/sec)\n",
               s1, cnt, ts, s2, tdiv((double)cnt, *t));
    } else {/* bytes,ops,time,bytes/sec,ops/sec */
        printf("%d,%d,%s,%.3f,%.3f\n",
            total, cnt, ts,
            tdiv((double)total, *t),
            tdiv((double)cnt, *t));
    }
}

/*
 * Parse multiple length statements for vectored I/O, and construct an I/O
 * vector matching it.
 */
static void *
create_iovec(BlockDriverState *bs, QEMUIOVector *qiov, char **argv, int nr_iov,
             int pattern)
{
    size_t *sizes = g_new0(size_t, nr_iov);
    size_t count = 0;
    void *buf = NULL;
    void *p;
    int i;

    for (i = 0; i < nr_iov; i++) {
        char *arg = argv[i];
        int64_t len;

        len = cvtnum(arg);
        if (len < 0) {
            printf("non-numeric length argument -- %s\n", arg);
            goto fail;
        }

        /* should be SIZE_T_MAX, but that doesn't exist */
        if (len > INT_MAX) {
            printf("too large length argument -- %s\n", arg);
            goto fail;
        }

        if (len & 0x1ff) {
            printf("length argument %" PRId64
                   " is not sector aligned\n", len);
            goto fail;
        }

        sizes[i] = len;
        count += len;
    }

    qemu_iovec_init(qiov, nr_iov);

    buf = p = qemu_io_alloc(bs, count, pattern);

    for (i = 0; i < nr_iov; i++) {
        qemu_iovec_add(qiov, p, sizes[i]);
        p += sizes[i];
    }

fail:
    g_free(sizes);
    return buf;
}

static int do_read(BlockDriverState *bs, char *buf, int64_t offset, int count,
                   int *total)
{
    int ret;

    ret = bdrv_read(bs, offset >> 9, (uint8_t *)buf, count >> 9);
    if (ret < 0) {
        return ret;
    }
    *total = count;
    return 1;
}

static int do_write(BlockDriverState *bs, char *buf, int64_t offset, int count,
                    int *total)
{
    int ret;

    ret = bdrv_write(bs, offset >> 9, (uint8_t *)buf, count >> 9);
    if (ret < 0) {
        return ret;
    }
    *total = count;
    return 1;
}

static int do_pread(BlockDriverState *bs, char *buf, int64_t offset, int count,
                    int *total)
{
    *total = bdrv_pread(bs, offset, (uint8_t *)buf, count);
    if (*total < 0) {
        return *total;
    }
    return 1;
}

static int do_pwrite(BlockDriverState *bs, char *buf, int64_t offset, int count,
                     int *total)
{
    *total = bdrv_pwrite(bs, offset, (uint8_t *)buf, count);
    if (*total < 0) {
        return *total;
    }
    return 1;
}

typedef struct {
    BlockDriverState *bs;
    int64_t offset;
    int count;
    int *total;
    int ret;
    bool done;
} CoWriteZeroes;

static void coroutine_fn co_write_zeroes_entry(void *opaque)
{
    CoWriteZeroes *data = opaque;

    data->ret = bdrv_co_write_zeroes(data->bs, data->offset / BDRV_SECTOR_SIZE,
                                     data->count / BDRV_SECTOR_SIZE, 0);
    data->done = true;
    if (data->ret < 0) {
        *data->total = data->ret;
        return;
    }

    *data->total = data->count;
}

static int do_co_write_zeroes(BlockDriverState *bs, int64_t offset, int count,
                              int *total)
{
    Coroutine *co;
    CoWriteZeroes data = {
        .bs     = bs,
        .offset = offset,
        .count  = count,
        .total  = total,
        .done   = false,
    };

    co = qemu_coroutine_create(co_write_zeroes_entry);
    qemu_coroutine_enter(co, &data);
    while (!data.done) {
        qemu_aio_wait();
    }
    if (data.ret < 0) {
        return data.ret;
    } else {
        return 1;
    }
}

static int do_write_compressed(BlockDriverState *bs, char *buf, int64_t offset,
                               int count, int *total)
{
    int ret;

    ret = bdrv_write_compressed(bs, offset >> 9, (uint8_t *)buf, count >> 9);
    if (ret < 0) {
        return ret;
    }
    *total = count;
    return 1;
}

static int do_load_vmstate(BlockDriverState *bs, char *buf, int64_t offset,
                           int count, int *total)
{
    *total = bdrv_load_vmstate(bs, (uint8_t *)buf, offset, count);
    if (*total < 0) {
        return *total;
    }
    return 1;
}

static int do_save_vmstate(BlockDriverState *bs, char *buf, int64_t offset,
                           int count, int *total)
{
    *total = bdrv_save_vmstate(bs, (uint8_t *)buf, offset, count);
    if (*total < 0) {
        return *total;
    }
    return 1;
}

#define NOT_DONE 0x7fffffff
static void aio_rw_done(void *opaque, int ret)
{
    *(int *)opaque = ret;
}

static int do_aio_readv(BlockDriverState *bs, QEMUIOVector *qiov,
                        int64_t offset, int *total)
{
    int async_ret = NOT_DONE;

    bdrv_aio_readv(bs, offset >> 9, qiov, qiov->size >> 9,
                   aio_rw_done, &async_ret);
    while (async_ret == NOT_DONE) {
        main_loop_wait(false);
    }

    *total = qiov->size;
    return async_ret < 0 ? async_ret : 1;
}

static int do_aio_writev(BlockDriverState *bs, QEMUIOVector *qiov,
                         int64_t offset, int *total)
{
    int async_ret = NOT_DONE;

    bdrv_aio_writev(bs, offset >> 9, qiov, qiov->size >> 9,
                    aio_rw_done, &async_ret);
    while (async_ret == NOT_DONE) {
        main_loop_wait(false);
    }

    *total = qiov->size;
    return async_ret < 0 ? async_ret : 1;
}

struct multiwrite_async_ret {
    int num_done;
    int error;
};

static void multiwrite_cb(void *opaque, int ret)
{
    struct multiwrite_async_ret *async_ret = opaque;

    async_ret->num_done++;
    if (ret < 0) {
        async_ret->error = ret;
    }
}

static int do_aio_multiwrite(BlockDriverState *bs, BlockRequest* reqs,
                             int num_reqs, int *total)
{
    int i, ret;
    struct multiwrite_async_ret async_ret = {
        .num_done = 0,
        .error = 0,
    };

    *total = 0;
    for (i = 0; i < num_reqs; i++) {
        reqs[i].cb = multiwrite_cb;
        reqs[i].opaque = &async_ret;
        *total += reqs[i].qiov->size;
    }

    ret = bdrv_aio_multiwrite(bs, reqs, num_reqs);
    if (ret < 0) {
        return ret;
    }

    while (async_ret.num_done < num_reqs) {
        main_loop_wait(false);
    }

    return async_ret.error < 0 ? async_ret.error : 1;
}

static void read_help(void)
{
    printf(
"\n"
" reads a range of bytes from the given offset\n"
"\n"
" Example:\n"
" 'read -v 512 1k' - dumps 1 kilobyte read from 512 bytes into the file\n"
"\n"
" Reads a segment of the currently open file, optionally dumping it to the\n"
" standard output stream (with -v option) for subsequent inspection.\n"
" -b, -- read from the VM state rather than the virtual disk\n"
" -C, -- report statistics in a machine parsable format\n"
" -l, -- length for pattern verification (only with -P)\n"
" -p, -- use bdrv_pread to read the file\n"
" -P, -- use a pattern to verify read data\n"
" -q, -- quiet mode, do not show I/O statistics\n"
" -s, -- start offset for pattern verification (only with -P)\n"
" -v, -- dump buffer to standard output\n"
"\n");
}

static int read_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t read_cmd = {
    .name       = "read",
    .altname    = "r",
    .cfunc      = read_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-abCpqv] [-P pattern [-s off] [-l len]] off len",
    .oneline    = "reads a number of bytes at a specified offset",
    .help       = read_help,
};

static int read_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, pflag = 0, qflag = 0, vflag = 0;
    int Pflag = 0, sflag = 0, lflag = 0, bflag = 0;
    int c, cnt;
    char *buf;
    int64_t offset;
    int count;
    /* Some compilers get confused and warn if this is not initialized.  */
    int total = 0;
    int pattern = 0, pattern_offset = 0, pattern_count = 0;

    while ((c = getopt(argc, argv, "bCl:pP:qs:v")) != EOF) {
        switch (c) {
        case 'b':
            bflag = 1;
            break;
        case 'C':
            Cflag = 1;
            break;
        case 'l':
            lflag = 1;
            pattern_count = cvtnum(optarg);
            if (pattern_count < 0) {
                printf("non-numeric length argument -- %s\n", optarg);
                return 0;
            }
            break;
        case 'p':
            pflag = 1;
            break;
        case 'P':
            Pflag = 1;
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                return 0;
            }
            break;
        case 'q':
            qflag = 1;
            break;
        case 's':
            sflag = 1;
            pattern_offset = cvtnum(optarg);
            if (pattern_offset < 0) {
                printf("non-numeric length argument -- %s\n", optarg);
                return 0;
            }
            break;
        case 'v':
            vflag = 1;
            break;
        default:
            return qemuio_command_usage(&read_cmd);
        }
    }

    if (optind != argc - 2) {
        return qemuio_command_usage(&read_cmd);
    }

    if (bflag && pflag) {
        printf("-b and -p cannot be specified at the same time\n");
        return 0;
    }

    offset = cvtnum(argv[optind]);
    if (offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    optind++;
    count = cvtnum(argv[optind]);
    if (count < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    if (!Pflag && (lflag || sflag)) {
        return qemuio_command_usage(&read_cmd);
    }

    if (!lflag) {
        pattern_count = count - pattern_offset;
    }

    if ((pattern_count < 0) || (pattern_count + pattern_offset > count))  {
        printf("pattern verification range exceeds end of read data\n");
        return 0;
    }

    if (!pflag) {
        if (offset & 0x1ff) {
            printf("offset %" PRId64 " is not sector aligned\n",
                   offset);
            return 0;
        }
        if (count & 0x1ff) {
            printf("count %d is not sector aligned\n",
                   count);
            return 0;
        }
    }

    buf = qemu_io_alloc(bs, count, 0xab);

    gettimeofday(&t1, NULL);
    if (pflag) {
        cnt = do_pread(bs, buf, offset, count, &total);
    } else if (bflag) {
        cnt = do_load_vmstate(bs, buf, offset, count, &total);
    } else {
        cnt = do_read(bs, buf, offset, count, &total);
    }
    gettimeofday(&t2, NULL);

    if (cnt < 0) {
        printf("read failed: %s\n", strerror(-cnt));
        goto out;
    }

    if (Pflag) {
        void *cmp_buf = g_malloc(pattern_count);
        memset(cmp_buf, pattern, pattern_count);
        if (memcmp(buf + pattern_offset, cmp_buf, pattern_count)) {
            printf("Pattern verification failed at offset %"
                   PRId64 ", %d bytes\n",
                   offset + pattern_offset, pattern_count);
        }
        g_free(cmp_buf);
    }

    if (qflag) {
        goto out;
    }

    if (vflag) {
        dump_buffer(buf, offset, count);
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, t1);
    print_report("read", &t2, offset, count, total, cnt, Cflag);

out:
    qemu_io_free(buf);

    return 0;
}

static void readv_help(void)
{
    printf(
"\n"
" reads a range of bytes from the given offset into multiple buffers\n"
"\n"
" Example:\n"
" 'readv -v 512 1k 1k ' - dumps 2 kilobytes read from 512 bytes into the file\n"
"\n"
" Reads a segment of the currently open file, optionally dumping it to the\n"
" standard output stream (with -v option) for subsequent inspection.\n"
" Uses multiple iovec buffers if more than one byte range is specified.\n"
" -C, -- report statistics in a machine parsable format\n"
" -P, -- use a pattern to verify read data\n"
" -v, -- dump buffer to standard output\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int readv_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t readv_cmd = {
    .name       = "readv",
    .cfunc      = readv_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cqv] [-P pattern ] off len [len..]",
    .oneline    = "reads a number of bytes at a specified offset",
    .help       = readv_help,
};

static int readv_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, qflag = 0, vflag = 0;
    int c, cnt;
    char *buf;
    int64_t offset;
    /* Some compilers get confused and warn if this is not initialized.  */
    int total = 0;
    int nr_iov;
    QEMUIOVector qiov;
    int pattern = 0;
    int Pflag = 0;

    while ((c = getopt(argc, argv, "CP:qv")) != EOF) {
        switch (c) {
        case 'C':
            Cflag = 1;
            break;
        case 'P':
            Pflag = 1;
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                return 0;
            }
            break;
        case 'q':
            qflag = 1;
            break;
        case 'v':
            vflag = 1;
            break;
        default:
            return qemuio_command_usage(&readv_cmd);
        }
    }

    if (optind > argc - 2) {
        return qemuio_command_usage(&readv_cmd);
    }


    offset = cvtnum(argv[optind]);
    if (offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }
    optind++;

    if (offset & 0x1ff) {
        printf("offset %" PRId64 " is not sector aligned\n",
               offset);
        return 0;
    }

    nr_iov = argc - optind;
    buf = create_iovec(bs, &qiov, &argv[optind], nr_iov, 0xab);
    if (buf == NULL) {
        return 0;
    }

    gettimeofday(&t1, NULL);
    cnt = do_aio_readv(bs, &qiov, offset, &total);
    gettimeofday(&t2, NULL);

    if (cnt < 0) {
        printf("readv failed: %s\n", strerror(-cnt));
        goto out;
    }

    if (Pflag) {
        void *cmp_buf = g_malloc(qiov.size);
        memset(cmp_buf, pattern, qiov.size);
        if (memcmp(buf, cmp_buf, qiov.size)) {
            printf("Pattern verification failed at offset %"
                   PRId64 ", %zd bytes\n", offset, qiov.size);
        }
        g_free(cmp_buf);
    }

    if (qflag) {
        goto out;
    }

    if (vflag) {
        dump_buffer(buf, offset, qiov.size);
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, t1);
    print_report("read", &t2, offset, qiov.size, total, cnt, Cflag);

out:
    qemu_iovec_destroy(&qiov);
    qemu_io_free(buf);
    return 0;
}

static void write_help(void)
{
    printf(
"\n"
" writes a range of bytes from the given offset\n"
"\n"
" Example:\n"
" 'write 512 1k' - writes 1 kilobyte at 512 bytes into the open file\n"
"\n"
" Writes into a segment of the currently open file, using a buffer\n"
" filled with a set pattern (0xcdcdcdcd).\n"
" -b, -- write to the VM state rather than the virtual disk\n"
" -c, -- write compressed data with bdrv_write_compressed\n"
" -p, -- use bdrv_pwrite to write the file\n"
" -P, -- use different pattern to fill file\n"
" -C, -- report statistics in a machine parsable format\n"
" -q, -- quiet mode, do not show I/O statistics\n"
" -z, -- write zeroes using bdrv_co_write_zeroes\n"
"\n");
}

static int write_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t write_cmd = {
    .name       = "write",
    .altname    = "w",
    .cfunc      = write_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-bcCpqz] [-P pattern ] off len",
    .oneline    = "writes a number of bytes at a specified offset",
    .help       = write_help,
};

static int write_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, pflag = 0, qflag = 0, bflag = 0, Pflag = 0, zflag = 0;
    int cflag = 0;
    int c, cnt;
    char *buf = NULL;
    int64_t offset;
    int count;
    /* Some compilers get confused and warn if this is not initialized.  */
    int total = 0;
    int pattern = 0xcd;

    while ((c = getopt(argc, argv, "bcCpP:qz")) != EOF) {
        switch (c) {
        case 'b':
            bflag = 1;
            break;
        case 'c':
            cflag = 1;
            break;
        case 'C':
            Cflag = 1;
            break;
        case 'p':
            pflag = 1;
            break;
        case 'P':
            Pflag = 1;
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                return 0;
            }
            break;
        case 'q':
            qflag = 1;
            break;
        case 'z':
            zflag = 1;
            break;
        default:
            return qemuio_command_usage(&write_cmd);
        }
    }

    if (optind != argc - 2) {
        return qemuio_command_usage(&write_cmd);
    }

    if (bflag + pflag + zflag > 1) {
        printf("-b, -p, or -z cannot be specified at the same time\n");
        return 0;
    }

    if (zflag && Pflag) {
        printf("-z and -P cannot be specified at the same time\n");
        return 0;
    }

    offset = cvtnum(argv[optind]);
    if (offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    optind++;
    count = cvtnum(argv[optind]);
    if (count < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    if (!pflag) {
        if (offset & 0x1ff) {
            printf("offset %" PRId64 " is not sector aligned\n",
                   offset);
            return 0;
        }

        if (count & 0x1ff) {
            printf("count %d is not sector aligned\n",
                   count);
            return 0;
        }
    }

    if (!zflag) {
        buf = qemu_io_alloc(bs, count, pattern);
    }

    gettimeofday(&t1, NULL);
    if (pflag) {
        cnt = do_pwrite(bs, buf, offset, count, &total);
    } else if (bflag) {
        cnt = do_save_vmstate(bs, buf, offset, count, &total);
    } else if (zflag) {
        cnt = do_co_write_zeroes(bs, offset, count, &total);
    } else if (cflag) {
        cnt = do_write_compressed(bs, buf, offset, count, &total);
    } else {
        cnt = do_write(bs, buf, offset, count, &total);
    }
    gettimeofday(&t2, NULL);

    if (cnt < 0) {
        printf("write failed: %s\n", strerror(-cnt));
        goto out;
    }

    if (qflag) {
        goto out;
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, t1);
    print_report("wrote", &t2, offset, count, total, cnt, Cflag);

out:
    if (!zflag) {
        qemu_io_free(buf);
    }

    return 0;
}

static void
writev_help(void)
{
    printf(
"\n"
" writes a range of bytes from the given offset source from multiple buffers\n"
"\n"
" Example:\n"
" 'write 512 1k 1k' - writes 2 kilobytes at 512 bytes into the open file\n"
"\n"
" Writes into a segment of the currently open file, using a buffer\n"
" filled with a set pattern (0xcdcdcdcd).\n"
" -P, -- use different pattern to fill file\n"
" -C, -- report statistics in a machine parsable format\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int writev_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t writev_cmd = {
    .name       = "writev",
    .cfunc      = writev_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cq] [-P pattern ] off len [len..]",
    .oneline    = "writes a number of bytes at a specified offset",
    .help       = writev_help,
};

static int writev_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, qflag = 0;
    int c, cnt;
    char *buf;
    int64_t offset;
    /* Some compilers get confused and warn if this is not initialized.  */
    int total = 0;
    int nr_iov;
    int pattern = 0xcd;
    QEMUIOVector qiov;

    while ((c = getopt(argc, argv, "CqP:")) != EOF) {
        switch (c) {
        case 'C':
            Cflag = 1;
            break;
        case 'q':
            qflag = 1;
            break;
        case 'P':
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                return 0;
            }
            break;
        default:
            return qemuio_command_usage(&writev_cmd);
        }
    }

    if (optind > argc - 2) {
        return qemuio_command_usage(&writev_cmd);
    }

    offset = cvtnum(argv[optind]);
    if (offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }
    optind++;

    if (offset & 0x1ff) {
        printf("offset %" PRId64 " is not sector aligned\n",
               offset);
        return 0;
    }

    nr_iov = argc - optind;
    buf = create_iovec(bs, &qiov, &argv[optind], nr_iov, pattern);
    if (buf == NULL) {
        return 0;
    }

    gettimeofday(&t1, NULL);
    cnt = do_aio_writev(bs, &qiov, offset, &total);
    gettimeofday(&t2, NULL);

    if (cnt < 0) {
        printf("writev failed: %s\n", strerror(-cnt));
        goto out;
    }

    if (qflag) {
        goto out;
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, t1);
    print_report("wrote", &t2, offset, qiov.size, total, cnt, Cflag);
out:
    qemu_iovec_destroy(&qiov);
    qemu_io_free(buf);
    return 0;
}

static void multiwrite_help(void)
{
    printf(
"\n"
" writes a range of bytes from the given offset source from multiple buffers,\n"
" in a batch of requests that may be merged by qemu\n"
"\n"
" Example:\n"
" 'multiwrite 512 1k 1k ; 4k 1k'\n"
"  writes 2 kB at 512 bytes and 1 kB at 4 kB into the open file\n"
"\n"
" Writes into a segment of the currently open file, using a buffer\n"
" filled with a set pattern (0xcdcdcdcd). The pattern byte is increased\n"
" by one for each request contained in the multiwrite command.\n"
" -P, -- use different pattern to fill file\n"
" -C, -- report statistics in a machine parsable format\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int multiwrite_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t multiwrite_cmd = {
    .name       = "multiwrite",
    .cfunc      = multiwrite_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cq] [-P pattern ] off len [len..] [; off len [len..]..]",
    .oneline    = "issues multiple write requests at once",
    .help       = multiwrite_help,
};

static int multiwrite_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, qflag = 0;
    int c, cnt;
    char **buf;
    int64_t offset, first_offset = 0;
    /* Some compilers get confused and warn if this is not initialized.  */
    int total = 0;
    int nr_iov;
    int nr_reqs;
    int pattern = 0xcd;
    QEMUIOVector *qiovs;
    int i;
    BlockRequest *reqs;

    while ((c = getopt(argc, argv, "CqP:")) != EOF) {
        switch (c) {
        case 'C':
            Cflag = 1;
            break;
        case 'q':
            qflag = 1;
            break;
        case 'P':
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                return 0;
            }
            break;
        default:
            return qemuio_command_usage(&writev_cmd);
        }
    }

    if (optind > argc - 2) {
        return qemuio_command_usage(&writev_cmd);
    }

    nr_reqs = 1;
    for (i = optind; i < argc; i++) {
        if (!strcmp(argv[i], ";")) {
            nr_reqs++;
        }
    }

    reqs = g_malloc0(nr_reqs * sizeof(*reqs));
    buf = g_malloc0(nr_reqs * sizeof(*buf));
    qiovs = g_malloc(nr_reqs * sizeof(*qiovs));

    for (i = 0; i < nr_reqs && optind < argc; i++) {
        int j;

        /* Read the offset of the request */
        offset = cvtnum(argv[optind]);
        if (offset < 0) {
            printf("non-numeric offset argument -- %s\n", argv[optind]);
            goto out;
        }
        optind++;

        if (offset & 0x1ff) {
            printf("offset %lld is not sector aligned\n",
                   (long long)offset);
            goto out;
        }

        if (i == 0) {
            first_offset = offset;
        }

        /* Read lengths for qiov entries */
        for (j = optind; j < argc; j++) {
            if (!strcmp(argv[j], ";")) {
                break;
            }
        }

        nr_iov = j - optind;

        /* Build request */
        buf[i] = create_iovec(bs, &qiovs[i], &argv[optind], nr_iov, pattern);
        if (buf[i] == NULL) {
            goto out;
        }

        reqs[i].qiov = &qiovs[i];
        reqs[i].sector = offset >> 9;
        reqs[i].nb_sectors = reqs[i].qiov->size >> 9;

        optind = j + 1;

        pattern++;
    }

    /* If there were empty requests at the end, ignore them */
    nr_reqs = i;

    gettimeofday(&t1, NULL);
    cnt = do_aio_multiwrite(bs, reqs, nr_reqs, &total);
    gettimeofday(&t2, NULL);

    if (cnt < 0) {
        printf("aio_multiwrite failed: %s\n", strerror(-cnt));
        goto out;
    }

    if (qflag) {
        goto out;
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, t1);
    print_report("wrote", &t2, first_offset, total, total, cnt, Cflag);
out:
    for (i = 0; i < nr_reqs; i++) {
        qemu_io_free(buf[i]);
        if (reqs[i].qiov != NULL) {
            qemu_iovec_destroy(&qiovs[i]);
        }
    }
    g_free(buf);
    g_free(reqs);
    g_free(qiovs);
    return 0;
}

struct aio_ctx {
    QEMUIOVector qiov;
    int64_t offset;
    char *buf;
    int qflag;
    int vflag;
    int Cflag;
    int Pflag;
    int pattern;
    struct timeval t1;
};

static void aio_write_done(void *opaque, int ret)
{
    struct aio_ctx *ctx = opaque;
    struct timeval t2;

    gettimeofday(&t2, NULL);


    if (ret < 0) {
        printf("aio_write failed: %s\n", strerror(-ret));
        goto out;
    }

    if (ctx->qflag) {
        goto out;
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, ctx->t1);
    print_report("wrote", &t2, ctx->offset, ctx->qiov.size,
                 ctx->qiov.size, 1, ctx->Cflag);
out:
    qemu_io_free(ctx->buf);
    qemu_iovec_destroy(&ctx->qiov);
    g_free(ctx);
}

static void aio_read_done(void *opaque, int ret)
{
    struct aio_ctx *ctx = opaque;
    struct timeval t2;

    gettimeofday(&t2, NULL);

    if (ret < 0) {
        printf("readv failed: %s\n", strerror(-ret));
        goto out;
    }

    if (ctx->Pflag) {
        void *cmp_buf = g_malloc(ctx->qiov.size);

        memset(cmp_buf, ctx->pattern, ctx->qiov.size);
        if (memcmp(ctx->buf, cmp_buf, ctx->qiov.size)) {
            printf("Pattern verification failed at offset %"
                   PRId64 ", %zd bytes\n", ctx->offset, ctx->qiov.size);
        }
        g_free(cmp_buf);
    }

    if (ctx->qflag) {
        goto out;
    }

    if (ctx->vflag) {
        dump_buffer(ctx->buf, ctx->offset, ctx->qiov.size);
    }

    /* Finally, report back -- -C gives a parsable format */
    t2 = tsub(t2, ctx->t1);
    print_report("read", &t2, ctx->offset, ctx->qiov.size,
                 ctx->qiov.size, 1, ctx->Cflag);
out:
    qemu_io_free(ctx->buf);
    qemu_iovec_destroy(&ctx->qiov);
    g_free(ctx);
}

static void aio_read_help(void)
{
    printf(
"\n"
" asynchronously reads a range of bytes from the given offset\n"
"\n"
" Example:\n"
" 'aio_read -v 512 1k 1k ' - dumps 2 kilobytes read from 512 bytes into the file\n"
"\n"
" Reads a segment of the currently open file, optionally dumping it to the\n"
" standard output stream (with -v option) for subsequent inspection.\n"
" The read is performed asynchronously and the aio_flush command must be\n"
" used to ensure all outstanding aio requests have been completed.\n"
" -C, -- report statistics in a machine parsable format\n"
" -P, -- use a pattern to verify read data\n"
" -v, -- dump buffer to standard output\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int aio_read_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t aio_read_cmd = {
    .name       = "aio_read",
    .cfunc      = aio_read_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cqv] [-P pattern ] off len [len..]",
    .oneline    = "asynchronously reads a number of bytes",
    .help       = aio_read_help,
};

static int aio_read_f(BlockDriverState *bs, int argc, char **argv)
{
    int nr_iov, c;
    struct aio_ctx *ctx = g_new0(struct aio_ctx, 1);

    while ((c = getopt(argc, argv, "CP:qv")) != EOF) {
        switch (c) {
        case 'C':
            ctx->Cflag = 1;
            break;
        case 'P':
            ctx->Pflag = 1;
            ctx->pattern = parse_pattern(optarg);
            if (ctx->pattern < 0) {
                g_free(ctx);
                return 0;
            }
            break;
        case 'q':
            ctx->qflag = 1;
            break;
        case 'v':
            ctx->vflag = 1;
            break;
        default:
            g_free(ctx);
            return qemuio_command_usage(&aio_read_cmd);
        }
    }

    if (optind > argc - 2) {
        g_free(ctx);
        return qemuio_command_usage(&aio_read_cmd);
    }

    ctx->offset = cvtnum(argv[optind]);
    if (ctx->offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        g_free(ctx);
        return 0;
    }
    optind++;

    if (ctx->offset & 0x1ff) {
        printf("offset %" PRId64 " is not sector aligned\n",
               ctx->offset);
        g_free(ctx);
        return 0;
    }

    nr_iov = argc - optind;
    ctx->buf = create_iovec(bs, &ctx->qiov, &argv[optind], nr_iov, 0xab);
    if (ctx->buf == NULL) {
        g_free(ctx);
        return 0;
    }

    gettimeofday(&ctx->t1, NULL);
    bdrv_aio_readv(bs, ctx->offset >> 9, &ctx->qiov,
                   ctx->qiov.size >> 9, aio_read_done, ctx);
    return 0;
}

static void aio_write_help(void)
{
    printf(
"\n"
" asynchronously writes a range of bytes from the given offset source\n"
" from multiple buffers\n"
"\n"
" Example:\n"
" 'aio_write 512 1k 1k' - writes 2 kilobytes at 512 bytes into the open file\n"
"\n"
" Writes into a segment of the currently open file, using a buffer\n"
" filled with a set pattern (0xcdcdcdcd).\n"
" The write is performed asynchronously and the aio_flush command must be\n"
" used to ensure all outstanding aio requests have been completed.\n"
" -P, -- use different pattern to fill file\n"
" -C, -- report statistics in a machine parsable format\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int aio_write_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t aio_write_cmd = {
    .name       = "aio_write",
    .cfunc      = aio_write_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cq] [-P pattern ] off len [len..]",
    .oneline    = "asynchronously writes a number of bytes",
    .help       = aio_write_help,
};

static int aio_write_f(BlockDriverState *bs, int argc, char **argv)
{
    int nr_iov, c;
    int pattern = 0xcd;
    struct aio_ctx *ctx = g_new0(struct aio_ctx, 1);

    while ((c = getopt(argc, argv, "CqP:")) != EOF) {
        switch (c) {
        case 'C':
            ctx->Cflag = 1;
            break;
        case 'q':
            ctx->qflag = 1;
            break;
        case 'P':
            pattern = parse_pattern(optarg);
            if (pattern < 0) {
                g_free(ctx);
                return 0;
            }
            break;
        default:
            g_free(ctx);
            return qemuio_command_usage(&aio_write_cmd);
        }
    }

    if (optind > argc - 2) {
        g_free(ctx);
        return qemuio_command_usage(&aio_write_cmd);
    }

    ctx->offset = cvtnum(argv[optind]);
    if (ctx->offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        g_free(ctx);
        return 0;
    }
    optind++;

    if (ctx->offset & 0x1ff) {
        printf("offset %" PRId64 " is not sector aligned\n",
               ctx->offset);
        g_free(ctx);
        return 0;
    }

    nr_iov = argc - optind;
    ctx->buf = create_iovec(bs, &ctx->qiov, &argv[optind], nr_iov, pattern);
    if (ctx->buf == NULL) {
        g_free(ctx);
        return 0;
    }

    gettimeofday(&ctx->t1, NULL);
    bdrv_aio_writev(bs, ctx->offset >> 9, &ctx->qiov,
                    ctx->qiov.size >> 9, aio_write_done, ctx);
    return 0;
}

static int aio_flush_f(BlockDriverState *bs, int argc, char **argv)
{
    bdrv_drain_all();
    return 0;
}

static const cmdinfo_t aio_flush_cmd = {
    .name       = "aio_flush",
    .cfunc      = aio_flush_f,
    .oneline    = "completes all outstanding aio requests"
};

static int flush_f(BlockDriverState *bs, int argc, char **argv)
{
    bdrv_flush(bs);
    return 0;
}

static const cmdinfo_t flush_cmd = {
    .name       = "flush",
    .altname    = "f",
    .cfunc      = flush_f,
    .oneline    = "flush all in-core file state to disk",
};

static int truncate_f(BlockDriverState *bs, int argc, char **argv)
{
    int64_t offset;
    int ret;

    offset = cvtnum(argv[1]);
    if (offset < 0) {
        printf("non-numeric truncate argument -- %s\n", argv[1]);
        return 0;
    }

    ret = bdrv_truncate(bs, offset);
    if (ret < 0) {
        printf("truncate: %s\n", strerror(-ret));
        return 0;
    }

    return 0;
}

static const cmdinfo_t truncate_cmd = {
    .name       = "truncate",
    .altname    = "t",
    .cfunc      = truncate_f,
    .argmin     = 1,
    .argmax     = 1,
    .args       = "off",
    .oneline    = "truncates the current file at the given offset",
};

static int length_f(BlockDriverState *bs, int argc, char **argv)
{
    int64_t size;
    char s1[64];

    size = bdrv_getlength(bs);
    if (size < 0) {
        printf("getlength: %s\n", strerror(-size));
        return 0;
    }

    cvtstr(size, s1, sizeof(s1));
    printf("%s\n", s1);
    return 0;
}


static const cmdinfo_t length_cmd = {
    .name   = "length",
    .altname    = "l",
    .cfunc      = length_f,
    .oneline    = "gets the length of the current file",
};


static int info_f(BlockDriverState *bs, int argc, char **argv)
{
    BlockDriverInfo bdi;
    ImageInfoSpecific *spec_info;
    char s1[64], s2[64];
    int ret;

    if (bs->drv && bs->drv->format_name) {
        printf("format name: %s\n", bs->drv->format_name);
    }
    if (bs->drv && bs->drv->protocol_name) {
        printf("format name: %s\n", bs->drv->protocol_name);
    }

    ret = bdrv_get_info(bs, &bdi);
    if (ret) {
        return 0;
    }

    cvtstr(bdi.cluster_size, s1, sizeof(s1));
    cvtstr(bdi.vm_state_offset, s2, sizeof(s2));

    printf("cluster size: %s\n", s1);
    printf("vm state offset: %s\n", s2);

    spec_info = bdrv_get_specific_info(bs);
    if (spec_info) {
        printf("Format specific information:\n");
        bdrv_image_info_specific_dump(fprintf, stdout, spec_info);
        qapi_free_ImageInfoSpecific(spec_info);
    }

    return 0;
}



static const cmdinfo_t info_cmd = {
    .name       = "info",
    .altname    = "i",
    .cfunc      = info_f,
    .oneline    = "prints information about the current file",
};

static void discard_help(void)
{
    printf(
"\n"
" discards a range of bytes from the given offset\n"
"\n"
" Example:\n"
" 'discard 512 1k' - discards 1 kilobyte from 512 bytes into the file\n"
"\n"
" Discards a segment of the currently open file.\n"
" -C, -- report statistics in a machine parsable format\n"
" -q, -- quiet mode, do not show I/O statistics\n"
"\n");
}

static int discard_f(BlockDriverState *bs, int argc, char **argv);

static const cmdinfo_t discard_cmd = {
    .name       = "discard",
    .altname    = "d",
    .cfunc      = discard_f,
    .argmin     = 2,
    .argmax     = -1,
    .args       = "[-Cq] off len",
    .oneline    = "discards a number of bytes at a specified offset",
    .help       = discard_help,
};

static int discard_f(BlockDriverState *bs, int argc, char **argv)
{
    struct timeval t1, t2;
    int Cflag = 0, qflag = 0;
    int c, ret;
    int64_t offset;
    int count;

    while ((c = getopt(argc, argv, "Cq")) != EOF) {
        switch (c) {
        case 'C':
            Cflag = 1;
            break;
        case 'q':
            qflag = 1;
            break;
        default:
            return qemuio_command_usage(&discard_cmd);
        }
    }

    if (optind != argc - 2) {
        return qemuio_command_usage(&discard_cmd);
    }

    offset = cvtnum(argv[optind]);
    if (offset < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    optind++;
    count = cvtnum(argv[optind]);
    if (count < 0) {
        printf("non-numeric length argument -- %s\n", argv[optind]);
        return 0;
    }

    gettimeofday(&t1, NULL);
    ret = bdrv_discard(bs, offset >> BDRV_SECTOR_BITS,
                       count >> BDRV_SECTOR_BITS);
    gettimeofday(&t2, NULL);

    if (ret < 0) {
        printf("discard failed: %s\n", strerror(-ret));
        goto out;
    }

    /* Finally, report back -- -C gives a parsable format */
    if (!qflag) {
        t2 = tsub(t2, t1);
        print_report("discard", &t2, offset, count, count, 1, Cflag);
    }

out:
    return 0;
}

static int alloc_f(BlockDriverState *bs, int argc, char **argv)
{
    int64_t offset, sector_num;
    int nb_sectors, remaining;
    char s1[64];
    int num, sum_alloc;
    int ret;

    offset = cvtnum(argv[1]);
    if (offset < 0) {
        printf("non-numeric offset argument -- %s\n", argv[1]);
        return 0;
    } else if (offset & 0x1ff) {
        printf("offset %" PRId64 " is not sector aligned\n",
               offset);
        return 0;
    }

    if (argc == 3) {
        nb_sectors = cvtnum(argv[2]);
        if (nb_sectors < 0) {
            printf("non-numeric length argument -- %s\n", argv[2]);
            return 0;
        }
    } else {
        nb_sectors = 1;
    }

    remaining = nb_sectors;
    sum_alloc = 0;
    sector_num = offset >> 9;
    while (remaining) {
        ret = bdrv_is_allocated(bs, sector_num, remaining, &num);
        if (ret < 0) {
            printf("is_allocated failed: %s\n", strerror(-ret));
            return 0;
        }
        sector_num += num;
        remaining -= num;
        if (ret) {
            sum_alloc += num;
        }
        if (num == 0) {
            nb_sectors -= remaining;
            remaining = 0;
        }
    }

    cvtstr(offset, s1, sizeof(s1));

    printf("%d/%d sectors allocated at offset %s\n",
           sum_alloc, nb_sectors, s1);
    return 0;
}

static const cmdinfo_t alloc_cmd = {
    .name       = "alloc",
    .altname    = "a",
    .argmin     = 1,
    .argmax     = 2,
    .cfunc      = alloc_f,
    .args       = "off [sectors]",
    .oneline    = "checks if a sector is present in the file",
};

static int map_f(BlockDriverState *bs, int argc, char **argv)
{
    int64_t offset;
    int64_t nb_sectors;
    char s1[64];
    int num, num_checked;
    int ret;
    const char *retstr;

    offset = 0;
    nb_sectors = bs->total_sectors;

    do {
        num_checked = MIN(nb_sectors, INT_MAX);
        ret = bdrv_is_allocated(bs, offset, num_checked, &num);
        retstr = ret ? "    allocated" : "not allocated";
        cvtstr(offset << 9ULL, s1, sizeof(s1));
        printf("[% 24" PRId64 "] % 8d/% 8d sectors %s at offset %s (%d)\n",
               offset << 9ULL, num, num_checked, retstr, s1, ret);

        offset += num;
        nb_sectors -= num;
    } while (offset < bs->total_sectors);

    return 0;
}

static const cmdinfo_t map_cmd = {
       .name           = "map",
       .argmin         = 0,
       .argmax         = 0,
       .cfunc          = map_f,
       .args           = "",
       .oneline        = "prints the allocated areas of a file",
};

static int break_f(BlockDriverState *bs, int argc, char **argv)
{
    int ret;

    ret = bdrv_debug_breakpoint(bs, argv[1], argv[2]);
    if (ret < 0) {
        printf("Could not set breakpoint: %s\n", strerror(-ret));
    }

    return 0;
}

static const cmdinfo_t break_cmd = {
       .name           = "break",
       .argmin         = 2,
       .argmax         = 2,
       .cfunc          = break_f,
       .args           = "event tag",
       .oneline        = "sets a breakpoint on event and tags the stopped "
                         "request as tag",
};

static int resume_f(BlockDriverState *bs, int argc, char **argv)
{
    int ret;

    ret = bdrv_debug_resume(bs, argv[1]);
    if (ret < 0) {
        printf("Could not resume request: %s\n", strerror(-ret));
    }

    return 0;
}

static const cmdinfo_t resume_cmd = {
       .name           = "resume",
       .argmin         = 1,
       .argmax         = 1,
       .cfunc          = resume_f,
       .args           = "tag",
       .oneline        = "resumes the request tagged as tag",
};

static int wait_break_f(BlockDriverState *bs, int argc, char **argv)
{
    while (!bdrv_debug_is_suspended(bs, argv[1])) {
        qemu_aio_wait();
    }

    return 0;
}

static const cmdinfo_t wait_break_cmd = {
       .name           = "wait_break",
       .argmin         = 1,
       .argmax         = 1,
       .cfunc          = wait_break_f,
       .args           = "tag",
       .oneline        = "waits for the suspension of a request",
};

static int abort_f(BlockDriverState *bs, int argc, char **argv)
{
    abort();
}

static const cmdinfo_t abort_cmd = {
       .name           = "abort",
       .cfunc          = abort_f,
       .flags          = CMD_NOFILE_OK,
       .oneline        = "simulate a program crash using abort(3)",
};

static void sleep_cb(void *opaque)
{
    bool *expired = opaque;
    *expired = true;
}

static int sleep_f(BlockDriverState *bs, int argc, char **argv)
{
    char *endptr;
    long ms;
    struct QEMUTimer *timer;
    bool expired = false;

    ms = strtol(argv[1], &endptr, 0);
    if (ms < 0 || *endptr != '\0') {
        printf("%s is not a valid number\n", argv[1]);
        return 0;
    }

    timer = qemu_new_timer_ns(host_clock, sleep_cb, &expired);
    qemu_mod_timer(timer, qemu_get_clock_ns(host_clock) + SCALE_MS * ms);

    while (!expired) {
        main_loop_wait(false);
    }

    qemu_free_timer(timer);

    return 0;
}

static const cmdinfo_t sleep_cmd = {
       .name           = "sleep",
       .argmin         = 1,
       .argmax         = 1,
       .cfunc          = sleep_f,
       .flags          = CMD_NOFILE_OK,
       .oneline        = "waits for the given value in milliseconds",
};

static void help_oneline(const char *cmd, const cmdinfo_t *ct)
{
    if (cmd) {
        printf("%s ", cmd);
    } else {
        printf("%s ", ct->name);
        if (ct->altname) {
            printf("(or %s) ", ct->altname);
        }
    }

    if (ct->args) {
        printf("%s ", ct->args);
    }
    printf("-- %s\n", ct->oneline);
}

static void help_onecmd(const char *cmd, const cmdinfo_t *ct)
{
    help_oneline(cmd, ct);
    if (ct->help) {
        ct->help();
    }
}

static void help_all(void)
{
    const cmdinfo_t *ct;

    for (ct = cmdtab; ct < &cmdtab[ncmds]; ct++) {
        help_oneline(ct->name, ct);
    }
    printf("\nUse 'help commandname' for extended help.\n");
}

static int help_f(BlockDriverState *bs, int argc, char **argv)
{
    const cmdinfo_t *ct;

    if (argc == 1) {
        help_all();
        return 0;
    }

    ct = find_command(argv[1]);
    if (ct == NULL) {
        printf("command %s not found\n", argv[1]);
        return 0;
    }

    help_onecmd(argv[1], ct);
    return 0;
}

static const cmdinfo_t help_cmd = {
    .name       = "help",
    .altname    = "?",
    .cfunc      = help_f,
    .argmin     = 0,
    .argmax     = 1,
    .flags      = CMD_FLAG_GLOBAL,
    .args       = "[command]",
    .oneline    = "help for one or all commands",
};

bool qemuio_command(const char *cmd)
{
    char *input;
    const cmdinfo_t *ct;
    char **v;
    int c;
    bool done = false;

    input = g_strdup(cmd);
    v = breakline(input, &c);
    if (c) {
        ct = find_command(v[0]);
        if (ct) {
            done = command(ct, c, v);
        } else {
            fprintf(stderr, "command \"%s\" not found\n", v[0]);
        }
    }
    g_free(input);
    g_free(v);

    return done;
}

static void __attribute((constructor)) init_qemuio_commands(void)
{
    /* initialize commands */
    qemuio_add_command(&help_cmd);
    qemuio_add_command(&read_cmd);
    qemuio_add_command(&readv_cmd);
    qemuio_add_command(&write_cmd);
    qemuio_add_command(&writev_cmd);
    qemuio_add_command(&multiwrite_cmd);
    qemuio_add_command(&aio_read_cmd);
    qemuio_add_command(&aio_write_cmd);
    qemuio_add_command(&aio_flush_cmd);
    qemuio_add_command(&flush_cmd);
    qemuio_add_command(&truncate_cmd);
    qemuio_add_command(&length_cmd);
    qemuio_add_command(&info_cmd);
    qemuio_add_command(&discard_cmd);
    qemuio_add_command(&alloc_cmd);
    qemuio_add_command(&map_cmd);
    qemuio_add_command(&break_cmd);
    qemuio_add_command(&resume_cmd);
    qemuio_add_command(&wait_break_cmd);
    qemuio_add_command(&abort_cmd);
    qemuio_add_command(&sleep_cmd);
}