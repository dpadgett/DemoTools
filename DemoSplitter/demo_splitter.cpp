// demo_splitter.cpp : Defines the entry point for the console application.
//

#include "deps.h"
#include "client/client.h"
#include "demo_utils.h"
#include "demo_common.h"
#include "combined_demo_common.h"
#include "utils.h"
#include "jansson.h"

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

typedef struct {
	char filename[MAX_OSPATH];
	fileHandle_t fp;
	combinedDemoContext_t *cctx;
	demoContext_t *ctx;  // filtered to a single client
	int firstServerCommand;
	int framesSaved;
	int currentMap;
	qboolean eos;
	// idx of merged server command seq where the last server cmd from this demo was
	int serverCommandSyncIdx;
} demoEntry_t;
demoEntry_t entry;

void FreeMsg( msg_t *msg ) {
}

extern	cvar_t* cl_shownet;

static const char* svc_strings[256] = {
	"svc_bad",

	"svc_nop",
	"svc_gamestate",
	"svc_configstring",
	"svc_baseline",
	"svc_serverCommand",
	"svc_download",
	"svc_snapshot",
	"svc_setgame",
	"svc_mapchange",
};

#define SHOWNET( msg, s ) do { if ( cl_shownet->integer >= 2 ) { Com_Printf( "%3i:%s\n", (msg)->readcount - 1, (s) ); } } while ( 0 )

/*
==================
CL_ParsePacketEntities

==================
*/
static entityState_t zeroEnt = {};
void CL_DeltaEntity( msg_t* msg, clSnapshot_t* frame, int newnum, entityState_t* old, qboolean unchanged );
void MSG_ReadDeltaEntityWithFloats( msg_t* msg, entityState_t* from, entityState_t* to, entityState_t* floatForced,
	int number, qboolean isFloatForced );
