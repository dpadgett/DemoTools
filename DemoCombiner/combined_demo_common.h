#pragma once
#include "demo_common.h"

#define MAX_COMBINED_MSGLEN ( MAX_MSGLEN * 32 )

enum svc_combined_ops_e {
	csvc_EOF = svc_EOF,
	svc_demoMetadata,
	svc_demoEnded,
	svc_demoGamestate,
	svc_demoGamestateOverrides,
	svc_demoTailData,
	svc_demoEntityOverrides,
};

typedef struct entityOverride_s {
	int clientNum;
	int entNum;
	entityState_t *baseEnt;
	entityState_t *ent;
} entityOverride_t;

// metadata for the gamestate message
typedef struct gamestateMetadata_s {
	int demoIdx;
	int serverReliableAcknowledge;
	int serverMessageSequence;
	int serverCommandSequence;
	int reliableAcknowledge;
	byte messageExtraByte;
	int numConfigStringOverrides;
	int maxConfigStringOverrides;
	int *configStringOverrideIndex;
	char **configStringOverride;
	qboolean overridesSent;
} gamestateMetadata_t;

typedef struct demoMetadata_s {
	char filename[MAX_STRING_CHARS];
	int fileMtime;
	uint32_t fileSize;
	uint32_t fileCrc;
	int clientnum;
	int firstFrameTime;
	gamestateMetadata_t* lastGamestate;
	// whether the file reached the end of stream
	qboolean eos;
	qboolean eosSent;
	int tailLen;  // set if file ended with extra bytes
	int tailOffset;
	byte* tail;
	qboolean tailSent;
} demoMetadata_t;

static inline uint32_t DemoCrc32Update( uint32_t crc, const void *data, size_t len ) {
	static uint32_t table[256];
	static qboolean tableInit = qfalse;
	if ( !tableInit ) {
		for ( uint32_t i = 0; i < 256; i++ ) {
			uint32_t c = i;
			for ( int k = 0; k < 8; k++ ) {
				c = ( c & 1 ) ? ( 0xEDB88320U ^ ( c >> 1 ) ) : ( c >> 1 );
			}
			table[i] = c;
		}
		tableInit = qtrue;
	}
	const byte *p = (const byte *)data;
	crc = ~crc;
	for ( size_t i = 0; i < len; i++ ) {
		crc = table[( crc ^ p[i] ) & 0xFFU] ^ ( crc >> 8 );
	}
	return ~crc;
}

typedef struct combinedDemoContext_s {
	demoContext_t ctx;
	char serverCommandBuffer[1024][MAX_STRING_CHARS];
	int serverCommandBufferCount;
	int serverCommandBitmask[MAX_BUFFERED_SERVER_COMMANDS];
	int32_t ent_owners[2][MAX_GENTITIES];
	int ent_owner_idx = 0;
	// for current snapshot
	int numMatches = 0;
	// demo file index
	int matches[256];
	// bitmask
	int32_t matchedClients;
	// which snap was delta'd from for each matched client
	int deltasnap[MAX_CLIENTS];
	playerState_t playerStates[2][MAX_CLIENTS];
	playerState_t playerStatesForcedFields[2][MAX_CLIENTS];
	playerState_t vehPlayerStates[2][MAX_CLIENTS];
	playerState_t vehPlayerStatesForcedFields[2][MAX_CLIENTS];
	// bitmask to determine current vs previous player states
	int curPlayerStateIdxMask;
	// 1 if the prev player state is set
	int playerStateValidMask;
	byte messageExtraByte[MAX_CLIENTS];
	int reliableAcknowledgeIdxMask;
	int reliableAcknowledge[2][MAX_CLIENTS];
	int serverReliableAcknowledge[2][MAX_CLIENTS];
	int lastServerMessageSequence[MAX_CLIENTS];
	int serverMessageSequence[MAX_CLIENTS];
	byte areabytes[MAX_CLIENTS];
	byte areamask[MAX_CLIENTS][MAX_MAP_AREA_BYTES];
	entityState_t parseEntitiesFloatForced[MAX_PARSE_ENTITIES];
	int parseEntitiesNoDeltaForced[MAX_PARSE_ENTITIES];
	int snapFlags[MAX_CLIENTS];
	int numDemos;
	int numHandledDemos;
	demoMetadata_t *demos;
	int numGamestates;
	int numHandledGamestates;
	int maxGamestates;
	gamestateMetadata_t **gamestates;
	int numEntityOverrides;
	int maxEntityOverrides;
	int entityOverrideClientMask;
	entityOverride_t *entityOverrides;
} combinedDemoContext_t;

