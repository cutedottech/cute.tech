#pragma once

/* Mount the LittleFS data partition and seed the default page if empty.
   Call once at boot, before the network comes up. */
void storage_init(void);

/* Start the HTTP server: serves files from the filesystem, the /edit editor,
   and the file-management endpoints under /api. Call once an IP is acquired. */
void start_webserver(void);