void CL_ParseMergedPacketEntities( msg_t* msg, clSnapshot_t* oldframe, clSnapshot_t* newframe ) {
	int			newnum;
	entityState_t* oldstate;
	entityState_t* oldfloatForced;
	int			oldindex, oldnum;
	int* owners = cctx->ent_owners[cctx->ent_owner_idx ^ 1];
	int* prev_owners = cctx->ent_owners[cctx->ent_owner_idx];
	cctx->ent_owner_idx ^= 1;
	memset( owners, 0, sizeof( *owners ) * MAX_GENTITIES );

	newframe->parseEntitiesNum = ctx->cl.parseEntitiesNum;
	newframe->numEntities = 0;

	// delta from the entities present in oldframe
	oldindex = 0;
	oldstate = NULL;
	oldfloatForced = NULL;
	if ( !oldframe ) {
		oldnum = 99999;
	}
	else {
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		}
		else {
			oldstate = ctx->parseEntityPtr[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldfloatForced = &ctx->parseEntitiesFloatForced[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}

	if ( cl_shownet->integer >= 1 ) {
		Com_Printf( "owner_delta: " );
	}
	while ( 1 ) {
		// read the entity index number
		newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
		if ( newframe->serverTime == 1370498 ) {
			Com_Printf( "WTF7\n" );
		}

		if ( newnum == ( MAX_GENTITIES - 1 ) ) {
			break;
		}

		if ( msg->readcount > msg->cursize ) {
			Com_Error( ERR_DROP, "CL_ParsePacketEntities: end of message" );
		}

		while ( oldnum < newnum ) {
			// one or more entities from the old packet are unchanged
			if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
			}
			CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );
			if ( oldfloatForced->number ) {
				int penum = ctx->cl.parseEntitiesNum - 1;
				entityState_t* newfloatForced = &ctx->parseEntitiesFloatForced[penum & ( MAX_PARSE_ENTITIES - 1 )];
				*newfloatForced = *oldfloatForced;
			}
			owners[oldnum] = prev_owners[oldnum];

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			}
			else {
				oldstate = ctx->parseEntityPtr[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldfloatForced = &ctx->parseEntitiesFloatForced[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
		}
		if ( oldnum == newnum ) {
			// delta from previous state
			if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  delta: %i\n", msg->readcount, newnum );
			}
			int numEnts = newframe->numEntities;
			CL_DeltaEntity( msg, newframe, newnum, oldstate, qfalse );
			if ( newframe->numEntities == numEnts ) {
				// it was a delta remove
				owners[newnum] = 0;
			} else {
				int penum = ctx->cl.parseEntitiesNum - 1;
				entityState_t* newfloatForced = &ctx->parseEntitiesFloatForced[penum & ( MAX_PARSE_ENTITIES - 1 )];
				MSG_ReadDeltaEntityWithFloats( msg, oldfloatForced, newfloatForced, NULL, 0, qtrue );
				// read owner delta
				int owner_delta = 0;
				qboolean unchanged = MSG_ReadBits( msg, 1 ) == 0 ? qtrue : qfalse;
				if ( !unchanged ) {
					owner_delta = MSG_ReadLong( msg );
				}
				if ( cl_shownet->integer >= 1 ) {
					Com_Printf( "%d %d ", newnum, owner_delta );
				}
				owners[newnum] = prev_owners[newnum] ^ owner_delta;
			}

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			}
			else {
				oldstate = ctx->parseEntityPtr[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldfloatForced = &ctx->parseEntitiesFloatForced[
					( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
			continue;
		}

		if ( oldnum > newnum ) {
			// delta from baseline
			if ( cl_shownet->integer == 3 ) {
				Com_Printf( "%3i:  baseline: %i\n", msg->readcount, newnum );
			}
			int numEnts = newframe->numEntities;
			CL_DeltaEntity( msg, newframe, newnum, &ctx->cl.entityBaselines[newnum], qfalse );
			if ( newframe->numEntities == numEnts ) {
				// this was a delta remove, so didn't send owners
				owners[newnum] = 0;
				continue;
			}
			int penum = ctx->cl.parseEntitiesNum - 1;
			entityState_t* newfloatForced = &ctx->parseEntitiesFloatForced[penum & ( MAX_PARSE_ENTITIES - 1 )];
			MSG_ReadDeltaEntityWithFloats( msg, &zeroEnt, newfloatForced, NULL, 0, qtrue );
			// read owner delta
			int owner_delta = 0;
			qboolean unchanged = MSG_ReadBits( msg, 1 ) == 0 ? qtrue : qfalse;
			if ( !unchanged ) {
				owner_delta = MSG_ReadLong( msg );
			}
			if ( cl_shownet->integer >= 1 ) {
				Com_Printf( "%d %d ", newnum, owner_delta );
			}
			owners[newnum] = prev_owners[newnum] ^ owner_delta;
			continue;
		}

	}

	// any remaining entities in the old frame are copied over
	while ( oldnum != 99999 ) {
		// one or more entities from the old packet are unchanged
		if ( cl_shownet->integer == 3 ) {
			Com_Printf( "%3i:  unchanged: %i\n", msg->readcount, oldnum );
		}
		CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );
		if ( oldfloatForced->number ) {
			int penum = ctx->cl.parseEntitiesNum - 1;
			entityState_t* newfloatForced = &ctx->parseEntitiesFloatForced[penum & ( MAX_PARSE_ENTITIES - 1 )];
			*newfloatForced = *oldfloatForced;
		}
		owners[oldnum] = prev_owners[oldnum];

		oldindex++;

		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		}
		else {
			oldstate = ctx->parseEntityPtr[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldfloatForced = &ctx->parseEntitiesFloatForced[
				( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}

	if ( cl_shownet->integer >= 1 ) {
		Com_Printf( "\n" );
	}
}

/*
================
CL_ParseSnapshot

If the snapshot is parsed properly, it will be copied to
cl.snap and saved in cl.snapshots[].  If the snapshot is invalid
for any reason, no changes to the state will be made at all.
================
*/
void CL_ParseMergedSnapshot( msg_t* msg ) {
	int			len;
	clSnapshot_t* old;
	clSnapshot_t	newSnap;
	int			deltaNum;
	int			oldMessageNum;
	int			i, packetNum;

	// get the reliable sequence acknowledge number
	// NOTE: now sent with all server to client messages
	//clc.reliableAcknowledge = MSG_ReadLong( msg );

	// read in the new snapshot to a temporary buffer
	// we will only copy to cl.snap if it is valid
	Com_Memset( &newSnap, 0, offsetof( clSnapshot_t, ps ) );
	newSnap.numEntities = 0;
	newSnap.parseEntitiesNum = 0;

	// we will have read any new server commands in this
	// message before we got to svc_snapshot
	newSnap.serverCommandNum = ctx->clc.serverCommandSequence;

	newSnap.serverTime = MSG_ReadLong( msg );

	cctx->matchedClients = MSG_ReadLong( msg );
	// read original delta frame for each match
	for ( int i = 0; ( 1 << i ) <= cctx->matchedClients; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			int deltaNum = MSG_ReadByte( msg );
			cctx->deltasnap[i] = deltaNum;
			if ( deltaNum == 0 || ( deltaNum & 0x80 ) ) {
				int raIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
				cctx->serverReliableAcknowledge[raIdx][i] = MSG_ReadLong( msg );
			}
		}
	}

	// if we were just unpaused, we can only *now* really let the
	// change come into effect or the client hangs.
	//cl_paused->modified = qfalse;

	newSnap.messageNum = ctx->clc.serverMessageSequence;

	deltaNum = MSG_ReadByte( msg );
	if ( !deltaNum ) {
		newSnap.deltaNum = -1;
	}
	else {
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	bool snapFlagsIdentical = MSG_ReadBits( msg, 1 );
	newSnap.snapFlags = snapFlagsIdentical ? MSG_ReadByte( msg ) : -1;
	for ( int i = 0; ( 1 << i ) <= cctx->matchedClients; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			if ( snapFlagsIdentical ) {
				cctx->snapFlags[i] = newSnap.snapFlags;
			} else {
				cctx->snapFlags[i] = MSG_ReadByte( msg );
				if ( newSnap.snapFlags == -1 ) {
					newSnap.snapFlags = cctx->snapFlags[i];
				}
			}
		}
	}
	

	// If the frame is delta compressed from data that we
	// no longer have available, we must suck up the rest of
	// the frame, but not use it, then ask for a non-compressed
	// message
	if ( newSnap.deltaNum <= 0 ) {
		newSnap.valid = qtrue;		// uncompressed frame
		old = NULL;
		ctx->clc.demowaiting = qfalse;	// we can start recording now
	}
	else {
		old = &ctx->cl.snapshots[newSnap.deltaNum & PACKET_MASK];
		if ( !old->valid ) {
			// should never happen
			Com_Printf( "Delta from invalid frame (not supposed to happen!).\n" );
			while ( ( newSnap.deltaNum & PACKET_MASK ) != ( newSnap.messageNum & PACKET_MASK ) && !old->valid ) {
				newSnap.deltaNum++;
				old = &ctx->cl.snapshots[newSnap.deltaNum & PACKET_MASK];
			}
			if ( old->valid ) {
				Com_Printf( "Found more recent frame to delta from.\n" );
			}
		}
		if ( !old->valid ) {
			Com_Printf( "Failed to find more recent frame to delta from.\n" );
		}
		else if ( old->messageNum != newSnap.deltaNum ) {
			// The frame that the server did the delta from
			// is too old, so we can't reconstruct it properly.
			Com_Printf( "Delta frame too old.\n" );
		}
		else if ( ctx->cl.parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES - 128 ) {
			Com_Printf( "Delta parseEntitiesNum too old.\n" );
		}
		else {
			newSnap.valid = qtrue;	// valid delta parse
		}
	}

	// read areamask
	len = MSG_ReadByte( msg );

	if ( (unsigned) len > sizeof( newSnap.areamask ) )
	{
		Com_Error( ERR_DROP, "CL_ParseSnapshot: Invalid size %d for areamask", len );
		return;
	}

	ctx->areabytes = len;
	for ( int i = 0; ( 1 << i ) <= cctx->matchedClients; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			MSG_ReadData( msg, &cctx->areamask[i], len );
		}
	}

	// read playerinfo
	SHOWNET( msg, "playerstate" );
	for ( int i = 0; ( 1 << i ) <= cctx->matchedClients; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			playerState_t* ps = NULL, * oldps = NULL, * vps = NULL, * oldvps = NULL;
			playerState_t* psff = NULL, * oldpsff = NULL, * vpsff = NULL, * oldvpsff = NULL;
			int psIdx = cctx->curPlayerStateIdxMask & ( 1 << i ) ? 0 : 1;
			ps = &cctx->playerStates[psIdx][i];
			psff = &cctx->playerStatesForcedFields[psIdx][i];
			vps = &cctx->vehPlayerStates[psIdx][i];
			vpsff = &cctx->vehPlayerStatesForcedFields[psIdx][i];
			if ( cctx->playerStateValidMask & ( 1 << i ) ) {
				oldps = &cctx->playerStates[psIdx ^ 1][i];
				oldpsff = &cctx->playerStatesForcedFields[psIdx ^ 1][i];
				if ( oldps->m_iVehicleNum ) {
					// leave null if oldps didn't have a vehicle
					oldvps = &cctx->vehPlayerStates[psIdx ^ 1][i];
					oldvpsff = &cctx->vehPlayerStatesForcedFields[psIdx ^ 1][i];
				}
			}
			MSG_ReadDeltaPlayerstate( msg, oldps, ps );
			MSG_ReadDeltaPlayerstateForcedFields( msg, oldpsff, psff );
			if ( oldps && oldps->commandTime == 765062 ) {
				Com_Printf( "WTF6?\n" );
			}
			if ( ps->m_iVehicleNum )
			{ //this means we must have written our vehicle's ps too
				MSG_ReadDeltaPlayerstate( msg, oldvps, vps, qtrue );
				MSG_ReadDeltaPlayerstateForcedFields( msg, oldvpsff, vpsff, qtrue );
			}
			cctx->curPlayerStateIdxMask ^= ( 1 << i );
			cctx->playerStateValidMask |= ( 1 << i );
		}
	}

	// read packet entities
	SHOWNET( msg, "packet entities" );
	CL_ParseMergedPacketEntities( msg, old, &newSnap );

	// if not valid, dump the entire thing now that it has
	// been properly read
	if ( !newSnap.valid ) {
		return;
	}

	// clear the valid flags of any snapshots between the last
	// received and this one, so if there was a dropped packet
	// it won't look like something valid to delta from next
	// time we wrap around in the buffer
	oldMessageNum = ctx->cl.snap.messageNum + 1;

	if ( newSnap.messageNum - oldMessageNum >= PACKET_BACKUP ) {
		oldMessageNum = newSnap.messageNum - ( PACKET_BACKUP - 1 );
	}
	for ( ; oldMessageNum < newSnap.messageNum; oldMessageNum++ ) {
		ctx->cl.snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
	}

	// copy to the current good spot
	newSnap.ping = 999;
	// calculate ping time
	for ( i = 0; i < PACKET_BACKUP; i++ ) {
		packetNum = ( ctx->clc.netchan.outgoingSequence - 1 - i ) & PACKET_MASK;
		if ( newSnap.ps.commandTime >= ctx->cl.outPackets[packetNum].p_serverTime ) {
			newSnap.ping = ctx->cls.realtime - ctx->cl.outPackets[packetNum].p_realtime;
			break;
		}
	}
	CopyClSnapshot( &ctx->cl.snap, &newSnap );
	// save the frame off in the backup array for later delta comparisons
	CopyClSnapshot( &ctx->cl.snapshots[ctx->cl.snap.messageNum & PACKET_MASK], &ctx->cl.snap );

	if ( cl_shownet->integer == 3 ) {
		Com_Printf( "   snapshot:%i  delta:%i  ping:%i\n", ctx->cl.snap.messageNum,
			ctx->cl.snap.deltaNum, ctx->cl.snap.ping );
	}

	ctx->cl.newSnapshots = qtrue;
}

/*
=====================
CL_ParseCommandString

Command strings are just saved off until cgame asks for them
when it transitions a snapshot
=====================
*/
typedef struct cmdToken_s {
	const char* ptr;
	int len;
} cmdToken_t;

static int SplitCommandTokens( const char* str, cmdToken_t* tokens, int maxTokens ) {
	int count = 0;
	const char* p = str;
	while ( 1 ) {
		if ( count >= maxTokens ) {
			return -1;
		}
		const char* space = strchr( p, ' ' );
		if ( !space ) {
			tokens[count].ptr = p;
			tokens[count].len = (int) strlen( p );
			count++;
			break;
		}
		tokens[count].ptr = p;
		tokens[count].len = (int) ( space - p );
		count++;
		p = space + 1;
	}
	return count;
}

void CL_ParseMergedCommandString( msg_t* msg, qboolean firstServerCommandInMessage ) {
	char* s;
	int		seq;
	int		index;

	seq = MSG_ReadLong( msg );
	int bitmask = MSG_ReadLong( msg );
	qboolean buffered = (qboolean) MSG_ReadBits( msg, 1 );
	if ( buffered ) {
		int commandIdx = MSG_ReadShort( msg );
		s = cctx->serverCommandBuffer[( cctx->serverCommandBufferCount - commandIdx) % 1024];
	}
	else {
		qboolean isDelta = (qboolean) MSG_ReadBits( msg, 1 );
		if ( !isDelta ) {
			s = MSG_ReadString( msg );
		} else {
			static char deltaCommandBuf[MAX_STRING_CHARS];
			int distLong = MSG_ReadBits( msg, 1 );
			int dist = ( distLong ? MSG_ReadBits( msg, 7 ) : MSG_ReadBits( msg, 4 ) ) + 1;
			const char* ref = cctx->serverCommandBuffer[( cctx->serverCommandBufferCount - dist ) % 1024];
			cmdToken_t refToks[256];
			int refNumToks = SplitCommandTokens( ref, refToks, 256 );
			int sameNumToks = MSG_ReadBits( msg, 1 );
			int numToks = sameNumToks ? refNumToks : MSG_ReadBits( msg, 8 );
			int outPos = 0;
			int r_idx = 0;
			for ( int i = 0; i < numToks; i++ ) {
				if ( i > 0 && outPos < MAX_STRING_CHARS - 1 ) {
					deltaCommandBuf[outPos++] = ' ';
				}
				int changed = MSG_ReadBits( msg, 1 );
				if ( !changed ) {
					int copyLen = refToks[r_idx].len;
					if ( outPos + copyLen >= MAX_STRING_CHARS ) {
						copyLen = MAX_STRING_CHARS - 1 - outPos;
					}
					if ( copyLen > 0 ) {
						memcpy( deltaCommandBuf + outPos, refToks[r_idx].ptr, copyLen );
						outPos += copyLen;
					}
					r_idx++;
				} else {
					int isJump = MSG_ReadBits( msg, 1 );
					if ( isJump ) {
						int jumpIdx = MSG_ReadBits( msg, 8 );
						int copyLen = refToks[jumpIdx].len;
						if ( outPos + copyLen >= MAX_STRING_CHARS ) {
							copyLen = MAX_STRING_CHARS - 1 - outPos;
						}
						if ( copyLen > 0 ) {
							memcpy( deltaCommandBuf + outPos, refToks[jumpIdx].ptr, copyLen );
							outPos += copyLen;
						}
						r_idx = jumpIdx + 1;
					} else {
						int cp = MSG_ReadBits( msg, 3 );
						if ( cp > 0 && r_idx < refNumToks ) {
							int copyLen = cp;
							if ( outPos + copyLen >= MAX_STRING_CHARS ) {
								copyLen = MAX_STRING_CHARS - 1 - outPos;
							}
							if ( copyLen > 0 ) {
								memcpy( deltaCommandBuf + outPos, refToks[r_idx].ptr, copyLen );
								outPos += copyLen;
							}
						}
						while ( 1 ) {
							int c = MSG_ReadByte( msg );
							if ( c <= 0 ) {
								break;
							}
							if ( outPos < MAX_STRING_CHARS - 1 ) {
								deltaCommandBuf[outPos++] = (char) c;
							}
						}
						r_idx++;
					}
				}
			}
			deltaCommandBuf[outPos] = '\0';
			s = deltaCommandBuf;
		}
		Q_strncpyz( cctx->serverCommandBuffer[cctx->serverCommandBufferCount % 1024], s, MAX_STRING_CHARS );
		cctx->serverCommandBufferCount++;
	}

	if ( firstServerCommandInMessage ) {
		ctx->serverReliableAcknowledge = seq - 1;
	}

	// see if we have already executed stored it off
	if ( ctx->clc.serverCommandSequence >= seq ) {
		return;
	}
	ctx->clc.serverCommandSequence = seq;

	index = seq & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( ctx->clc.serverCommands[index], s, sizeof( ctx->clc.serverCommands[index] ) );
	cctx->serverCommandBitmask[index] = bitmask;
}


void CL_ParseDemoMetadata( msg_t* msg ) {
	int count = MSG_ReadByte( msg );
	int startIdx = cctx->numDemos;
	cctx->numDemos += count;
	cctx->demos = (demoMetadata_t *) realloc( cctx->demos, cctx->numDemos * sizeof( *cctx->demos ) );
	Com_Memset( &cctx->demos[startIdx], 0, count * sizeof( *cctx->demos ) );

	for ( int idx = startIdx; idx < cctx->numDemos; idx++ ) {
		demoMetadata_t* demo = &cctx->demos[idx];
		Q_strncpyz( demo->filename, MSG_ReadString( msg ), sizeof( demo->filename ) );
		demo->fileMtime = MSG_ReadLong( msg );
		demo->clientnum = MSG_ReadByte( msg );
		demo->firstFrameTime = MSG_ReadLong( msg );
	}
}

void CL_ParseDemoEnded( msg_t* msg ) {
	int idx = MSG_ReadByte( msg );
	demoMetadata_t* demo = &cctx->demos[idx];
	demo->eos = qtrue;
	demo->eosSent = qtrue;
}

void CL_ParseDemoGamestate( msg_t* msg ) {
	int demoIdx = MSG_ReadByte( msg );
	demoMetadata_t* demo = &cctx->demos[demoIdx];
	if ( cctx->numGamestates >= (int) ARRAY_LEN( cctx->gamestates ) ) {
		Com_Error( ERR_DROP, "CL_ParseDemoGamestate: MAX_GAMESTATES exceeded" );
	}
	gamestateMetadata_t* gamestateMetadata = &cctx->gamestates[cctx->numGamestates++];
	demo->lastGamestate = gamestateMetadata;
	gamestateMetadata->demoIdx = demoIdx;
	gamestateMetadata->serverReliableAcknowledge = MSG_ReadLong( msg );
	gamestateMetadata->serverMessageSequence = MSG_ReadLong( msg );
	gamestateMetadata->serverCommandSequence = MSG_ReadLong( msg );
	gamestateMetadata->reliableAcknowledge = MSG_ReadLong( msg );
	gamestateMetadata->messageExtraByte = MSG_ReadByte( msg );
}

static char configStringOverrides[MAX_GAMESTATE_CHARS * 64];
static int configStringOverrideDataLen = 0;
void CL_ParseDemoGamestateOverrides( msg_t* msg ) {
	int demoIdx = MSG_ReadByte( msg );
	demoMetadata_t* demo = &cctx->demos[demoIdx];
	gamestateMetadata_t* gamestateMetadata = demo->lastGamestate;
	gamestateMetadata->numConfigStringOverrides = MSG_ReadShort( msg );
	for ( int i = 0; i < demo->lastGamestate->numConfigStringOverrides; i++ ) {
		demo->lastGamestate->configStringOverrideIndex[i] = MSG_ReadShort( msg );
		char *str = MSG_ReadString( msg );
		int size = strlen( str );
		Q_strncpyz( &configStringOverrides[configStringOverrideDataLen], str, sizeof( configStringOverrides ) - configStringOverrideDataLen );
		demo->lastGamestate->configStringOverride[i] = &configStringOverrides[configStringOverrideDataLen];
		configStringOverrideDataLen += size + 1;
	}
}

void CL_ParseTailData( msg_t* msg ) {
	int demoIdx = MSG_ReadByte( msg );
	demoMetadata_t* demo = &cctx->demos[demoIdx];
	demo->tailLen = MSG_ReadLong( msg );
	if ( demo->tailLen <= 0 ) {
		return;
	}

	demo->tail = (byte*) calloc( demo->tailLen, 1 );
	MSG_ReadData( msg, demo->tail, demo->tailLen );
}

/*
==================
CL_ParseGamestate
==================
*/
void CL_ParseRMG( msg_t* msg );
void CL_ParseMergedGamestate( msg_t* msg ) {
	int				i;
	entityState_t* es;
	int				newnum;
	entityState_t	nullstate;
	int				cmd;
	char* s;

	ctx->clc.connectPacketCount = 0;

	Com_Memset( &ctx->cl.gameState, 0, sizeof( ctx->cl.gameState ) );
	Com_Memset( &ctx->cl.entityBaselines, 0, sizeof( ctx->cl.entityBaselines ) );

	// a gamestate always marks a server command sequence
	//ctx->clc.serverCommandSequence = MSG_ReadLong( msg );
	ctx->clc.serverCommandSequence = -1;

	// parse all the configstrings and baselines
	ctx->cl.gameState.dataCount = 1;	// leave a 0 at the beginning for uninitialized configstrings
	while ( 1 ) {
		cmd = MSG_ReadByte( msg );

		if ( cmd == svc_EOF ) {
			break;
		}

		if ( cmd == svc_configstring ) {
			int		len, start;

			start = msg->readcount;

			i = MSG_ReadShort( msg );
			if ( i < 0 || i >= MAX_CONFIGSTRINGS ) {
				Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );
			}
			s = MSG_ReadBigString( msg );

			if ( cl_shownet->integer >= 2 )
			{
				Com_Printf( "%3i: %d: %s\n", start, i, s );
			}

			len = strlen( s );

			if ( len + 1 + ctx->cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
			}

			// append it to the gameState string buffer
			ctx->cl.gameState.stringOffsets[i] = ctx->cl.gameState.dataCount;
			Com_Memcpy( ctx->cl.gameState.stringData + ctx->cl.gameState.dataCount, s, len + 1 );
			ctx->cl.gameState.dataCount += len + 1;
		}
		else if ( cmd == svc_baseline ) {
			newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
			if ( newnum < 0 || newnum >= MAX_GENTITIES ) {
				Com_Error( ERR_DROP, "Baseline number out of range: %i", newnum );
			}
			Com_Memset( &nullstate, 0, sizeof( nullstate ) );
			es = &ctx->cl.entityBaselines[newnum];
			MSG_ReadDeltaEntity( msg, &nullstate, es, newnum );
		}
		else {
			Com_Error( ERR_DROP, "CL_ParseGamestate: bad command byte" );
		}
	}

	ctx->clc.clientNum = MSG_ReadLong( msg );
	// read the checksum feed
	ctx->clc.checksumFeed = MSG_ReadLong( msg );

	CL_ParseRMG( msg ); //rwwRMG - get info for it from the server
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseSetGame( msg_t* msg );
void CL_ParseDownload( msg_t* msg );
void CL_ParseMergedServerMessage( msg_t* msg ) {
	int			cmd;

	if ( cl_shownet->integer == 2 ) {
		Com_Printf( "%i ", msg->cursize );
	}
	else if ( cl_shownet->integer >= 2 ) {
		Com_Printf( "------------------\n" );
	}

	MSG_Bitstream( msg );

	// get the message number for clients who incremented by more than 1
	// rest of clients will be updated once we get the matched client mask for the snapshot
	long serverMessageSequenceMask = MSG_ReadLong( msg );
	for ( int i = 0; ( 1 << i ) <= serverMessageSequenceMask; i++ ) {
		if ( serverMessageSequenceMask & ( 1 << i ) ) {
			cctx->lastServerMessageSequence[i] = cctx->serverMessageSequence[i];
			int prev = cctx->lastServerMessageSequence[i];
			int* cur = &cctx->serverMessageSequence[i];
			int delta = MSG_ReadLong( msg );
			*cur = prev + 1 + delta;
		}
	}

	// get the reliable sequence acknowledge number
	//long reliableAcknowledge = MSG_ReadLong( msg );
	long reliableAcknowledgeMask = MSG_ReadLong( msg );
	for ( int i = 0; ( 1 << i ) <= reliableAcknowledgeMask; i++ ) {
		if ( reliableAcknowledgeMask & ( 1 << i ) ) {
			int raIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 1 : 0;
			int prev = cctx->reliableAcknowledge[raIdx ^ 1][i];
			int *cur = &cctx->reliableAcknowledge[raIdx][i];
			int delta = MSG_ReadLong( msg );
			*cur = prev + delta;
			cctx->reliableAcknowledgeIdxMask ^= ( 1 << i );
		}
	}

	qboolean firstServerCommandInMessage = qtrue;

	//
	// parse the message
	//
	while ( 1 ) {
		if ( msg->readcount > msg->cursize ) {
			Com_Error( ERR_DROP, "CL_ParseServerMessage: read past end of server message" );
			break;
		}

		cmd = MSG_ReadByte( msg );

		if ( cmd == svc_EOF ) {
			SHOWNET( msg, "END OF MESSAGE" );
			int matchedMask = cctx->matchedClients;
			if ( !ctx->cl.newSnapshots || matchedMask == 0 ) {
				break;
			}
			for ( int i = 0; ( 1 << i ) <= matchedMask; i++ ) {
				if ( matchedMask & ( 1 << i ) ) {
					int extraByte = MSG_ReadByte( msg );
					cctx->messageExtraByte[i] = extraByte;
				}
			}
			break;
		}

		if ( cl_shownet->integer >= 2 ) {
			if ( !svc_strings[cmd] ) {
				Com_Printf( "%3i:BAD CMD %i\n", msg->readcount - 1, cmd );
			}
			else {
				SHOWNET( msg, svc_strings[cmd] );
			}
		}

		// other commands
		switch ( cmd ) {
		default:
			Com_Error( ERR_DROP, "CL_ParseServerMessage: Illegible server message\n" );
			break;
		case svc_nop:
			break;
		case svc_serverCommand:
			CL_ParseMergedCommandString( msg, firstServerCommandInMessage );
			firstServerCommandInMessage = qfalse;
			break;
		case svc_gamestate:
			CL_ParseMergedGamestate( msg );
			break;
		case svc_snapshot:
			CL_ParseMergedSnapshot( msg );
			serverMessageSequenceMask = cctx->matchedClients ^ serverMessageSequenceMask;
			for ( int i = 0; ( 1 << i ) <= serverMessageSequenceMask; i++ ) {
				if ( serverMessageSequenceMask & ( 1 << i ) ) {
					cctx->lastServerMessageSequence[i] = cctx->serverMessageSequence[i];
					int prev = cctx->lastServerMessageSequence[i];
					int* cur = &cctx->serverMessageSequence[i];
					*cur = prev + 1;
				}
			}
			break;
		case svc_setgame:
			CL_ParseSetGame( msg );
			break;
		case svc_download:
			CL_ParseDownload( msg );
			break;
		case svc_mapchange:
			//if ( cls.cgameStarted )
			//	CGVM_MapChange();
			break;
		case svc_demoMetadata:
			CL_ParseDemoMetadata( msg );
			break;
		case svc_demoEnded:
			CL_ParseDemoEnded( msg );
			break;
		case svc_demoGamestate:
			CL_ParseDemoGamestate( msg );
			break;
		case svc_demoGamestateOverrides:
			CL_ParseDemoGamestateOverrides( msg );
			break;
		case svc_demoTailData:
			CL_ParseTailData( msg );
			break;
		}
	}
}

// returns demoFinished
msg_t *ReadNextMessageRaw( demoEntry_t *demo ) {
	static msg_t s_msg;
	static byte s_msgData[MAX_COMBINED_MSGLEN];
	MSG_Init( &s_msg, s_msgData, MAX_COMBINED_MSGLEN );
	if ( !CL_ReadDemoMessage( demo->fp, &s_msg ) ) {
		return nullptr;
	}
	return &s_msg;
}

msg_t *ReadNextMessage( demoEntry_t *demo ) {
	msg_t *msg;
	int entityEventSequence = 0;
	if ( ctx->cl.snap.valid ) {
		entityEventSequence = ctx->cl.snap.ps.entityEventSequence;
	}
	ctx->cl.newSnapshots = qfalse;
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		try {
			CL_ParseMergedServerMessage( msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			FreeMsg( msg );
			return nullptr;
		}

		if ( !ctx->cl.newSnapshots ) {
			FreeMsg( msg );
			continue;
		}
		ctx->cl.snap.ps.entityEventSequence = entityEventSequence;
		if ( lastSnapValid && ( ( lastSnapFlags ^ ctx->cl.snap.snapFlags ) & SNAPFLAG_SERVERCOUNT ) != 0 ) {
			demo->currentMap++;
		}
		if ( ctx->clc.serverCommandSequence - ctx->clc.lastExecutedServerCommand > MAX_RELIABLE_COMMANDS ) {
			ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10; // fudge factor
		}
		demo->firstServerCommand = ctx->clc.lastExecutedServerCommand;
		int firstNonEmptyCommand = 0;
		// process any new server commands
		for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
			char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			if ( command[0] && firstNonEmptyCommand == 0 ) {
				firstNonEmptyCommand = ctx->clc.lastExecutedServerCommand;
			}
		}
		if ( demo->firstServerCommand == ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10 && firstNonEmptyCommand > 0 ) {
			demo->firstServerCommand = firstNonEmptyCommand;
		}
		return msg;
	}
	return nullptr;
}

// returns true if demo context could be parsed
qboolean ParseDemoContext( demoEntry_t *demo ) {
	// clear any existing state
	demoContext_t *oldCtx;
	oldCtx = ctx;
	ctx = &demo->cctx->ctx;
	// load initial state from demo
	msg_t *msg;
	if ( ( msg = ReadNextMessage( demo ) ) != nullptr ) {
		ctx = oldCtx;
		FreeMsg( msg );
		return qtrue;
	}
	ctx = oldCtx;
	return qfalse;
}

int ParseTime( char *timeStr ) {
	int time = -1;
	if ( strlen( timeStr ) != 12 ) {
		printf( "Invalid time format %s.  Must be hh:mm:ss.sss\n", timeStr );
		return -1;
	}
	timeStr[2] = timeStr[5]  = timeStr[8] = '\0';
	time = atoi( timeStr );
	time = ( time * 60 ) + atoi( &timeStr[3] );
	time = ( time * 60 ) + atoi( &timeStr[6] );
	time = ( time * 1000 ) + atoi( &timeStr[9] );
	return time;
}


int CG_InClientBitflags(entityState_t *ent, int client)
{
	int checkIn;
	int sub = 0;

	if (client > 47)
	{
		checkIn = ent->trickedentindex4;
		sub = 48;
	}
	else if (client > 31)
	{
		checkIn = ent->trickedentindex3;
		sub = 32;
	}
	else if (client > 15)
	{
		checkIn = ent->trickedentindex2;
		sub = 16;
	}
	else
	{
		checkIn = ent->trickedentindex;
	}

	if (checkIn & (1 << (client-sub)))
	{
		return 1;
	}

	return 0;
}

qboolean BG_InKnockDownOnly( int anim )
{
	switch ( anim )
	{
	case BOTH_KNOCKDOWN1:
	case BOTH_KNOCKDOWN2:
	case BOTH_KNOCKDOWN3:
	case BOTH_KNOCKDOWN4:
	case BOTH_KNOCKDOWN5:
		return qtrue;
	}
	return qfalse;
}

typedef struct netField_s {
	const char* name;
	size_t	offset;
	int		bits;		// 0 = float
#ifndef FINAL_BUILD
	unsigned	mCount;
#endif
} netField_t;
extern netField_t entityStateFields[132];

typedef struct {
	demoContext_t *ctx;
	FILE *outFile;
	int framesSaved;
	int serverCommandOffset;
	int numHandledGamestates;
	qboolean seqInitialized;
	qboolean done;
} splitDemoState_t;

static combinedDemoContext_t mergedCtx;
combinedDemoContext_t* cctx = &mergedCtx;
// bitmask of which clients had this ent in cur/prev snaps.
int RunSplit(const char *inFile, int demoIdx, const char *outFilename)
{
	cl_shownet->integer = 0;
	configStringOverrideDataLen = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");

	memset( &entry, 0, sizeof( demoEntry_t ) );
	{ // debugger fucks up idx without this
		Q_strncpyz( entry.filename, inFile, sizeof( entry.filename ) );
		FS_FOpenFileRead( entry.filename, &entry.fp, qfalse );
		if ( !entry.fp ) {
			printf( "Failed to open file %s\n", entry.filename );
			return -1;
		}
		entry.cctx = &mergedCtx; //(demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
		entry.ctx = NULL;
		entry.currentMap = 0;
		entry.eos = qfalse;
		entry.serverCommandSyncIdx = 0;
		//entry.ctx->clc.clientNum = clientnum;
	}

	qboolean demoFinished = qfalse;
	Com_Memset( &mergedCtx, 0, sizeof( mergedCtx ) );
	// Com_Memset( mergedCtx.serverReliableAcknowledge, -1, sizeof( mergedCtx.serverReliableAcknowledge ) ); // so that it starts at 0 when unknown
	if ( !ParseDemoContext( &entry ) ) {
		printf( "Failed to parse demo context for demo %s\n", entry.filename );
		return -1;
	}

	if ( demoIdx == -1 ) {
		ctx = &mergedCtx.ctx;
		msg_t *msg;
		while ( ( msg = ReadNextMessage( &entry ) ) != nullptr ) {
			FreeMsg( msg );
		}
		for ( int idx = 0; idx < cctx->numDemos; idx++ ) {
			demoMetadata_t *demo = &cctx->demos[idx];
			Com_Printf( "Demo %d: client %d, filename %s, mtime %d\n", idx, demo->clientnum, demo->filename, demo->fileMtime );
		}
		free( cctx->demos );
		FS_FCloseFile( entry.fp );
		return 0;
	}

	int numInitializedDemos = 0;
	splitDemoState_t *states = NULL;

	auto initNewDemos = [&]() -> int {
		if ( cctx->numDemos <= numInitializedDemos ) {
			return 0;
		}
		states = (splitDemoState_t *) realloc( states, cctx->numDemos * sizeof( splitDemoState_t ) );
		Com_Memset( &states[numInitializedDemos], 0, ( cctx->numDemos - numInitializedDemos ) * sizeof( splitDemoState_t ) );
		for ( int d = numInitializedDemos; d < cctx->numDemos; d++ ) {
			if ( demoIdx != -2 && d != demoIdx ) {
				states[d].done = qtrue;
				continue;
			}
			demoMetadata_t* demo = &cctx->demos[d];
			Com_Printf( "Found demo: %s\n", demo->filename );
			int clientnum = demo->clientnum;
			states[d].ctx = (demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
			states[d].ctx->clc.clientNum = clientnum;
			states[d].ctx->cl.gameState = mergedCtx.ctx.cl.gameState;
			memcpy( states[d].ctx->cl.entityBaselines, mergedCtx.ctx.cl.entityBaselines, sizeof( mergedCtx.ctx.cl.entityBaselines ) );
			states[d].ctx->clc.checksumFeed = mergedCtx.ctx.clc.checksumFeed;

			const char *curOutFilename = outFilename;
			char splitPathBuf[MAX_OSPATH];
			if ( demoIdx == -2 ) {
				const char *dir = ( outFilename && *outFilename ) ? outFilename : ".";
				Com_sprintf( splitPathBuf, sizeof( splitPathBuf ), "%s/split_%d.dm_26", dir, d );
				curOutFilename = splitPathBuf;
			} else if ( !curOutFilename || !*curOutFilename ) {
				curOutFilename = demo->filename;
			}

			FILE *outFile;
			if ( !Q_strncmp( curOutFilename, "-", 2 ) ) {
				outFile = stdout;
#ifdef WIN32
				setmode(fileno(stdout), O_BINARY);
#else
				//freopen( NULL, "wb", stdout );
    // apparently this isn't necessary on linux :?
#endif
			} else {
				outFile = fopen( curOutFilename, "wb" );
			}
			if ( !outFile ) {
				printf( "Couldn't open output file\n" );
				return -1;
			}
			states[d].outFile = outFile;
		}
		numInitializedDemos = cctx->numDemos;
		return 0;
	};

	if ( initNewDemos() != 0 ) {
		return -1;
	}

	ctx = &mergedCtx.ctx;
	int firstMissingFrame = -1;
	json_t *missingFrames = json_array();
	while ( true ) {
		if ( initNewDemos() != 0 ) {
			return -1;
		}
		for ( int d = 0; d < numInitializedDemos; d++ ) {
		if ( states[d].done ) {
			continue;
		}
		demoMetadata_t* demo = &cctx->demos[d];
		if ( demo->eos || demo->firstFrameTime == -1 ) {
			if ( demo->tailLen != 0 ) {
				if ( demo->tailLen > 0 && demo->tail != NULL ) {
					fwrite( demo->tail, demo->tailLen, 1, states[d].outFile );
				}
				free( demo->tail );
				demo->tail = NULL;
			}
			states[d].done = qtrue;
			continue;
		}
		int clientnum = demo->clientnum;
		entry.ctx = states[d].ctx;
		FILE *outFile = states[d].outFile;
		int &framesSaved = states[d].framesSaved;
		int &serverCommandOffset = states[d].serverCommandOffset;

		if ( mergedCtx.ctx.cl.snap.serverTime < demo->firstFrameTime || !(cctx->matchedClients & ( 1 << clientnum )) ) {
			continue;
		}
		//// find frame time
		//int frameTime = (int) (((unsigned int) -1) >> 1);//MAXINT;
		//int frameIdx = -1;
		//for ( idx = 0; idx < numDemos; idx++ ) {
		//	if ( entryList[idx].ctx->cl.snap.serverTime < frameTime && !entryList[idx].eos ) {
		//		frameTime = entryList[idx].ctx->cl.snap.serverTime;
		//		frameIdx = idx;
		//	}
		//}
		//if ( frameIdx == -1 ) {
		//	ctx = &mergedCtx.ctx;
		//	if (firstMissingFrame != -1) {
		//		int curFrame = getCurrentTime();
		//		int missingLength = curFrame - firstMissingFrame;
		//		if (missingLength >= 500) {
		//			json_t *missingFrameRange = json_array();
		//			json_array_append( missingFrameRange, json_integer( firstMissingFrame ) );
		//			json_array_append( missingFrameRange, json_integer( curFrame ) );
		//			json_array_append( missingFrames, missingFrameRange );
		//		}
		//		firstMissingFrame = -1;
		//	}
		//	break;
		//	/*printf( "ERROR: couldn't find any frame\n" );
		//	return -1;*/
		//}
		//// find all demos that got this frame
		//mergedCtx.numMatches = 0;
		//Com_Memset( mergedCtx.matches, 0, sizeof( mergedCtx.matches ) );
		//mergedCtx.matchedClients = 0;
		//Com_Memset( mergedCtx.deltasnap, 0, sizeof( mergedCtx.deltasnap ) );
		////int numNonMatches = 0;
		////int nonMatches[MAX_CLIENTS];
		//for ( idx = 0; idx < numDemos; idx++ ) {
		//	if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos ) {
		//		mergedCtx.matches[mergedCtx.numMatches++] = idx;
		//		int clientNum = entryList[idx].ctx->clc.clientNum;
		//		mergedCtx.matchedClients |= 1 << clientNum;
		//		int deltaNum = entryList[idx].ctx->cl.snap.deltaNum;
		//		int messageNum = entryList[idx].ctx->cl.snap.messageNum;
		//		mergedCtx.deltasnap[clientNum] = deltaNum == -1 ? 0 : messageNum - deltaNum;
		//		int psIdx = mergedCtx.curPlayerStateIdxMask & ( 1 << clientNum ) ? 1 : 0;
		//		mergedCtx.playerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.ps;
		//		if ( mergedCtx.playerStates[psIdx][clientNum].m_iVehicleNum ) {
		//			mergedCtx.vehPlayerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.vps;
		//		}
		//		mergedCtx.playerStateValidMask |= mergedCtx.curPlayerStateIdxMask & ( 1 << clientNum );
		//		mergedCtx.curPlayerStateIdxMask ^= ( 1 << clientNum );
		//	}/* else if ( entryList[idx].ctx->cl.snap.serverTime > frameTime && !entryList[idx].eos ) {
		//		// dropped frame, reuse last frame for .5s to smooth blips
		//		if ( entryList[idx].ctx->cl.snap.serverTime <= frameTime + 500 ) {
		//			nonMatches[numNonMatches++] = idx;
		//		}
		//	}*/
		//}
		//// numMatches must always be >0
		//ctx = entryList[mergedCtx.matches[0]].ctx;
		//// note: below advancing commented out because we still need to run through the server commands
		//// to pick up updated configstrings
		///*if ( !playerActive( clientNum ) ) {
		//	// commented out the other block since players usually reconnect, doesn't hurt to keep going and see
		//	//if ( framesSaved == 0 ) {
		//		// player didn't connect yet
		//		//continue;
		//		//printf("player not in configstrings at frame %d\n", frameTime);
		//		goto advanceLoop;
		//	//} else {
		//		// wrote out some frames so player disconnected
		//	//	break;
		//	//}
		//}
		//if ( framesSaved == 0 && getPlayerTeam( clientNum ) == TEAM_SPECTATOR ) {
		//	// player didn't enter game yet
		//	//printf("player not in game at frame %d\n", frameTime);
		//	goto advanceLoop;
		//}*/

		//ctx = &mergedCtx.ctx;
		//entry.ctx->clc.serverMessageSequence++;
		{
		if ( !states[d].seqInitialized ) {
			entry.ctx->cl.gameState = mergedCtx.ctx.cl.gameState;
			if ( demo->lastGamestate ) {
				entry.ctx->clc.lastExecutedServerCommand = entry.ctx->clc.serverCommandSequence = demo->lastGamestate->serverReliableAcknowledge + 1;  // TODO: right offset,  entry.firstServerCommand;
				entry.ctx->serverReliableAcknowledge = demo->lastGamestate->serverReliableAcknowledge;
				entry.ctx->clc.serverCommandSequence--;
				entry.ctx->clc.serverMessageSequence = demo->lastGamestate->serverMessageSequence;
			}
			states[d].seqInitialized = qtrue;
		}
		entry.ctx->clc.serverMessageSequence = cctx->serverMessageSequence[clientnum];
		////ctx->clc.reliableAcknowledge; // this doesn't need to be bumped
		int raIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << clientnum ) ? 1 : 0;
		entry.ctx->clc.reliableAcknowledge = cctx->reliableAcknowledge[raIdx ^ 1][clientnum];
		entry.ctx->clc.reliableSequence = entry.ctx->clc.reliableAcknowledge;

		// figure out commands
		int firstServerCommand = entry.ctx->clc.serverCommandSequence + 1;
		for ( int commandNum = entry.firstServerCommand; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
			char *command = ctx->clc.serverCommands[ commandNum & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			int bitmask = cctx->serverCommandBitmask[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
			if ( !( bitmask & ( 1 << clientnum ) ) ) {
				continue;
			}
			entry.ctx->clc.serverCommandSequence++;
			entry.ctx->clc.lastExecutedServerCommand++;
			Q_strncpyz( entry.ctx->clc.serverCommands[entry.ctx->clc.serverCommandSequence & ( MAX_RELIABLE_COMMANDS - 1 )], command, MAX_STRING_CHARS );
		}

		char bigConfigString[BIG_INFO_STRING];

		for ( int idx = states[d].numHandledGamestates; idx < cctx->numGamestates; idx++, states[d].numHandledGamestates++ ) {
			if ( cctx->gamestates[idx].demoIdx != d ) {
				continue;
			}
			gamestateMetadata_t* gamestateMetadata = &cctx->gamestates[idx];

			ctx = entry.ctx;
			// update any configstrings up to the gamestate's command sequence number
			for ( int commandNum = firstServerCommand; commandNum <= gamestateMetadata->serverCommandSequence; commandNum++, firstServerCommand++ ) {
				char* command = entry.ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
				if ( !( ( command[0] == 'c' && command[1] == 's' ) || ( command[0] == 'b' && command[1] == 'c' && command[2] == 's' ) ) ) {
					continue;
				}
				Cmd_TokenizeString( command );
				char* cmd = Cmd_Argv( 0 );
				if ( !strcmp( cmd, "bcs0" ) ) {
					Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv( 1 ), Cmd_Argv( 2 ) );
				}
				else if ( !strcmp( cmd, "bcs1" ) ) {
					char* s = Cmd_Argv( 2 );
					if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
						Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
					}
					strcat( bigConfigString, s );
				}
				else if ( !strcmp( cmd, "bcs2" ) ) {
					char* s = Cmd_Argv( 2 );
					if ( strlen( bigConfigString ) + strlen( s ) + 1 >= BIG_INFO_STRING ) {
						Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
					}
					strcat( bigConfigString, s );
					strcat( bigConfigString, "\"" );
					command = bigConfigString;
					Cmd_TokenizeString( command );
					cmd = Cmd_Argv( 0 );
				}
				if ( framesSaved > 0 && !strcmp( cmd, "cs" ) ) {
					CL_ConfigstringModified();
				}
			}

			if ( framesSaved == 0 && gamestateMetadata->numConfigStringOverrides > 0 ) {
				// handle any overridden configstrings
				for ( int i = 0; i < gamestateMetadata->numConfigStringOverrides; i++ ) {
					int num = gamestateMetadata->configStringOverrideIndex[i];
					char *str = gamestateMetadata->configStringOverride[i];
					Cmd_TokenizeString( va( "cs %d \"%s\"\n", num, str ) );
					CL_ConfigstringModified();
				}
			}
			// write header
			memcpy( entry.ctx->cl.entityBaselines, cctx->ctx.cl.entityBaselines, sizeof( cctx->ctx.cl.entityBaselines ) );
			entry.ctx->clc.checksumFeed = cctx->ctx.clc.checksumFeed;
			entry.ctx->messageExtraByte = gamestateMetadata->messageExtraByte;
			ctx = entry.ctx;
			ctx->clc.serverMessageSequence = gamestateMetadata->serverMessageSequence + 1;
			ctx->clc.reliableAcknowledge = gamestateMetadata->reliableAcknowledge;
			ctx->clc.reliableSequence = ctx->clc.reliableAcknowledge;
			writeDemoHeaderWithServerCommands( outFile, gamestateMetadata->serverReliableAcknowledge, gamestateMetadata->serverCommandSequence, serverCommandOffset );
			ctx->clc.serverMessageSequence = cctx->serverMessageSequence[clientnum];
			ctx->clc.reliableAcknowledge = cctx->reliableAcknowledge[raIdx ^ 1][clientnum];
			ctx->clc.reliableSequence = ctx->clc.reliableAcknowledge;
			ctx = &cctx->ctx;
		}

		ctx = entry.ctx;
		for ( int commandNum = firstServerCommand; commandNum <= entry.ctx->clc.serverCommandSequence; commandNum++ ) {
			char* command = entry.ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
			if ( !( ( command[0] == 'c' && command[1] == 's' ) || ( command[0] == 'b' && command[1] == 'c' && command[2] == 's' ) ) ) {
				continue;
			}
			Cmd_TokenizeString( command );
			char* cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "bcs0" ) ) {
				Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv( 1 ), Cmd_Argv( 2 ) );
			}
			else if ( !strcmp( cmd, "bcs1" ) ) {
				char* s = Cmd_Argv( 2 );
				if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
					Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
				}
				strcat( bigConfigString, s );
			}
			else if ( !strcmp( cmd, "bcs2" ) ) {
				char* s = Cmd_Argv( 2 );
				if ( strlen( bigConfigString ) + strlen( s ) + 1 >= BIG_INFO_STRING ) {
					Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
				}
				strcat( bigConfigString, s );
				strcat( bigConfigString, "\"" );
				command = bigConfigString;
				Cmd_TokenizeString( command );
				cmd = Cmd_Argv( 0 );
			}
			if ( !strcmp( cmd, "cs" ) ) {
				CL_ConfigstringModified();
			}
		}
		ctx = &cctx->ctx;

		//int firstServerCommand = ctx->clc.lastExecutedServerCommand;
		//for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
		//	int curCommand = firstServerCommand;
		//	idx = mergedCtx.matches[matchIdx];
		//	demoContext_t* entryCtx = entryList[idx].ctx;
		//	for ( int commandNum = entryList[idx].firstServerCommand; commandNum <= entryCtx->clc.serverCommandSequence; commandNum++ ) {
		//		char *command = entryCtx->clc.serverCommands[ commandNum & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		//		Cmd_TokenizeString( command );
		//		char *cmd = Cmd_Argv( 0 );
		//		if ( cmd[0] == '\0' )
		//			continue;
		//		// search for place to insert it
		//		bool newCommand = qtrue;
		//		int start = entryList[idx].serverCommandSyncIdx;
		//		if ( start == 0 ) {
		//			// normally start at sync idx even if it's in a previous frame, to account for frame drops
		//			start = firstServerCommand;
		//		}
		//		for ( int snapCmdNum = start; snapCmdNum <= ctx->clc.serverCommandSequence; snapCmdNum++ ) {
		//			if ( !Q_stricmpn( command, ctx->clc.serverCommands[ snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 ) ], MAX_STRING_CHARS ) ) {
		//				newCommand = qfalse;
		//				mergedCtx.serverCommandBitmask[snapCmdNum % MAX_RELIABLE_COMMANDS] |= 1 << entryList[idx].ctx->clc.clientNum;
		//				curCommand = snapCmdNum + 1;
		//				entryList[idx].serverCommandSyncIdx = curCommand;
		//				break;
		//			}
		//		}
		//		if ( newCommand ) {
		//			// insert it at index curCommand
		//			ctx->clc.serverCommandSequence++;
		//			ctx->clc.lastExecutedServerCommand++;
		//			for ( int snapCmdNum = ctx->clc.serverCommandSequence; snapCmdNum > curCommand; snapCmdNum-- ) {
		//				char *dest = ctx->clc.serverCommands[ snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		//				char *src = ctx->clc.serverCommands[ ( snapCmdNum - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		//				memmove( dest, src, MAX_STRING_CHARS );
		//				int* maskDest = &mergedCtx.serverCommandBitmask[snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 )];
		//				int* maskSrc = &mergedCtx.serverCommandBitmask[( snapCmdNum - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 )];
		//				*dest = *src;
		//			}
		//			Q_strncpyz( ctx->clc.serverCommands[ curCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ], command, MAX_STRING_CHARS );
		//			mergedCtx.serverCommandBitmask[curCommand & ( MAX_RELIABLE_COMMANDS - 1 )] = 1 << entryList[idx].ctx->clc.clientNum;
		//			curCommand++;
		//			entryList[idx].serverCommandSyncIdx = curCommand;
		//			//printf( "idx:%d cmd:%d Command: %s\n", idx, commandNum, command );
		//		}
		//	}
		//}
		//for ( int commandNum = firstServerCommand; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
		//	char *command = ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
		//	Cmd_TokenizeString( command );
		//	char *cmd = Cmd_Argv( 0 );
		//	if ( !strcmp( cmd, "cs" ) ) {
		//		// needed so util funcs can work against mergedCtx
		//		CL_ConfigstringModified();
		//		/*if ( atoi(Cmd_Argv( 1 )) == 1 ) {
		//			Com_Printf( "Server id: %s\n", Info_ValueForKey( Cmd_ArgsFrom( 2 ), "sv_serverid" ) );
		//		}*/
		//	}
		//}

		//// make a new snap directly in entry.ctx->cl.snap
		clSnapshot_t &snap = entry.ctx->cl.snap;
		snap.parseEntitiesNum = entry.ctx->cl.parseEntitiesNum;
		snap.snapFlags = cctx->snapFlags[clientnum];

		// generate snap.  follows similar logic in sv_snapshot.cc:SV_BuildClientSnapshot
		{
			clSnapshot_t *frame = &snap;
			int32_t *owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];
			int clientBit = 1 << clientnum;

			frame->numEntities = 0;
			for ( int entIdx = ctx->cl.snap.parseEntitiesNum; entIdx < ctx->cl.snap.parseEntitiesNum + ctx->cl.snap.numEntities; entIdx++ ) {
				int parseIdx = entIdx & ( MAX_PARSE_ENTITIES - 1 );
				entityState_t *ent = ctx->parseEntityPtr[parseIdx];
				if ( !( owners[ent->number] & clientBit ) ) {
					continue;
				}
				int dstIdx = entry.ctx->cl.parseEntitiesNum & ( MAX_PARSE_ENTITIES - 1 );
				entityState_t *floatForcedState = &entry.ctx->parseEntitiesFloatForced[dstIdx];
				entry.ctx->cl.parseEntitiesNum++;
				entry.ctx->parseEntityPtr[dstIdx] = ent;
				entry.ctx->parseEntityVersion[dstIdx] = ctx->parseEntityVersion[parseIdx];
				entityState_t *srcFloatForced = &ctx->parseEntitiesFloatForced[parseIdx];
				if ( srcFloatForced->number & clientBit ) {
					floatForcedState->number = 1;
					for ( int i = 0; i < (int) ARRAY_LEN( entityStateFields ); i++ ) {
						size_t offset = entityStateFields[i].offset;
						int val = *(int*) ( (byte*) srcFloatForced + offset );
						*(int*) ( (byte*) floatForcedState + offset ) = ( val & clientBit ) ? 1 : 0;
					}
				} else if ( floatForcedState->number ) {
					Com_Memset( floatForcedState, 0, sizeof( *floatForcedState ) );
				}
				if ( entry.ctx->cl.parseEntitiesNum >= 0x7FFFFFFE ) {
					Com_Error( ERR_FATAL, "entry.ctx->cl.parseEntitiesNum wrapped" );
				}
				frame->numEntities++;
			}

			int psIdx = cctx->curPlayerStateIdxMask & clientBit ? 1 : 0;
			snap.ps = cctx->playerStates[psIdx][clientnum];
			const playerState_t &srcPsff = cctx->playerStatesForcedFields[psIdx][clientnum];
			playerState_t &dstPsff = entry.ctx->playerStateForcedFields[entry.ctx->clc.serverMessageSequence & PACKET_MASK];
			if ( srcPsff.ping ) {
				dstPsff = srcPsff;
			} else if ( dstPsff.ping ) {
				Com_Memset( &dstPsff, 0, sizeof( dstPsff ) );
			}
			if ( snap.ps.m_iVehicleNum ) {
				snap.vps = cctx->vehPlayerStates[psIdx][clientnum];
				const playerState_t &srcVpsff = cctx->vehPlayerStatesForcedFields[psIdx][clientnum];
				playerState_t &dstVpsff = entry.ctx->vehPlayerStateForcedFields[entry.ctx->clc.serverMessageSequence & PACKET_MASK];
				if ( srcVpsff.ping ) {
					dstVpsff = srcVpsff;
				} else if ( dstVpsff.ping ) {
					Com_Memset( &dstVpsff, 0, sizeof( dstVpsff ) );
				}
			}
		}

		snap.messageNum = entry.ctx->clc.serverMessageSequence;
		snap.serverCommandNum = entry.ctx->clc.serverCommandSequence;
		snap.valid = qtrue;
		snap.serverTime = ctx->cl.snap.serverTime;
		snap.deltaNum = ( cctx->deltasnap[clientnum] & 0x7f ) == 0 ? -1 : snap.messageNum - ( cctx->deltasnap[clientnum] & 0x7f );
		if ( snap.deltaNum != -1 && !( cctx->deltasnap[clientnum] & 0x80 ) ) {
			const clSnapshot_t &deltaSnap = entry.ctx->cl.snapshots[snap.deltaNum & PACKET_MASK];
			firstServerCommand = deltaSnap.serverCommandNum + 1;
		} else {
			int raIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << clientnum ) ? 0 : 1;
			firstServerCommand = cctx->serverReliableAcknowledge[raIdx][clientnum] + 1;
		}
		Com_Memcpy( snap.areamask, cctx->areamask[clientnum], ctx->areabytes );
		entry.ctx->areabytes = ctx->areabytes;

		//// copy new snap into backup array
		CopyClSnapshot( &entry.ctx->cl.snapshots[snap.messageNum & PACKET_MASK], &snap );

		entry.ctx->messageExtraByte = cctx->messageExtraByte[clientnum];

		if ( framesSaved > 0 ) {
			ctx = entry.ctx;
			writeDeltaSnapshot( firstServerCommand, outFile, qfalse, serverCommandOffset );
			ctx = &cctx->ctx;
			framesSaved++;
		} else {
			ctx = entry.ctx;
			writeDeltaSnapshot( firstServerCommand, outFile, qtrue, serverCommandOffset );
			ctx = &cctx->ctx;
			// copy rest
			framesSaved = 1;
		}
		if ( framesSaved >= 7 ) {
			//break;
		}


		//printf( "Wrote frame at time %d [%d:%02d.%04d]\n", ctx->cl.snap.serverTime, getCurrentTime() / 1000 / 60, (getCurrentTime() / 1000) % 60, getCurrentTime() % 1000 );
		/*printf( "Wrote frame at time %d clients ", ctx->cl.snap.serverTime );
		for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
			printf( "%d ", matches[matchIdx] );
		}
		printf( "\n" );*/
		}
		}

		// update configstrings on mergedCtx for any demos that join on subsequent frames
		{
			char bigConfigString[BIG_INFO_STRING];
			ctx = &mergedCtx.ctx;
			for ( int commandNum = entry.firstServerCommand; commandNum <= mergedCtx.ctx.clc.serverCommandSequence; commandNum++ ) {
				char* command = mergedCtx.ctx.clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
				if ( !( ( command[0] == 'c' && command[1] == 's' ) || ( command[0] == 'b' && command[1] == 'c' && command[2] == 's' ) ) ) {
					continue;
				}
				Cmd_TokenizeString( command );
				char* cmd = Cmd_Argv( 0 );
				if ( !strcmp( cmd, "bcs0" ) ) {
					Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv( 1 ), Cmd_Argv( 2 ) );
				}
				else if ( !strcmp( cmd, "bcs1" ) ) {
					char* s = Cmd_Argv( 2 );
					if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
						Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
					}
					strcat( bigConfigString, s );
				}
				else if ( !strcmp( cmd, "bcs2" ) ) {
					char* s = Cmd_Argv( 2 );
					if ( strlen( bigConfigString ) + strlen( s ) + 1 >= BIG_INFO_STRING ) {
						Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
					}
					strcat( bigConfigString, s );
					strcat( bigConfigString, "\"" );
					command = bigConfigString;
					Cmd_TokenizeString( command );
					cmd = Cmd_Argv( 0 );
				}
				if ( !strcmp( cmd, "cs" ) ) {
					CL_ConfigstringModified();
				}
			}
		}

