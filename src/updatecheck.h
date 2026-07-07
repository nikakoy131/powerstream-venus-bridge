#pragma once
#include <stddef.h>
#include "esp_err.h"

/* Query the GitHub releases API for the repo in settings (github_url) and
   compare the latest release tag with the running firmware version. Writes a
   JSON result to out, e.g.
     {"ok":true,"current":"v0.9.0","latest":"v0.9.1",
      "update_available":true,"url":"https://github.com/owner/repo/releases/latest"}
   or {"ok":false,"err":"..."} on failure. Blocks (does a TLS request), so call
   it from a task with a generous stack. Returns ESP_OK when ok:true. */
esp_err_t updatecheck_run(char *out, size_t outlen);
