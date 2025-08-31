#pragma once
#include "demo_common.h"

enum svc_combined_ops_e {
	csvc_EOF = svc_EOF,
	svc_demoMetadata,
	svc_demoEnded,
	svc_demoGamestate,
	svc_demoGamestateOverrides,
};

// metadata for the gamestate message
typedef struct gamestateMetadata_s {
	int demoIdx;
	int serverReliableAcknowledge;
	int serverMessageSequence;
	int serverCommandSequence;
	int reliableAcknowledge;
	byte messageExtraByte;
	int numConfigStringOverrides;
	int configStringOverrideIndex[MAX_CONFIGSTRINGS];
	char *configStringOverride[MAX_CONFIGSTRINGS];
} gamestateMetadata_t;

typedef struct demoMetadata_s {
	char filename[MAX_STRING_CHARS];
	int fileMtime;
	int clientnum;
	int firstFrameTime;
	gamestateMetadata_t* lastGamestate;
	// whether the file reached the end of stream
	qboolean eos;
	qboolean eosSent;
} demoMetadata_t;

typedef struct combinedDemoContext_s {
	demoContext_t ctx;
	char serverCommandBuffer[1024][MAX_STRING_CHARS];
	int serverCommandBufferCount;
	int serverCommandBitmask[MAX_RELIABLE_COMMANDS];
	int32_t ent_owners[2][MAX_GENTITIES];
	int ent_owner_idx = 0;
	// for current snapshot
	int numMatches = 0;
	// demo file index
	int matches[MAX_CLIENTS];
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
	byte areamask[MAX_CLIENTS][MAX_MAP_AREA_BYTES];
	entityState_t parseEntitiesFloatForced[MAX_PARSE_ENTITIES];
	int snapFlags[MAX_CLIENTS];
	int numDemos;
	demoMetadata_t *demos;
	int numGamestates;
	int numHandledGamestates;
	gamestateMetadata_t gamestates[MAX_CLIENTS * 5];  // allow 5 gamestates per client
} combinedDemoContext_t;

extern combinedDemoContext_t* cctx;
