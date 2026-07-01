#include "server.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"

static const char *TAG = "server";

/* Default page + editor app are embedded in flash (see CMakeLists EMBED_FILES) */
extern const char index_html_start[]  asm("_binary_index_html_start");
extern const char index_html_end[]    asm("_binary_index_html_end");
extern const char editor_html_start[] asm("_binary_editor_html_start");
extern const char editor_html_end[]   asm("_binary_editor_html_end");

#define MOUNT_POINT   "/fs"
#define WEB_PATH_MAX  256
#define FS_PATH_MAX   (WEB_PATH_MAX + 8)    /* room for "/fs" prefix + ".tmp" */
#define MAX_UPLOAD    (256 * 1024)          /* per-file write cap */
#define MAX_DEPTH     8                     /* directory recursion guard */

/* ── small helpers ───────────────────────────────────────────────────────── */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_size; i++) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
            if (hi >= 0 && lo >= 0) { dst[di++] = (char)((hi << 4) | lo); i += 2; continue; }
        }
        dst[di++] = (src[i] == '+') ? ' ' : src[i];
    }
    dst[di] = '\0';
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r')        { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t')        { dst[j++] = '\\'; dst[j++] = 't'; }
        else if ((unsigned char)c >= 0x20) { dst[j++] = c; }
        /* other control chars dropped */
    }
    dst[j] = '\0';
}

static const char *content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html") || !strcmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(ext, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(ext, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(ext, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcmp(ext, ".png"))  return "image/png";
    if (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcmp(ext, ".gif"))  return "image/gif";
    if (!strcmp(ext, ".ico"))  return "image/x-icon";
    if (!strcmp(ext, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* Map a web path ("/foo/bar.css") to a filesystem path ("/fs/foo/bar.css").
   Rejects anything that isn't an absolute path or that contains "..". */
static bool to_fs_path(const char *web, char *out, size_t out_size)
{
    if (!web || web[0] != '/')  return false;
    if (strstr(web, ".."))      return false;
    int n = snprintf(out, out_size, "%s%s", MOUNT_POINT, web);
    return n > 0 && (size_t)n < out_size;
}

static bool path_exists(const char *fs_path)
{
    struct stat st;
    return stat(fs_path, &st) == 0;
}

/* Create every parent directory of fs_path (the last component is left alone). */
static void mkdir_parents(const char *fs_path)
{
    char tmp[FS_PATH_MAX];
    strlcpy(tmp, fs_path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);   /* ignore EEXIST */
            *p = '/';
        }
    }
}

static void remove_recursive(const char *fs_path, int depth)
{
    if (depth > MAX_DEPTH) return;
    struct stat st;
    if (stat(fs_path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(fs_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[FS_PATH_MAX];
                if (snprintf(child, sizeof(child), "%s/%s", fs_path, e->d_name) >= (int)sizeof(child))
                    continue;
                remove_recursive(child, depth + 1);
            }
            closedir(d);
        }
        rmdir(fs_path);
    } else {
        unlink(fs_path);
    }
}

/* Read the ?path= query parameter (URL-decoded) into out. */
static bool get_path_param(httpd_req_t *req, char *out, size_t out_size)
{
    char query[WEB_PATH_MAX * 2];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char enc[WEB_PATH_MAX];
    if (httpd_query_key_value(query, "path", enc, sizeof(enc)) != ESP_OK) return false;
    url_decode(out, enc, out_size);
    return out[0] == '/' && !strstr(out, "..");
}

static esp_err_t serve_file(httpd_req_t *req, const char *fs_path, const char *ctype)
{
    FILE *f = fopen(fs_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, ctype);
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ── handlers ────────────────────────────────────────────────────────────── */

/* Catch-all: serve any file from the filesystem; "/" falls back to the
   embedded default page if no index.html has been created yet. */
static esp_err_t static_get_handler(httpd_req_t *req)
{
    char web[WEB_PATH_MAX];
    const char *q = strchr(req->uri, '?');
    size_t len = q ? (size_t)(q - req->uri) : strlen(req->uri);
    if (len >= sizeof(web)) len = sizeof(web) - 1;
    memcpy(web, req->uri, len);
    web[len] = '\0';

    if (strcmp(web, "/") == 0) {
        if (path_exists(MOUNT_POINT "/index.html"))
            return serve_file(req, MOUNT_POINT "/index.html", "text/html; charset=utf-8");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
    }

    char fs_path[FS_PATH_MAX];
    struct stat st;
    if (!to_fs_path(web, fs_path, sizeof(fs_path)) ||
        stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    return serve_file(req, fs_path, content_type(web));
}

static esp_err_t editor_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, editor_html_start, editor_html_end - editor_html_start);
}

/* Recursively stream the file tree as a flat JSON array.
   esc/line are static: each frame fully emits its entry before recursing and
   never reads them again, and the httpd task runs handlers serially. */