advanceLoop:
		// for all demos that have this frame, advance to the next frame.
		if ( !entry.eos ) {
			ctx = &cctx->ctx;
			msg_t *msg = ReadNextMessage( &entry );
			if ( initNewDemos() != 0 ) {
				return -1;
			}
			for ( int d = 0; d < numInitializedDemos; d++ ) {
				demoMetadata_t *demo = &cctx->demos[d];
				if ( !states[d].done ) {
					if ( demo->tailLen != 0 ) {
						if ( demo->tailLen > 0 && demo->tail != NULL ) {  // < 0 signals no tail, == 0 signals -1 -1 tail.
							fwrite( demo->tail, demo->tailLen, 1, states[d].outFile );
						}
						free( demo->tail );
						demo->tail = NULL;
						// demo->tailLen = 0;
					}
					if ( msg == nullptr || demo->eos ) {
						states[d].done = qtrue;
					}
				}
			}
			if ( msg != nullptr ) {
				FreeMsg( msg );
			}
			qboolean shouldStop = ( msg == nullptr ) ? qtrue : qfalse;
			if ( demoIdx >= 0 && demoIdx < numInitializedDemos && states[demoIdx].done ) {
				shouldStop = qtrue;
			}
			if ( shouldStop ) {
				entry.eos = qtrue;
				break;
			}
		}
	}

	if ( demoIdx >= 0 && cctx->numDemos <= demoIdx ) {
		Com_Error( ERR_FATAL, "Demo %d exceeds numDemos %d!\n", demoIdx, cctx->numDemos );
	}

	for ( int d = 0; d < numInitializedDemos; d++ ) {
		if ( !states[d].outFile ) {
			continue;
		}
		demoMetadata_t *demo = &cctx->demos[d];
		if ( demo->tailLen == 0 ) {
			int len = -1;
			fwrite( &len, 4, 1, states[d].outFile );
			fwrite( &len, 4, 1, states[d].outFile );
		}
		free( states[d].ctx );
		fclose( states[d].outFile );
	}
	free( states );
	free( cctx->demos );
	FS_FCloseFile( entry.fp );

	FILE *metaFile;
	if ( Q_strncmp( outFilename, "-", 2 ) ) {
		metaFile = stdout;
	} else {
		//metaFile = fopen( va( "%s.dm_meta", filename ), "wb" );
		metaFile = NULL;
	}
	if ( metaFile ) {
		json_dumpf( missingFrames, metaFile, JSON_INDENT(2) | JSON_PRESERVE_ORDER );
		fclose( metaFile );
	}

	//system("PAUSE");

	return 0;
}

int main(int argc, char** argv)
{
	if ( argc < 3 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" infile.dm_26 clientnum demo.dm_26\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	int clientnum = atoi(argv[2] );
	const char *inFile = argv[1];
	const char *outFile = argc > 3 ? argv[3] : "";
	RunSplit( inFile, clientnum, outFile );
}
