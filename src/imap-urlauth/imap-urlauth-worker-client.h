#ifndef IMAP_URLAUTH_WORKER_CLIENT_H
#define IMAP_URLAUTH_WORKER_CLIENT_H

#include "imap-urlauth-worker-common.h"

int client_worker_connect(struct client *wclient);
void client_worker_disconnect(struct client *wclient);

#endif
