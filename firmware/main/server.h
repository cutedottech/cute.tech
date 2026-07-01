#pragma once

/* Mount the LittleFS data partition and seed the default page if empty.
   Call once at boot, before the network comes up. */
void storage_init(void);

/* Start the HTTP server: serves files from the filesystem, the /edit editor,
   and the file-management endpoints under /api. Call once an IP is acquired.
   edit_password guards the endpoints that change files (?key= query param);
   pass "" to leave editing open. */
void start_webserver(const char *edit_password);
