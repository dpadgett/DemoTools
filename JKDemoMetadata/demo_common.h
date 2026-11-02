#pragma once
#include "client/client.h"
#include "enhanced_msg.h"

typedef struct demoContext_s {
	clientActive_t cl;
	clientConnection_t clc;
	clientStatic_t cls;
	int serverReliableAcknowledge;
	int areabytes;  // of cl.snap
	byte messageExtraByte;
#define ENTITY_POOL_SIZE 16384
	entityState_t	parseEntitiesFloatForced[MAX_PARSE_ENTITIES];
	entityState_t*	parseEntityPtr[MAX_PARSE_ENTITIES];
	uint32_t		parseEntityVersion[MAX_PARSE_ENTITIES];
	entityState_t	entityPool[ENTITY_POOL_SIZE];
	uint32_t		entityPoolSlotIdx[ENTITY_POOL_SIZE];
	uint32_t		entityPoolSlotVer[ENTITY_POOL_SIZE];
	uint32_t		entityPoolIdx;
	playerState_t	playerStateForcedFields[PACKET_BACKUP];
	playerState_t	vehPlayerStateForcedFields[PACKET_BACKUP];
} demoContext_t;

extern demoContext_t *ctx;

static inline void CopyClSnapshot( clSnapshot_t* dst, const clSnapshot_t* src ) {
	if ( src->ps.m_iVehicleNum ) {
		*dst = *src;
	} else {
		memcpy( dst, src, offsetof( clSnapshot_t, vps ) );
		dst->numEntities = src->numEntities;
		dst->parseEntitiesNum = src->parseEntitiesNum;
		dst->serverCommandNum = src->serverCommandNum;
	}
}
