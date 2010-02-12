/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Daemon
 *  ------------------
 *  Copyright (C) 2001-2009, Eduardo Silva P.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * Some history about this module
 * ------------------------------
 * 2008 - Rewrite module, suport dynamic themes by Eduardo
 * 2008 - Felipe Astroza (max) provided the mk_dirhtml_human_readable_size_func()
 * 2007 - Add struct client_request support by Eduardo
 * 2002 - Original version written by Daniel R. Ome
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "monkey.h"
#include "http.h"
#include "http_status.h"
#include "str.h"
#include "memory.h"
#include "utils.h"
#include "config.h"
#include "method.h"
#include "socket.h"
#include "header.h"
#include "file.h"
#include "iov.h"

#include "plugin.h"
#include "dirlisting.h"

/* Plugin data for register */
mk_plugin_data_t _shortname = "dirlisting";
mk_plugin_data_t _name = "Directory Listing";
mk_plugin_data_t _version = "1.0";
mk_plugin_stage_t _stages = MK_PLUGIN_STAGE_40;;

struct plugin_api *mk_api;

char *_tags_global[] = { "%_html_title_%",
    "%_theme_path_%",
    NULL
};

char *_tags_entry[] = { "%_target_title_%",
    "%_target_url_%",
    "%_target_name_%",
    "%_target_time_%",
    "%_target_size_%",
    NULL
};

/* DIR_HTML logic:
 * ---------------
 * [Monkey Start]
 * |
 * *--> mk_dirhtml_conf()
 *      |
 *      *--> mk_dirhtml_read_config()
 *      *--> mk_dirhtml_theme_load()
 *           |
 *           *--> mk_dirhtml_load_file() (FILE_HEADER, FILE_ENTRY, FILE_FOOTER)
 *           *--> mk_dirhtml_template_create()
 *                |
 *                *-->
 *
 *
 *
 */

/* Function wrote by Max (Felipe Astroza), thanks! */
char *mk_dirhtml_human_readable_size(off_t size)
{
    unsigned long u = 1024, i, len;
    char *buf;
    static const char *__units[] = { "b", "K", "M", "G",
        "T", "P", "E", "Z", "Y", NULL
    };

    for (i = 0; __units[i] != NULL; i++) {
        if ((size / u) == 0) {
            break;
        }
        u *= 1024;
    }
    if (!i) {
        mk_api->str_build(&buf, &len, "%u%s", size, __units[0]);
    }
    else {
        float fsize = (float) ((double) size / (u / 1024));
        mk_api->str_build(&buf, &len, "%.1f%s", fsize, __units[i]);
    }

    return buf;
}

struct mk_f_list *mk_dirhtml_create_element(char *file,
                                            unsigned char type,
                                            char *full_path,
                                            unsigned long *list_len)
{
    off_t size;
    int n;
    struct tm *st_time;
    struct mk_f_list *entry;
    struct file_info *entry_info;

    entry_info = mk_api->file_get_info(full_path);

    entry = mk_api->mem_alloc_z(sizeof(struct mk_f_list));
    entry->name = file;
    entry->type = type;
    entry->info = entry_info;
    entry->next = NULL;

    st_time = localtime((time_t *) & entry_info->last_modification);

    entry->ft_modif = mk_api->mem_alloc_z(50);
    n = strftime(entry->ft_modif, 50, "%d-%b-%G %H:%M", st_time);

    size = entry->info->size;

    if (type != DT_DIR) {
        entry->size = mk_dirhtml_human_readable_size(size);
    }
    else {
        entry->size = MK_DIRHTML_SIZE_DIR;
    }

    *list_len = *list_len + 1;

    return entry;
}

