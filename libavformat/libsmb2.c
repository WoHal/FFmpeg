/*
 * Copyright (c) 2014 Lukasz Marek <lukasz.m.luki@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <inttypes.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "url.h"

typedef struct {
    const AVClass *class;
    struct smb2_context *ctx;
    struct smb2_url *url;
    struct smb2dir *dh;
    struct smb2fh *fh;
    int64_t filesize;
    int trunc;
    int timeout;
    char *workgroup;
} LIBSMB2Context;

static av_cold int libsmb2_connect(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;

    libsmb2->ctx = smb2_init_context();
    if (libsmb2->ctx == NULL) {
        int ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Cannot initialize context: %s.\n", strerror(errno));
        return ret;
    }

    libsmb2->url = smb2_parse_url(libsmb2->ctx, h->filename);
    if (libsmb2->url == NULL) {
        int ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Parse url failed: %s\n", strerror(errno));
        return ret;
    }

    smb2_set_security_mode(libsmb2->ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (libsmb2->timeout != -1)
        smb2_set_timeout(libsmb2->ctx, libsmb2->timeout);
    if (libsmb2->workgroup)
        smb2_set_workstation(libsmb2->ctx, libsmb2->workgroup);

    if (libsmb2->url->password) {
        smb2_set_password(libsmb2->ctx, libsmb2->url->password);
    }
    if (smb2_connect_share(libsmb2->ctx, libsmb2->url->server, libsmb2->url->share, libsmb2->url->user) < 0) {
        int ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Initialization failed: %s\n", strerror(errno));
        return ret;
    }
    return 0;
}

static av_cold int libsmb2_close(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    if (libsmb2->fh) {
        smb2_close(libsmb2->ctx, libsmb2->fh);
        libsmb2->fh = NULL;
    }
    if (libsmb2->dh) {
        smb2_closedir(libsmb2->ctx, libsmb2->dh);
        libsmb2->dh = NULL;
    }
    if (libsmb2->url) {
        smb2_destroy_url(libsmb2->url);
        libsmb2->url = NULL;
    }
    if (libsmb2->ctx) {
        smb2_disconnect_share(libsmb2->ctx);
        smb2_destroy_context(libsmb2->ctx);
        libsmb2->ctx = NULL;
    }
    return 0;
}

static av_cold int libsmb2_open(URLContext *h, const char *url, int flags)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int access, ret;
    struct smb2_stat_64 st;

    libsmb2->filesize = -1;

    if ((ret = libsmb2_connect(h)) < 0)
        goto fail;

    if ((flags & AVIO_FLAG_WRITE) && (flags & AVIO_FLAG_READ)) {
        access = O_CREAT | O_RDWR;
        if (libsmb2->trunc)
            access |= O_TRUNC;
    } else if (flags & AVIO_FLAG_WRITE) {
        access = O_CREAT | O_WRONLY;
        if (libsmb2->trunc)
            access |= O_TRUNC;
    } else
        access = O_RDONLY;

    /* 0666 = -rw-rw-rw- = read+write for everyone, minus umask */
    if ((libsmb2->fh = smb2_open(libsmb2->ctx, libsmb2->url->path, access)) == NULL) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "File open failed: %s\n", strerror(errno));
        goto fail;
    }

    if (smb2_stat(libsmb2->ctx, libsmb2->url->path, &st) < 0)
        av_log(h, AV_LOG_WARNING, "Cannot stat file: %s\n", strerror(errno));
    else
        libsmb2->filesize = st.smb2_size;

    return 0;
  fail:
    libsmb2_close(h);
    return ret;
}

static int64_t libsmb2_seek(URLContext *h, int64_t pos, int whence)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int64_t newpos;

    if (whence == AVSEEK_SIZE) {
        if (libsmb2->filesize == -1) {
            av_log(h, AV_LOG_ERROR, "Error during seeking: filesize is unknown.\n");
            return AVERROR(EIO);
        } else
            return libsmb2->filesize;
    }

    if ((newpos = smb2_lseek(libsmb2->ctx, libsmb2->fh, pos, whence, NULL)) < 0) {
        int err = errno;
        av_log(h, AV_LOG_ERROR, "Error during seeking: %s\n", strerror(err));
        return AVERROR(err);
    }

    return newpos;
}

static int libsmb2_read(URLContext *h, unsigned char *buf, int size)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int bytes_read;

    if ((bytes_read = smb2_read(libsmb2->ctx, libsmb2->fh, buf, size)) < 0) {
        int ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Read error: %s\n", strerror(errno));
        return ret;
    }

    return bytes_read ? bytes_read : AVERROR_EOF;
}

static int libsmb2_write(URLContext *h, const unsigned char *buf, int size)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int bytes_written;

    if ((bytes_written = smb2_write(libsmb2->ctx, libsmb2->fh, buf, size)) < 0) {
        int ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Write error: %s\n", strerror(errno));
        return ret;
    }

    return bytes_written;
}