static void list_into(httpd_req_t *req, const char *fs_dir, const char *web_dir,
                      bool *first, int depth)
{
    if (depth > MAX_DEPTH) return;
    DIR *d = opendir(fs_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char fs_child[FS_PATH_MAX], web_child[WEB_PATH_MAX];
        int fn = snprintf(fs_child,  sizeof(fs_child),  "%s/%s", fs_dir,  e->d_name);
        int wn = snprintf(web_child, sizeof(web_child), "%s/%s", web_dir, e->d_name);
        if (fn < 0 || fn >= (int)sizeof(fs_child) || wn < 0 || wn >= (int)sizeof(web_child))
            continue;

        struct stat st;
        if (stat(fs_child, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);

        static char esc[WEB_PATH_MAX * 2];
        static char line[WEB_PATH_MAX * 2 + 96];
        json_escape(web_child, esc, sizeof(esc));
        int n = snprintf(line, sizeof(line),
                         "%s{\"path\":\"%s\",\"type\":\"%s\",\"size\":%ld}",
                         *first ? "" : ",", esc, is_dir ? "dir" : "file",
                         (long)st.st_size);
        httpd_resp_send_chunk(req, line, n);
        *first = false;

        if (is_dir) list_into(req, fs_child, web_child, first, depth + 1);
    }
    closedir(d);
}

static esp_err_t api_files_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    list_into(req, MOUNT_POINT, "", &first, 0);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_file_get_handler(httpd_req_t *req)
{
    char web[WEB_PATH_MAX], fs_path[FS_PATH_MAX];
    if (!get_path_param(req, web, sizeof(web)) || !to_fs_path(web, fs_path, sizeof(fs_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_FAIL;
    }
    return serve_file(req, fs_path, "text/plain; charset=utf-8");
}

static esp_err_t api_file_put_handler(httpd_req_t *req)
{
    char web[WEB_PATH_MAX], fs_path[FS_PATH_MAX];
    if (!get_path_param(req, web, sizeof(web)) || !to_fs_path(web, fs_path, sizeof(fs_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_FAIL;
    }
    if (req->content_len > MAX_UPLOAD) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large");
        return ESP_FAIL;
    }

    size_t total = req->content_len;
    char *body = malloc(total ? total : 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); return ESP_FAIL; }
        received += r;
    }

    /* Atomic write: stage to .tmp, then rename over the target. */
    mkdir_parents(fs_path);
    char tmp[FS_PATH_MAX + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", fs_path) >= (int)sizeof(tmp)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
        return ESP_FAIL;
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
        return ESP_FAIL;
    }
    size_t written = received ? fwrite(body, 1, received, f) : 0;
    fclose(f);
    free(body);
    if (written != received) {
        unlink(tmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_FAIL;
    }
    unlink(fs_path);                 /* rename won't overwrite on LittleFS */
    if (rename(tmp, fs_path) != 0) {
        unlink(tmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t api_file_delete_handler(httpd_req_t *req)
{
    char web[WEB_PATH_MAX], fs_path[FS_PATH_MAX];
    if (!get_path_param(req, web, sizeof(web)) || strcmp(web, "/") == 0 ||
        !to_fs_path(web, fs_path, sizeof(fs_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_FAIL;
    }
    remove_recursive(fs_path, 0);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t api_mkdir_handler(httpd_req_t *req)
{
    char web[WEB_PATH_MAX], fs_path[FS_PATH_MAX];
    if (!get_path_param(req, web, sizeof(web)) || !to_fs_path(web, fs_path, sizeof(fs_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_FAIL;
    }
    mkdir_parents(fs_path);
    mkdir(fs_path, 0755);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = "storage",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);

    /* Seed the editable index.html from the embedded default on first boot. */
    if (!path_exists(MOUNT_POINT "/index.html")) {
        FILE *f = fopen(MOUNT_POINT "/index.html", "w");
        if (f) {
            fwrite(index_html_start, 1, index_html_end - index_html_start, f);
            fclose(f);
            ESP_LOGI(TAG, "seeded default index.html");
        }
    }
}

static void register_uri(httpd_handle_t server, const char *uri,
                         httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = method, .handler = handler };
    httpd_register_uri_handler(server, &u);
}

void start_webserver(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    /* Specific routes first; the wildcard catch-all must be registered last. */
    register_uri(server, "/api/files", HTTP_GET,    api_files_handler);
    register_uri(server, "/api/file",  HTTP_GET,    api_file_get_handler);
    register_uri(server, "/api/file",  HTTP_PUT,    api_file_put_handler);
    register_uri(server, "/api/file",  HTTP_DELETE, api_file_delete_handler);
    register_uri(server, "/api/mkdir", HTTP_POST,   api_mkdir_handler);
    register_uri(server, "/edit",      HTTP_GET,    editor_get_handler);
    register_uri(server, "/*",         HTTP_GET,    static_get_handler);

    ESP_LOGI(TAG, "HTTP server started — open /edit to customise your site");
}