struct mk_f_list *mk_dirhtml_create_list(DIR * dir, char *path,
                                         unsigned long *list_len)
{
    unsigned long len;
    char *full_path = 0;
    struct dirent *ent;
    struct mk_f_list *list = 0, *entry = 0, *last = 0;

    /* Before to send the information, we need to build
     * the list of entries, this really sucks because the user
     * always will want to have the information sorted, why we don't 
     * add some spec to the HTTP protocol in order to send the information
     * in a generic way and let the client choose how to show it
     * as they does browsing a FTP server ???, we can save bandweight,
     * let the cool firefox developers create different templates and
     * we are going to have a more happy end users.
     *
     * that kind of ideas comes when you are in an airport just waiting :)
     */

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* Look just for files and dirs */
        if (ent->d_type != DT_REG && ent->d_type != DT_DIR
            && ent->d_type != DT_LNK) {
            continue;
        }

        if (!ent->d_name) {
            puts("mk_dirhtml :: buffer error");
        }


        mk_api->str_build(&full_path, &len, "%s%s", path, ent->d_name);

        entry = mk_dirhtml_create_element(ent->d_name,
                                          ent->d_type, full_path, list_len);

        mk_api->mem_free(full_path);

        if (!entry) {
            continue;
        }

        if (!list) {
            list = entry;
        }
        else {
            last->next = entry;
        }
        last = entry;
    }

    return list;
}

/* Read dirhtml config and themes */
int mk_dirhtml_conf(char *confdir)
{
    int ret = 0;
    unsigned long len;
    char *conf_file;

    mk_api->str_build(&conf_file, &len, "%s", confdir);
    ret = mk_dirhtml_read_config(conf_file);

    /* This function will load the default theme 
     * setted in dirhtml_conf struct 
     */
    ret = mk_dirhtml_theme_load();
    mk_api->pointer_set(&mk_dirhtml_default_mime, MK_DIRHTML_DEFAULT_MIME);

    return ret;
}

/* 
 * Read the main configuration file for dirhtml: dirhtml.conf, 
 * it will alloc the dirhtml_conf struct
*/
int mk_dirhtml_read_config(char *path)
{
    unsigned long len;
    char *default_file;
    struct mk_config *p;

    mk_api->str_build(&default_file, &len, "%sdirhtml.conf", path);
    p = conf = mk_api->config_create(default_file);

    /* alloc dirhtml config struct */
    dirhtml_conf = mk_api->mem_alloc(sizeof(struct dirhtml_config));

    while (p) {
        if (strcasecmp(p->key, "Theme") == 0) {
            dirhtml_conf->theme = mk_api->str_dup(p->val);
            mk_api->str_build(&dirhtml_conf->theme_path, &len,
                              "%sthemes/%s/", path, dirhtml_conf->theme);
        }
        p = p->next;
    }

    mk_api->mem_free(default_file);
    return 0;
}

int mk_dirhtml_theme_load()
{
    /* Data */
    char *header, *entry, *footer;

    /* Load theme files */
    header = mk_dirhtml_load_file(MK_DIRHTML_FILE_HEADER);
    entry = mk_dirhtml_load_file(MK_DIRHTML_FILE_ENTRY);
    footer = mk_dirhtml_load_file(MK_DIRHTML_FILE_FOOTER);

    if (!header || !entry || !footer) {
        mk_api->mem_free(header);
        mk_api->mem_free(entry);
        mk_api->mem_free(footer);
        return -1;
    }

    /* Parse themes */
    mk_dirhtml_tpl_header = mk_dirhtml_template_create(header);
    mk_dirhtml_tpl_entry = mk_dirhtml_template_create(entry);
    mk_dirhtml_tpl_footer = mk_dirhtml_template_create(footer);

#ifdef DEBUG_THEME
    /* Debug data */
    mk_dirhtml_theme_debug(&mk_dirhtml_tpl_header);
    mk_dirhtml_theme_debug(&mk_dirhtml_tpl_entry);
    mk_dirhtml_theme_debug(&mk_dirhtml_tpl_footer);

#endif
    mk_api->mem_free(header);
    mk_api->mem_free(entry);
    mk_api->mem_free(footer);

    return 0;
}

