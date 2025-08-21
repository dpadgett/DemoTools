#pragma once
#include "client/client.h"

typedef struct demoContext_s {
	clientActive_t cl;
	clientConnection_t clc;
	clientStatic_t cls;
	int serverReliableAcknowledge;
	int areabytes;  // of cl.snap
	byte messageExtraByte;
	entityState_t	parseEntitiesFloatForced[MAX_PARSE_ENTITIES];
	playerState_t	playerStateForcedFields[PACKET_BACKUP];
	playerState_t	vehPlayerStateForcedFields[PACKET_BACKUP];
} demoContext_t;

extern demoContext_t *ctx;
