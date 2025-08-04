#pragma once
#include "client/client.h"

typedef struct demoContext_s {
	clientActive_t cl;
	clientConnection_t clc;
	clientStatic_t cls;
	int serverReliableAcknowledge;
} demoContext_t;

extern demoContext_t *ctx;