#ifdef DEBUG_THEME
int mk_dirhtml_theme_debug(struct dirhtml_template **st_tpl)
{
    int i = 0;
    struct dirhtml_template *aux;

    aux = *st_tpl;

    printf("\n** DEBUG_THEME **");
    fflush(stdout);

    while (aux) {
        printf("\n%i) len=%i, tag_id=%i", i, aux->len, aux->tag_id);
        if (aux->tag_id >= 0) {
            printf(" (%s) ", aux->tags[aux->tag_id]);
        }
        fflush(stdout);
        aux = aux->next;
        i++;
    }
    return 0;
}
#endif

/* Search which tag exists first in content :
 * ex: %_html_title_%
 */
int mk_dirhtml_theme_match_tag(char *content, char *tpl[])
{
    int i, len, match;

    for (i = 0; tpl[i]; i++) {
        len = strlen(tpl[i]);
        match = (int) mk_api->str_search_n(content, tpl[i], len);
        if (match >= 0) {
            return i;
        }
    }

    return -1;
}

/* return the number of valid tags found in text string */
int mk_dirhtml_content_count_tags(char *content, char *tpl[])
{
    int pos = 0, count = 0;
    int len, tpl_idx;
    int loop = 0;

    len = strlen(content);
    while (loop < len) {
        pos =
            (int) mk_api->str_search_n(content + loop, MK_DIRHTML_TAG_INIT,
                                       -1);
        if (pos >= 0) {
            tpl_idx = mk_dirhtml_theme_match_tag(content + loop, tpl);
            if (tpl_idx >= 0) {
                count++;
            }
            loop += pos;
        }
        else {
            break;
        }
        loop++;

    }
    return count;
}

struct dirhtml_template *mk_dirhtml_template_create(char *content)
{
    int i = 0, cont_len;
    int pos, last = 0;          /* 0=search init, 1=search end */
    int n_tags = 0, tpl_idx = 0;

    char *_buf;
    int _len;

    /* Global keys */
    char **_tpl = 0;

    /* Template to return */
    struct dirhtml_template *st_tpl = 0;

    cont_len = strlen(content);
    if (cont_len <= 0) {
        return NULL;
    }

    /* Parsing content */
    while (i < cont_len) {
        pos = (int) mk_api->str_search_n(content + i,
                                         MK_DIRHTML_TAG_INIT, -1);

        if (pos < 0) {
            break;
        }

        /* Checking global tag, if it's not found, proceed with 
         * 'entry tags'
         */
        _tpl = (char **) _tags_global;
        tpl_idx = mk_dirhtml_theme_match_tag(content + i + pos, _tpl);

        /* if global template do not match, use the entry tags */
        if (tpl_idx < 0) {
            _tpl = (char **) _tags_entry;
            tpl_idx = mk_dirhtml_theme_match_tag(content + i + pos, _tpl);
        }

        /* if tag found is known, we add them to our list */
        if (tpl_idx >= 0) {

            _buf = mk_api->str_copy_substr(content, i, i + pos);
            _len = strlen(_buf);

            /* Dummy if/else to create or pass a created st_tpl */
            if (!st_tpl) {
                st_tpl = mk_dirhtml_template_list_add(NULL,
                                                      _buf, _len, _tpl, -1);
            }
            else {
                mk_dirhtml_template_list_add(&st_tpl, _buf, _len, _tpl, -1);
            }
            i += (pos + strlen(_tpl[tpl_idx]));

            /* This means that a value need to be replaced */
            mk_dirhtml_template_list_add(&st_tpl, NULL, -1, _tpl, tpl_idx);
            n_tags++;
        }
        else {
            i++;
        }
    }