extern combinedDemoContext_t* cctx;

static inline gamestateMetadata_t* AllocCombinedGamestate( combinedDemoContext_t *c ) {
	if ( c->numGamestates >= c->maxGamestates ) {
		int newMax = c->maxGamestates ? c->maxGamestates * 2 : 64;
		c->gamestates = (gamestateMetadata_t**) realloc( c->gamestates, newMax * sizeof( gamestateMetadata_t* ) );
		c->maxGamestates = newMax;
	}
	gamestateMetadata_t *gs = (gamestateMetadata_t*) calloc( 1, sizeof( gamestateMetadata_t ) );
	c->gamestates[c->numGamestates++] = gs;
	return gs;
}

static inline void AddConfigStringOverride( gamestateMetadata_t *gs, int index, const char *str ) {
	if ( gs->numConfigStringOverrides >= gs->maxConfigStringOverrides ) {
		int newMax = gs->maxConfigStringOverrides ? gs->maxConfigStringOverrides * 2 : 16;
		gs->configStringOverrideIndex = (int*) realloc( gs->configStringOverrideIndex, newMax * sizeof( int ) );
		gs->configStringOverride = (char**) realloc( gs->configStringOverride, newMax * sizeof( char* ) );
		gs->maxConfigStringOverrides = newMax;
	}
	int idx = gs->numConfigStringOverrides++;
	gs->configStringOverrideIndex[idx] = index;
	size_t len = strlen( str ) + 1;
	gs->configStringOverride[idx] = (char*) malloc( len );
	memcpy( gs->configStringOverride[idx], str, len );
}

static inline void AddEntityOverride( combinedDemoContext_t *c, int clientNum, int entNum, entityState_t *baseEnt, entityState_t *ent ) {
	if ( c->numEntityOverrides >= c->maxEntityOverrides ) {
		int newMax = c->maxEntityOverrides ? c->maxEntityOverrides * 2 : 64;
		c->entityOverrides = (entityOverride_t*) realloc( c->entityOverrides, newMax * sizeof( entityOverride_t ) );
		c->maxEntityOverrides = newMax;
	}
	entityOverride_t *ov = &c->entityOverrides[c->numEntityOverrides++];
	ov->clientNum = clientNum;
	ov->entNum = entNum;
	ov->baseEnt = baseEnt;
	ov->ent = ent;
	c->entityOverrideClientMask |= ( 1 << clientNum );
}

static inline void FreeCombinedGamestates( combinedDemoContext_t *c ) {
	free( c->entityOverrides );
	c->entityOverrides = NULL;
	c->numEntityOverrides = 0;
	c->maxEntityOverrides = 0;
	c->entityOverrideClientMask = 0;
	if ( !c->gamestates ) {
		return;
	}
	for ( int i = 0; i < c->numGamestates; i++ ) {
		gamestateMetadata_t *gs = c->gamestates[i];
		if ( gs ) {
			for ( int j = 0; j < gs->numConfigStringOverrides; j++ ) {
				free( gs->configStringOverride[j] );
			}
			free( gs->configStringOverride );
			free( gs->configStringOverrideIndex );
			free( gs );
		}
	}
	free( c->gamestates );
	c->gamestates = NULL;
	c->numGamestates = 0;
	c->maxGamestates = 0;
}
