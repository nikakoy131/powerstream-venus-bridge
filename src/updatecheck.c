#include "updatecheck.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"

#define TAG "updchk"

/* Parse "vMAJOR.MINOR.PATCH" (leading 'v' and any trailing suffix ignored). */
static bool parse_semver(const char *s, int out[3])
{
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

static int semver_cmp(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* Extract owner/repo from a URL like https://github.com/owner/repo(.git)(/...) */
static bool parse_repo(const char *url, char *owner, size_t olen,
                       char *repo, size_t rlen)
{
    const char *p = strstr(url, "github.com/");
    if (!p) return false;
    p += strlen("github.com/");
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return false;
    size_t on = (size_t)(slash - p);
    if (on == 0 || on >= olen) return false;
    memcpy(owner, p, on);
    owner[on] = '\0';

    const char *r = slash + 1;
    size_t rn = 0;
    while (r[rn] && r[rn] != '/' && r[rn] != '?' && r[rn] != '#') rn++;
    if (rn >= 4 && strncmp(r + rn - 4, ".git", 4) == 0) rn -= 4;   /* strip .git */
    if (rn == 0 || rn >= rlen) return false;
    memcpy(repo, r, rn);
    repo[rn] = '\0';
    return true;
}

esp_err_t updatecheck_run(char *out, size_t outlen)
{
    settings_t cfg = settings_get();
    char owner[64], repo[80];
    if (!parse_repo(cfg.github_url, owner, sizeof(owner), repo, sizeof(repo))) {
        snprintf(out, outlen, "{\"ok\":false,\"err\":\"bad GitHub URL\"}");
        return ESP_FAIL;
    }

    char api[224];
    snprintf(api, sizeof(api),
             "https://api.github.com/repos/%s/%s/releases/latest", owner, repo);

    esp_http_client_config_t hc = {
        .url               = api,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 8000,
        .buffer_size       = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        snprintf(out, outlen, "{\"ok\":false,\"err\":\"client init\"}");
        return ESP_FAIL;
    }
    /* GitHub rejects requests without a User-Agent. */
    esp_http_client_set_header(c, "User-Agent", "powerstream-venus-bridge");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        snprintf(out, outlen, "{\"ok\":false,\"err\":\"connect failed\"}");
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    /* Accumulate up to ~4 KB; tag_name sits near the top of the JSON. */
    static char body[4096];
    int n = 0, r;
    while (n < (int)sizeof(body) - 1 &&
           (r = esp_http_client_read(c, body + n, (int)sizeof(body) - 1 - n)) > 0)
        n += r;
    body[n] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (status != 200) {
        snprintf(out, outlen, "{\"ok\":false,\"err\":\"GitHub HTTP %d\"}", status);
        return ESP_FAIL;
    }

    /* Pull the "tag_name":"..." value out of the JSON by hand (no cJSON on 6.x). */
    char latest[48] = {0};
    const char *t = strstr(body, "\"tag_name\"");
    if (t && (t = strchr(t, ':')) && (t = strchr(t, '"'))) {
        t++;
        int i = 0;
        while (*t && *t != '"' && i < (int)sizeof(latest) - 1) latest[i++] = *t++;
        latest[i] = '\0';
    }
    if (!latest[0]) {
        snprintf(out, outlen, "{\"ok\":false,\"err\":\"no release found\"}");
        return ESP_FAIL;
    }

    const char *cur = esp_app_get_description()->version;
    int cv[3], lv[3];
    bool avail;
    if (parse_semver(cur, cv) && parse_semver(latest, lv))
        avail = semver_cmp(lv, cv) > 0;
    else
        avail = strcmp(latest, cur) != 0;

    ESP_LOGI(TAG, "current=%s latest=%s -> %s", cur, latest,
             avail ? "update available" : "up to date");

    snprintf(out, outlen,
             "{\"ok\":true,\"current\":\"%s\",\"latest\":\"%s\","
             "\"update_available\":%s,"
             "\"url\":\"https://github.com/%s/%s/releases/latest\"}",
             cur, latest, avail ? "true" : "false", owner, repo);
    return ESP_OK;
}