    if (last < cont_len) {
        _buf = mk_api->str_copy_substr(content, i, cont_len);
        _len = strlen(_buf);

        if (n_tags <= 0) {
            st_tpl = mk_dirhtml_template_list_add(NULL, _buf, _len, _tpl, -1);
        }
        else {
            mk_dirhtml_template_list_add(&st_tpl, _buf, _len, _tpl, -1);
        }
    }

    return st_tpl;
}

struct dirhtml_template
    *mk_dirhtml_template_list_add(struct dirhtml_template **header,
                                  char *buf, int len, char **tpl, int tag_id)
{
    struct dirhtml_template *node, *aux;

    node = mk_api->mem_alloc_z(sizeof(struct dirhtml_template));
    if (!node) {
        return NULL;
    }


    node->buf = buf;
    node->len = len;
    node->tag_id = tag_id;
    node->tags = tpl;
    node->next = NULL;

    /*
       if(tag_id >= 0){
       printf("\n  -> %s", node->tags[node->tag_id]);
       }
     */
    if (!header) {
        return (struct dirhtml_template *) node;
    }

    aux = *header;
    while ((*aux).next != NULL) {
        aux = (*aux).next;
    }

    (*aux).next = node;
    return (struct dirhtml_template *) node;
}

int mk_dirhtml_tag_get_id(char *tpl_tags[], char *tag)
{
    int i;
    for (i = 0; tpl_tags[i]; i++) {
        if (strcmp(tpl_tags[i], tag) == 0) {
            return i;
        }
    }

    return -1;
}

int mk_dirhtml_template_len(struct dirhtml_template *tpl)
{
    int len = 0;
    struct dirhtml_template *aux;

    aux = tpl;
    while (aux) {
        len++;
        aux = aux->next;
    }

    return len;
}

struct mk_iov *mk_dirhtml_theme_compose(struct dirhtml_template *template,
                                        struct dirhtml_value *values)
{
    /*
     * template = struct { char buf ; int len, int tag }
     * values = struct {int tag, char *value, struct *next}
     */

    struct mk_iov *iov;
    struct dirhtml_template *tpl = template;
    struct dirhtml_value *val = values;

    int tpl_len;

    tpl_len = mk_dirhtml_template_len(template);

    /* we duplicate the lenght in case we get separators */
    iov = (struct mk_iov *) mk_api->iov_create(tpl_len * 2, 1);
    tpl = template;

    while (tpl) {
        /* check for dynamic value */
        if (!tpl->buf && tpl->tag_id >= 0) {
            val = values;
            while (val) {
                if (val->tags == tpl->tags && val->tag_id == tpl->tag_id) {
                    mk_api->iov_add_entry(iov,
                                          val->value,
                                          val->len,
                                          val->sep, MK_IOV_NOT_FREE_BUF);
                    break;
                }
                else {
                    val = val->next;
                }

            }
        }
        /* static */
        else {
            mk_api->iov_add_entry(iov, tpl->buf,
                                  tpl->len, mk_iov_none, MK_IOV_NOT_FREE_BUF);
        }
        tpl = tpl->next;
    }
    return (struct mk_iov *) iov;
}

struct dirhtml_value *mk_dirhtml_tag_assign(struct dirhtml_value **values,
                                            int tag_id, mk_pointer sep,
                                            char *value, char **tags)
{
    struct dirhtml_value *check, *aux = 0;

    aux = mk_api->mem_alloc(sizeof(struct dirhtml_value));
    if (!aux) {
        return NULL;
    }

    aux->tag_id = tag_id;
    aux->value = value;
    aux->sep = sep;
    aux->tags = tags;

    if (value) {
        aux->len = strlen(value);
    }
    else {
        aux->len = -1;
    }

    aux->next = NULL;

    if (!values) {
        return (struct dirhtml_value *) aux;
    }

    check = *values;
    while ((*check).next) {
        check = (*check).next;
    }

    (*check).next = aux;


    return (struct dirhtml_value *) aux;
}

