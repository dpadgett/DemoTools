#pragma once
#include "demo_common.h"

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
	playerState_t vehPlayerStates[2][MAX_CLIENTS];
	// bitmask to determine current vs previous player states
	int curPlayerStateIdxMask;
	// 1 if the prev player state is set
	int playerStateValidMask;
	int initialServerReliableAcknowledgeMask;
	int initialServerReliableAcknowledge[MAX_CLIENTS];
	int initialServerMessageSequence[MAX_CLIENTS];
	int initialServerCommandSequence[MAX_CLIENTS];
	int reliableAcknowledgeIdxMask;
	int reliableAcknowledge[2][MAX_CLIENTS];
	int serverReliableAcknowledge[2][MAX_CLIENTS];
	int lastServerMessageSequence[MAX_CLIENTS];
	int serverMessageSequence[MAX_CLIENTS];
} combinedDemoContext_t;

extern combinedDemoContext_t* cctx;