static int libsmb2_open_dir(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret;

    if ((ret = libsmb2_connect(h)) < 0)
        goto fail;

    if ((libsmb2->dh = smb2_opendir(libsmb2->ctx, libsmb2->url->path)) == NULL) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "Error opening dir: %s\n", strerror(errno));
        goto fail;
    }

    return 0;

  fail:
    libsmb2_close(h);
    return ret;
}

static int libsmb2_read_dir(URLContext *h, AVIODirEntry **next)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    AVIODirEntry *entry;
    struct smb2dirent *dirent = NULL;
    char *url = NULL;
    int skip_entry;

    *next = entry = ff_alloc_dir_entry();
    if (!entry)
        return AVERROR(ENOMEM);

    do {
        dirent = smb2_readdir(libsmb2->ctx, libsmb2->dh);
        if (!dirent) {
            break;
        }
        switch (dirent->st.smb2_type) {
        case SMB2_TYPE_DIRECTORY:
            entry->type = AVIO_ENTRY_DIRECTORY;
            break;
        case SMB2_TYPE_FILE:
            entry->type = AVIO_ENTRY_FILE;
            break;
        case SMB2_TYPE_LINK:
        default:
            entry->type = AVIO_ENTRY_UNKNOWN;
            break;
        }
    } while (!strcmp(dirent->name, ".") ||
             !strcmp(dirent->name, ".."));

    entry->name = av_strdup(dirent->name);
    if (!entry->name) {
        av_freep(next);
        return AVERROR(ENOMEM);
    }

    if (libsmb2->url) {
        struct smb2_stat_64 st;
        if (!smb2_stat(libsmb2->ctx, libsmb2->url->path, &st)) {
            // entry->group_id = st.st_gid;
            // entry->user_id = st.st_uid;
            entry->size = st.smb2_size;
            // entry->filemode = st.st_mode & 0777;
            entry->modification_timestamp = INT64_C(1000000) * st.smb2_mtime;
            entry->access_timestamp =  INT64_C(1000000) * st.smb2_atime;
            entry->status_change_timestamp = INT64_C(1000000) * st.smb2_ctime;
        }
    }

    return 0;
}

static int libsmb2_close_dir(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    if (libsmb2->dh != NULL) {
        smb2_closedir(libsmb2->ctx, libsmb2->dh);
        libsmb2->dh = NULL;
    }
    libsmb2_close(h);
    return 0;
}

static int libsmb2_delete(URLContext *h)
{
    LIBSMB2Context *libsmb2 = h->priv_data;
    int ret;
    struct smb2_stat_64 st;

    if ((ret = libsmb2_connect(h)) < 0)
        goto cleanup;

    if ((libsmb2->fh = smb2_open(libsmb2->ctx, libsmb2->url->path, O_WRONLY)) < 0) {
        ret = AVERROR(errno);
        goto cleanup;
    }

    if (smb2_stat(libsmb2->ctx, libsmb2->url->path, &st) < 0) {
        ret = AVERROR(errno);
        goto cleanup;
    }

    smb2_close(libsmb2->ctx, libsmb2->fh);
    libsmb2->fh = NULL;

    if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
        if (smb2_rmdir(libsmb2->ctx, libsmb2->url->path) < 0) {
            ret = AVERROR(errno);
            goto cleanup;
        }
    } else {
        if (smb2_unlink(libsmb2->ctx, libsmb2->url->path) < 0) {
            ret = AVERROR(errno);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    libsmb2_close(h);
    return ret;
}

static int libsmb2_move(URLContext *h_src, URLContext *h_dst)
{
    LIBSMB2Context *libsmb2 = h_src->priv_data;
    int ret;

    if ((ret = libsmb2_connect(h_src)) < 0)
        goto cleanup;

    struct smb2_url *dst_url = smb2_parse_url(libsmb2->ctx, h_dst->filename);
    if ((ret = smb2_rename(libsmb2->ctx, libsmb2->url->path, dst_url->path)) < 0) {
        ret = AVERROR(errno);
        goto cleanup;
    }

    ret = 0;

cleanup:
    libsmb2_close(h_src);
    return ret;
}

#define OFFSET(x) offsetof(LIBSMB2Context, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"timeout",   "set timeout in ms of socket I/O operations",    OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, D|E },
    {"truncate",  "truncate existing files on write",              OFFSET(trunc),   AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, E },
    {"workgroup", "set the workgroup used for making connections", OFFSET(workgroup), AV_OPT_TYPE_STRING, { 0 }, 0, 0, D|E },
    {NULL}
};

static const AVClass libsmb2_context_class = {
    .class_name     = "libsmb2",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_libsmb2_protocol = {
    .name                = "smb",
    .url_open            = libsmb2_open,
    .url_read            = libsmb2_read,
    .url_write           = libsmb2_write,
    .url_seek            = libsmb2_seek,
    .url_close           = libsmb2_close,
    .url_delete          = libsmb2_delete,
    .url_move            = libsmb2_move,
    .url_open_dir        = libsmb2_open_dir,
    .url_read_dir        = libsmb2_read_dir,
    .url_close_dir       = libsmb2_close_dir,
    .priv_data_size      = sizeof(LIBSMB2Context),
    .priv_data_class     = &libsmb2_context_class,
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