void mk_dirhtml_tag_free_list(struct dirhtml_value **list)
{
    struct dirhtml_value *prev, *target;

    target = *list;
    while (target) {
        while ((*target).next) {
            prev = target;
            target = (*target).next;
        }
        mk_api->mem_free(target);

        if (target == *list) {
            break;
        }
        (*prev).next = NULL;
        target = *list;
    }
    *list = NULL;
}

char *mk_dirhtml_load_file(char *filename)
{
    char *tmp = 0, *data = 0;
    unsigned long len;

    mk_api->str_build(&tmp, &len, "%s%s", dirhtml_conf->theme_path, filename);

    if (!tmp) {
        return NULL;
    }

    data = mk_api->file_to_buffer(tmp);
    mk_api->mem_free(tmp);

    if (!data) {
        return NULL;
    }

    return (char *) data;
}

int mk_dirhtml_entry_cmp(const void *a, const void *b)
{
    struct mk_f_list *const *f_a = a;
    struct mk_f_list *const *f_b = b;

    return strcmp((*f_a)->name, (*f_b)->name);
}

int mk_dirhtml_send(int fd, struct request *sr, struct mk_iov *data)
{
    int n;
    unsigned long len;
    char *buf = 0;

    if (sr->protocol >= HTTP_PROTOCOL_11) {
        /* Chunk header */
        mk_api->str_build(&buf, &len, "%x%s", data->total_len - 1, MK_CRLF);

        /* Add chunked information */
        mk_api->iov_set_entry(data, buf, len, MK_IOV_FREE_BUF, 0);
    }
    n = (int) mk_api->iov_send(fd, data, MK_IOV_SEND_TO_SOCKET);
    return n;
}

int mk_dirhtml_send_chunked_end(int fd)
{
    char *_end = "0\r\n\r\n";
    int len = 5;

    return write(fd, _end, len);
}

void mk_dirhtml_free_list(struct mk_f_list **toc, unsigned long len)
{
    int i;
    struct mk_f_list *entry;

    for (i = 0; i < len; i++) {
        entry = toc[i];

        if (entry->type != DT_DIR) {
            mk_api->mem_free(entry->size);
        }
        mk_api->mem_free(entry->ft_modif);
        mk_api->mem_free(entry->info);
        mk_api->mem_free(entry);
    }

    mk_api->mem_free(toc);
}

int mk_dirhtml_init(struct client_request *cr, struct request *sr)
{
    DIR *dir;
    int i = 0, n;
    mk_pointer sep;

    /* file info */
    unsigned long list_len = 0;

    struct mk_f_list *file_list, *entry, **toc;
    struct mk_iov *iov_header, *iov_footer, *iov_entry;
    struct dirhtml_value *values_global = 0;
    struct dirhtml_value *values_entry = 0;

    if (!(dir = opendir(sr->real_path.data))) {
        return -1;
    }

    file_list = mk_dirhtml_create_list(dir, sr->real_path.data, &list_len);

    /* Building headers */
    sr->headers->status = M_HTTP_OK;
    sr->headers->cgi = SH_CGI;
    sr->headers->breakline = MK_HEADER_BREAKLINE;
    sr->headers->content_type = mk_dirhtml_default_mime;

    if (sr->protocol >= HTTP_PROTOCOL_11) {
        sr->headers->transfer_encoding = MK_HEADER_TE_TYPE_CHUNKED;
    }

    /* Sending headers */
    n = (int) mk_api->header_send(cr->socket, cr, sr, sr->log);

    /* Creating response template */
    /* Set %_html_title_% */
    values_global = mk_dirhtml_tag_assign(NULL, 0, mk_iov_none,
                                          sr->uri_processed,
                                          (char **) _tags_global);

    /* Set %_theme_path_% */
    mk_dirhtml_tag_assign(&values_global, 1, mk_iov_none,
                          dirhtml_conf->theme_path, (char **) _tags_global);

    /* HTML Header */
    iov_header = mk_dirhtml_theme_compose(mk_dirhtml_tpl_header,
                                          values_global);

    /* HTML Footer */
    iov_footer = mk_dirhtml_theme_compose(mk_dirhtml_tpl_footer,
                                          values_global);

    /* Creating table of contents and sorting */
    toc = mk_api->mem_alloc(sizeof(struct mk_f_list) * list_len);
    entry = file_list;
    i = 0;
    while (entry) {
        toc[i] = entry;
        i++;
        entry = entry->next;
    }
    qsort(toc, list_len, sizeof(*toc), mk_dirhtml_entry_cmp);

    n = mk_dirhtml_send(cr->socket, sr, iov_header);

    if (n < 0) {
        closedir(dir);

        mk_dirhtml_tag_free_list(&values_global);
        mk_api->iov_free(iov_header);
        mk_api->iov_free(iov_footer);
        mk_dirhtml_free_list(toc, list_len);
        return 0;
    }

    /* sending TOC */
    for (i = 0; i < list_len; i++) {
        /* %_target_title_% */
        if (toc[i]->type == DT_DIR) {
            sep = mk_iov_slash;
        }
        else {
            sep = mk_iov_none;
        }

        /* target title */
        values_entry = mk_dirhtml_tag_assign(NULL, 0, sep,
                                             toc[i]->name,
                                             (char **) _tags_entry);

        /* target url */
        mk_dirhtml_tag_assign(&values_entry, 1, sep,
                              toc[i]->name, (char **) _tags_entry);

        /* target name */
        mk_dirhtml_tag_assign(&values_entry, 2, sep,
                              toc[i]->name, (char **) _tags_entry);

        /* target modification time */
        mk_dirhtml_tag_assign(&values_entry, 3, mk_iov_none,
                              toc[i]->ft_modif, (char **) _tags_entry);

        /* target size */
        mk_dirhtml_tag_assign(&values_entry, 4, mk_iov_none,
                              toc[i]->size, (char **) _tags_entry);

        iov_entry = mk_dirhtml_theme_compose(mk_dirhtml_tpl_entry,
                                             values_entry);

        /* send entry */
        n = mk_dirhtml_send(cr->socket, sr, iov_entry);

        if ((i % 20) == 0 && i > 0) {
            mk_api->socket_cork_flag(cr->socket, TCP_CORK_OFF);
            mk_api->socket_cork_flag(cr->socket, TCP_CORK_ON);
        }

        if (n < 0) {
            break;
        }

        /* free entry list */
        mk_dirhtml_tag_free_list(&values_entry);
        mk_api->iov_free(iov_entry);
    }

    n = mk_dirhtml_send(cr->socket, sr, iov_footer);
    mk_api->socket_cork_flag(cr->socket, TCP_CORK_OFF);

    if (sr->protocol >= HTTP_PROTOCOL_11 && n >= 0) {
        mk_dirhtml_send_chunked_end(cr->socket);
    }

    closedir(dir);

    mk_dirhtml_tag_free_list(&values_global);
    mk_api->iov_free(iov_header);
    mk_api->iov_free(iov_footer);
    mk_dirhtml_free_list(toc, list_len);

    return 0;
}

int _mk_plugin_init(void **api, char *confdir)
{
    mk_api = *api;
    mk_dirhtml_conf(confdir);
    return 0;
}

int _mk_plugin_stage_40(struct client_request *cr, struct request *sr)
{
    /* Validate file/directory */
    if (!sr->file_info) {
        return -1;
    }

    /* This plugin just handle directories */
    if (sr->file_info->is_directory == MK_FILE_FALSE) {
        return -1;
    }

    /* check setup */
    if (sr->host_conf->getdir == VAR_OFF) {
        mk_request_error(M_CLIENT_FORBIDDEN, cr, sr, 1, sr->log);
        return -1;
    }

    mk_dirhtml_init(cr, sr);
    return 0;
}
