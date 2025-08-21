// demo_combiner.cpp : Defines the entry point for the console application.
//

#include "deps.h"
#include "client/client.h"
#include "combined_demo_utils.h"
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
	demoContext_t *ctx;
	int firstServerCommand;
	int framesSaved;
	int currentMap;
	qboolean eos;
	// for server commands sent in the gamestate message, since we only loop on snaps
	int gamestateServerReliableAcknowledge;
	int gamestateServerCommandSequence;
	// idx of merged server command seq where the last server cmd from this demo was
	int serverCommandSyncIdx;
} demoEntry_t;

void FreeMsg( msg_t *msg ) {
	free( msg->data );
	free( msg );
}

// returns demoFinished
msg_t *ReadNextMessageRaw( demoEntry_t *demo ) {
	msg_t *msg = (msg_t *) calloc( 1, sizeof( msg_t ) );
	byte *msgData = (byte *) calloc( MAX_MSGLEN, 1 );
	MSG_Init( msg, msgData, MAX_MSGLEN );
	if ( !CL_ReadDemoMessage( demo->fp, msg ) ) {
		return nullptr;
	}
	return msg;
}

msg_t *ReadNextMessage( demoEntry_t *demo ) {
	msg_t *msg;
	int entityEventSequence = 0;
	if ( ctx->cl.snap.valid ) {
		entityEventSequence = ctx->cl.snap.ps.entityEventSequence;
	}
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		try {
			CL_ParseServerMessage( msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			FreeMsg( msg );
			return nullptr;
		}

		if ( !ctx->cl.newSnapshots ) {
			demo->gamestateServerReliableAcknowledge = ctx->serverReliableAcknowledge;
			demo->gamestateServerCommandSequence = ctx->clc.serverCommandSequence;
			cctx->initialMessageExtraByte[ctx->clc.clientNum] = ctx->messageExtraByte;
			FreeMsg( msg );
			continue;
		}
		ctx->cl.snap.ps.entityEventSequence = entityEventSequence;
		if ( lastSnapValid && ( ( lastSnapFlags ^ ctx->cl.snap.snapFlags ) & SNAPFLAG_SERVERCOUNT ) != 0 ) {
			demo->currentMap++;
		}
		if ( ctx->clc.lastExecutedServerCommand == 0 && demo->gamestateServerCommandSequence > 0 ) {
			ctx->clc.lastExecutedServerCommand = demo->gamestateServerReliableAcknowledge + 1;
		}
		if ( ctx->clc.serverCommandSequence - ctx->clc.lastExecutedServerCommand > MAX_RELIABLE_COMMANDS ) {
			ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10; // fudge factor
		}
		demo->firstServerCommand = ctx->clc.lastExecutedServerCommand;
		int firstNonEmptyCommand = 0;
		// process any new server commands
		for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
			char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( cmd[0] && firstNonEmptyCommand == 0 ) {
				firstNonEmptyCommand = ctx->clc.lastExecutedServerCommand;
			}
			if ( !strcmp( cmd, "cs" ) ) {
				//CL_ConfigstringModified();
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
	ctx = demo->ctx;
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


typedef struct entityAndFloat_s {
	entityState_t* ent;
	entityState_t floatForced;
} entityAndFloat_t;

typedef struct snapshotEntityNumbers_s {
	int				numSnapshotEntities;
	entityAndFloat_t	snapshotEntities[MAX_SNAPSHOT_ENTITIES];
} snapshotEntityNumbers_t;

/*
=======================
SV_QsortEntityNumbers
=======================
*/
static int QDECL SV_QsortEntityNumbers( const void *a, const void *b ) {
	int ea, eb;

	ea = ((entityAndFloat_t*)a)->ent->number;
	eb = ((entityAndFloat_t*)b)->ent->number;

	if ( ea == eb ) {
		Com_Error( ERR_DROP, "SV_QsortEntityStates: duplicated entity" );
	}

	if ( ea < eb ) {
		return -1;
	}

	return 1;
}


/*
===============
SV_AddEntToSnapshot
===============
*/
typedef struct netField_s {
	const char* name;
	size_t	offset;
	int		bits;		// 0 = float
#ifndef FINAL_BUILD
	unsigned	mCount;
#endif
} netField_t;
extern netField_t entityStateFields[];
static void SV_AddEntToSnapshot( int sourceClient, entityState_t *ent, entityState_t *floatForcedEnt, snapshotEntityNumbers_t *eNums, qboolean overwrite = qfalse ) {
	// if we have already added this entity to this snapshot, don't add again
	/*if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
		return;
	}
	svEnt->snapshotCounter = sv.snapshotCounter;*/
	entityState_t *toFloatForced = NULL;
	for ( int idx = 0; idx < eNums->numSnapshotEntities; idx++ ) {
		if ( eNums->snapshotEntities[idx].ent->number == ent->number ) {
			if ( memcmp( eNums->snapshotEntities[idx].ent, ent, sizeof( *ent ) ) ) {
				// Com_Error( ERR_FATAL, "Entities differ!" );
				Com_Printf( "Entity %d differs between clients!\n", ent->number );
			}
			if ( overwrite ) {
				eNums->snapshotEntities[idx] = { ent };
			}
			toFloatForced = &eNums->snapshotEntities[idx].floatForced;
			break;
		}
	}

	// if we are full, silently discard entities
	if ( eNums->numSnapshotEntities == MAX_SNAPSHOT_ENTITIES ) {
		return;
	}

	if ( !toFloatForced ) {
		eNums->snapshotEntities[eNums->numSnapshotEntities] = { ent };
		toFloatForced = &eNums->snapshotEntities[eNums->numSnapshotEntities].floatForced;
		eNums->numSnapshotEntities++;
	}

	for ( int i = 0; i < ( sizeof( entityState_t ) / 4 ) - 1; i++ ) {
		netField_t* field = &entityStateFields[i];
		int* fromF = (int*) ( (byte*) floatForcedEnt + field->offset );
		int* toF = (int*) ( (byte*) toFloatForced + field->offset );
		if ( *fromF == 1 ) {
			*toF |= ( 1 << sourceClient );
		}
	}
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


extern bool sendFullNegativeZero;

static combinedDemoContext_t mergedCtx;
combinedDemoContext_t* cctx = &mergedCtx;
// bitmask of which clients had this ent in cur/prev snaps.
int RunMerge(char **demos, int numDemos, char *outFilename)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");

	demoEntry_t *entryList = (demoEntry_t *) calloc( numDemos, sizeof( demoEntry_t ) );
	{ // debugger fucks up idx without this
		for ( int idx = 0; idx < numDemos; idx++ ) {
			Q_strncpyz( entryList[idx].filename, demos[idx], sizeof( entryList[idx].filename ) );
			FS_FOpenFileRead( entryList[idx].filename, &entryList[idx].fp, qfalse );
			if ( !entryList[idx].fp ) {
				printf( "Failed to open file %s\n", entryList[idx].filename );
				return -1;
			}
			entryList[idx].ctx = (demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
			entryList[idx].currentMap = 0;
			entryList[idx].eos = qfalse;
			entryList[idx].serverCommandSyncIdx = 0;
		}
	}

	FILE *outFile;
	if ( !Q_strncmp( outFilename, "-", 2 ) ) {
		outFile = stdout;
#ifdef WIN32
		setmode(fileno(stdout), O_BINARY);
#else
		//freopen( NULL, "wb", stdout );
    // apparently this isn't necessary on linux :?
#endif
	} else {
		outFile = fopen( outFilename, "wb" );
	}
	if ( !outFile ) {
		printf( "Couldn't open output file\n" );
		return -1;
	}

	Com_Memset( &mergedCtx, 0, sizeof( mergedCtx ) );

	qboolean demoFinished = qfalse;
	int idx;
	for ( idx = 0; idx < numDemos; idx++ ) {
		if ( !ParseDemoContext( &entryList[idx] ) ) {
			printf( "Failed to parse demo context for demo %s, index %d\n", entryList[idx].filename, idx );
			return -1;
		}
	}
	// start with a base ctx
	// should be the earliest one to pick up earliest configstrings
	// note this could actually be wrong cuz sometimes serverTime resets on map restart
	{
		int minTime = entryList[0].ctx->cl.snap.serverTime;
		int minIdx = 0;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime < minTime ) {
				minTime = entryList[idx].ctx->cl.snap.serverTime;
				minIdx = idx;
			}
		}
		mergedCtx.ctx = *entryList[minIdx].ctx;
	}
	mergedCtx.ctx.clc.lastExecutedServerCommand = mergedCtx.ctx.clc.serverCommandSequence = 0; // entryList[0].firstServerCommand;
	mergedCtx.ctx.clc.serverCommandSequence--;
	mergedCtx.ctx.clc.clientNum = -1;
	ctx = &mergedCtx.ctx;
	ctx->clc.serverMessageSequence = 0;
	int serverCommandOffset = 0;
	int framesSaved = 0;
	int firstMissingFrame = -1;
	json_t *missingFrames = json_array();
	sendFullNegativeZero = true;
	while ( true ) {
		// find frame time
		int frameTime = (int) (((unsigned int) -1) >> 1);//MAXINT;
		int frameIdx = -1;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime < frameTime && !entryList[idx].eos ) {
				frameTime = entryList[idx].ctx->cl.snap.serverTime;
				frameIdx = idx;
			}
			cctx->initialServerReliableAcknowledge[entryList[idx].ctx->clc.clientNum] = entryList[idx].ctx->serverReliableAcknowledge;
			cctx->initialServerMessageSequence[entryList[idx].ctx->clc.clientNum] = entryList[idx].ctx->clc.serverMessageSequence - 1;
			cctx->initialServerCommandSequence[entryList[idx].ctx->clc.clientNum] = entryList[idx].gamestateServerCommandSequence;
			cctx->initialServerReliableAcknowledgeMask |= ( 1 << entryList[idx].ctx->clc.clientNum );
		}
		if ( frameIdx == -1 ) {
			ctx = &mergedCtx.ctx;
			if (firstMissingFrame != -1) {
				int curFrame = getCurrentTime();
				int missingLength = curFrame - firstMissingFrame;
				if (missingLength >= 500) {
					json_t *missingFrameRange = json_array();
					json_array_append( missingFrameRange, json_integer( firstMissingFrame ) );
					json_array_append( missingFrameRange, json_integer( curFrame ) );
					json_array_append( missingFrames, missingFrameRange );
				}
				firstMissingFrame = -1;
			}
			break;
			/*printf( "ERROR: couldn't find any frame\n" );
			return -1;*/
		}
		// find all demos that got this frame
		mergedCtx.numMatches = 0;
		Com_Memset( mergedCtx.matches, 0, sizeof( mergedCtx.matches ) );
		mergedCtx.matchedClients = 0;
		Com_Memset( mergedCtx.deltasnap, 0, sizeof( mergedCtx.deltasnap ) );
		//int numNonMatches = 0;
		//int nonMatches[MAX_CLIENTS];
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos ) {
				mergedCtx.matches[mergedCtx.numMatches++] = idx;
				int clientNum = entryList[idx].ctx->clc.clientNum;
				mergedCtx.matchedClients |= 1 << clientNum;
				int deltaNum = entryList[idx].ctx->cl.snap.deltaNum;
				int messageNum = entryList[idx].ctx->cl.snap.messageNum;
				mergedCtx.deltasnap[clientNum] = deltaNum == -1 ? 0 : messageNum - deltaNum;
				int psIdx = mergedCtx.curPlayerStateIdxMask & ( 1 << clientNum ) ? 1 : 0;
				mergedCtx.playerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.ps;
				mergedCtx.playerStatesForcedFields[psIdx][clientNum] = entryList[idx].ctx->playerStateForcedFields[entryList[idx].ctx->cl.snap.messageNum & PACKET_MASK];
				if ( mergedCtx.playerStates[psIdx][clientNum].m_iVehicleNum ) {
					mergedCtx.vehPlayerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.vps;
					mergedCtx.vehPlayerStatesForcedFields[psIdx][clientNum] = entryList[idx].ctx->vehPlayerStateForcedFields[entryList[idx].ctx->cl.snap.messageNum & PACKET_MASK];
				}
				int raIdx = mergedCtx.reliableAcknowledgeIdxMask & ( 1 << clientNum ) ? 1 : 0;
				mergedCtx.serverReliableAcknowledge[raIdx][clientNum] = entryList[idx].ctx->serverReliableAcknowledge;
				mergedCtx.reliableAcknowledge[raIdx][clientNum] = entryList[idx].ctx->clc.reliableAcknowledge;
				mergedCtx.reliableAcknowledgeIdxMask ^= ( 1 << clientNum );
				mergedCtx.playerStateValidMask |= mergedCtx.curPlayerStateIdxMask & ( 1 << clientNum );
				mergedCtx.curPlayerStateIdxMask ^= ( 1 << clientNum );
				mergedCtx.lastServerMessageSequence[clientNum] = mergedCtx.serverMessageSequence[clientNum];
				mergedCtx.serverMessageSequence[clientNum] = entryList[idx].ctx->clc.serverMessageSequence;
				Com_Memcpy( mergedCtx.areamask[clientNum], entryList[idx].ctx->cl.snap.areamask, entryList[idx].ctx->areabytes );
				mergedCtx.ctx.areabytes = entryList[idx].ctx->areabytes;
				mergedCtx.messageExtraByte[clientNum] = entryList[idx].ctx->messageExtraByte;
			}/* else if ( entryList[idx].ctx->cl.snap.serverTime > frameTime && !entryList[idx].eos ) {
				// dropped frame, reuse last frame for .5s to smooth blips
				if ( entryList[idx].ctx->cl.snap.serverTime <= frameTime + 500 ) {
					nonMatches[numNonMatches++] = idx;
				}
			}*/
		}
		// numMatches must always be >0
		ctx = entryList[mergedCtx.matches[0]].ctx;
		// note: below advancing commented out because we still need to run through the server commands
		// to pick up updated configstrings
		/*if ( !playerActive( clientNum ) ) {
			// commented out the other block since players usually reconnect, doesn't hurt to keep going and see
			//if ( framesSaved == 0 ) {
				// player didn't connect yet
				//continue;
				//printf("player not in configstrings at frame %d\n", frameTime);
				goto advanceLoop;
			//} else {
				// wrote out some frames so player disconnected
			//	break;
			//}
		}
		if ( framesSaved == 0 && getPlayerTeam( clientNum ) == TEAM_SPECTATOR ) {
			// player didn't enter game yet
			//printf("player not in game at frame %d\n", frameTime);
			goto advanceLoop;
		}*/

		ctx = &mergedCtx.ctx;
		ctx->clc.serverMessageSequence++;
		//ctx->clc.reliableAcknowledge; // this doesn't need to be bumped

		// figure out commands
		int firstServerCommand = ctx->clc.lastExecutedServerCommand;
		int firstServerCommandToExecute = firstServerCommand;
		for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
			int curCommand = firstServerCommand;
			idx = mergedCtx.matches[matchIdx];
			demoContext_t* entryCtx = entryList[idx].ctx;
			for ( int commandNum = entryList[idx].firstServerCommand; commandNum <= entryCtx->clc.serverCommandSequence; commandNum++ ) {
				char* command = entryCtx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
				Cmd_TokenizeString( command );
				char* cmd = Cmd_Argv( 0 );
				if ( cmd[0] == '\0' )
					continue;
				// search for place to insert it
				bool newCommand = qtrue;
				int start = Q_max( firstServerCommand, entryList[idx].serverCommandSyncIdx );
				if ( start == 0 ) {
					// normally start at sync idx even if it's in a previous frame, to account for frame drops
					start = firstServerCommand;
				}
				for ( int snapCmdNum = start; snapCmdNum <= ctx->clc.serverCommandSequence; snapCmdNum++ ) {
					if ( !Q_stricmpn( command, ctx->clc.serverCommands[snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 )], MAX_STRING_CHARS ) ) {
						newCommand = qfalse;
						mergedCtx.serverCommandBitmask[snapCmdNum % MAX_RELIABLE_COMMANDS] |= 1 << entryList[idx].ctx->clc.clientNum;
						curCommand = snapCmdNum + 1;
						entryList[idx].serverCommandSyncIdx = curCommand;
						break;
					}
				}
				if ( newCommand ) {
					// insert it at index curCommand
					ctx->clc.serverCommandSequence++;
					ctx->clc.lastExecutedServerCommand++;
					if ( commandNum <= entryList[idx].gamestateServerCommandSequence && framesSaved == 0 ) {
						// don't execute this one, it arrived with the gamestate
						firstServerCommandToExecute++;
						if ( curCommand >= firstServerCommandToExecute ) {
							Com_Printf( "Command ordering is wrong!\n" );
						}
					}
					for ( int snapCmdNum = ctx->clc.serverCommandSequence; snapCmdNum > curCommand; snapCmdNum-- ) {
						{
							char* dest = ctx->clc.serverCommands[snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 )];
							char* src = ctx->clc.serverCommands[( snapCmdNum - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 )];
							memmove( dest, src, MAX_STRING_CHARS );
						}
						{
							int* maskDest = &mergedCtx.serverCommandBitmask[snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 )];
							int* maskSrc = &mergedCtx.serverCommandBitmask[( snapCmdNum - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 )];
							*maskDest = *maskSrc;
						}
					}
					Q_strncpyz( ctx->clc.serverCommands[curCommand & ( MAX_RELIABLE_COMMANDS - 1 )], command, MAX_STRING_CHARS );
					mergedCtx.serverCommandBitmask[curCommand & ( MAX_RELIABLE_COMMANDS - 1 )] = 1 << entryList[idx].ctx->clc.clientNum;
					curCommand++;
					entryList[idx].serverCommandSyncIdx = curCommand;
					//printf( "idx:%d cmd:%d Command: %s\n", idx, commandNum, command );
				}
			}
		}
		if ( framesSaved == 0 ) {
			// write header first before we update configstrings
			writeMergedDemoHeader( outFile );
		}
		for ( int commandNum = firstServerCommandToExecute; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
			char* command = ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
			Cmd_TokenizeString( command );
			char* cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "cs" ) ) {
				// needed so util funcs can work against mergedCtx
				CL_ConfigstringModified();
				/*if ( atoi(Cmd_Argv( 1 )) == 1 ) {
					Com_Printf( "Server id: %s\n", Info_ValueForKey( Cmd_ArgsFrom( 2 ), "sv_serverid" ) );
				}*/
			}
		}

		// make a new snap
		clSnapshot_t snap;
		memset( &snap, 0, sizeof( snap ) );
		snap.parseEntitiesNum = ctx->cl.parseEntitiesNum;
		snap.snapFlags = entryList[mergedCtx.matches[0]].ctx->cl.snap.snapFlags; // &SNAPFLAG_SERVERCOUNT;

		// generate snap.  follows similar logic in sv_snapshot.cc:SV_BuildClientSnapshot
		{
			clSnapshot_t				*frame = &snap;
			snapshotEntityNumbers_t		entityNumbers;
			// buffer to store ents generated from the original demos' ps.
			// save because they're copied out before this gets out of scope.
			//entityState_t				dentity[32];
			//memset(dentity, 0, sizeof(dentity));

			int32_t *owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];
			mergedCtx.ent_owner_idx ^= 1;
			int32_t *prev_owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];

			// clear everything in this snapshot
			entityNumbers.numSnapshotEntities = 0;
			Com_Memset( frame->areamask, 0, sizeof( frame->areamask ) );
			memset( owners, 0, sizeof( *owners ) * MAX_GENTITIES );

			frame->numEntities = 0;

			if ( frameTime == 228005 ) {
				Com_Printf( "Bad frame\n" );
			}

			int matchesMask = 0;
			for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
				idx = mergedCtx.matches[matchIdx];
				demoContext_t *dctx = entryList[idx].ctx;
				clSnapshot_t *dsnap = &dctx->cl.snap;

				cctx->snapFlags[entryList[idx].ctx->clc.clientNum] = dsnap->snapFlags;

				// add all the entities
				for ( int entIdx = dsnap->parseEntitiesNum; entIdx < dsnap->parseEntitiesNum + dsnap->numEntities; entIdx++ ) {
					entityState_t *ent = &dctx->cl.parseEntities[entIdx & (MAX_PARSE_ENTITIES - 1)];
					entityState_t *floatForced = &dctx->parseEntitiesFloatForced[entIdx & ( MAX_PARSE_ENTITIES - 1 )];
					SV_AddEntToSnapshot( dctx->clc.clientNum, ent, floatForced, &entityNumbers );
					owners[ent->number] |= 1 << entryList[idx].ctx->clc.clientNum;
				}
				//BG_PlayerStateToEntityStateExtraPolate( &dsnap->ps, &dentity[idx], dsnap->ps.commandTime, qfalse );
				// this pains me to do but it bugs the ui, which tries to display a health meter for these players
				//dentity[idx].health = 0;
				//TODO: this must be fixed.  events are duplicated this way.  instead this should first check if there were
				// any temp events already created for this player.  if so use those.  otherwise create a new temp event.
				//SV_AddEntToSnapshot( &dentity[idx], &entityNumbers, qtrue );
				//owners[dentity[idx].number] |= 1 << entryList[idx].ctx->clc.clientNum;

				/* TODO fix this */ /* if ( dsnap->ps.clientNum == clientNum ) */ {
					// one of the demos merging is actually the players demo, so ps can just be copied!
					snap.ps = dsnap->ps;
				}

				matchesMask |= 1 << entryList[idx].ctx->clc.clientNum;
			}

			// any ents that are missing from this snap but we had in the last snap, assume they are supposed to be gone
			/*for ( int entIdx = 0; entIdx < MAX_GENTITIES; entIdx++ ) {
				if ( (prev_owners[entIdx] & matchesMask) != 0 && owners[entIdx] == 0 ) {
					owners[entIdx] = (prev_owners[entIdx] & matchesMask);
				}
			}*/

			// if there were portals visible, there may be out of order entities
			// in the list which will need to be resorted for the delta compression
			// to work correctly.  This also catches the error condition
			// of an entity being included twice.
			qsort( entityNumbers.snapshotEntities, entityNumbers.numSnapshotEntities,
				sizeof( entityNumbers.snapshotEntities[0] ), SV_QsortEntityNumbers );

			// now that all viewpoint's areabits have been OR'd together, invert
			// all of them to make it a mask vector, which is what the renderer wants
			for ( int i = 0 ; i < MAX_MAP_AREA_BYTES/4 ; i++ ) {
				//((int *)frame->areamask)[i] = ((int *)frame->areamask)[i] ^ -1;
				((int *)frame->areamask)[i] = 0;
			}

			// copy the entity states out
			frame->numEntities = 0;
			for ( int i = 0 ; i < entityNumbers.numSnapshotEntities ; i++ ) {
				entityAndFloat_t *ent = &entityNumbers.snapshotEntities[i];
				/* TODO fixme */ /* if ( ent->number == clientNum ) {
					// don't add ourself
					continue;
				} */
				entityState_t *state = &ctx->cl.parseEntities[ctx->cl.parseEntitiesNum % MAX_PARSE_ENTITIES];
				entityState_t *floatForced = &cctx->parseEntitiesFloatForced[ctx->cl.parseEntitiesNum % MAX_PARSE_ENTITIES];
				ctx->cl.parseEntitiesNum++;
				*state = *ent->ent;
				*floatForced = ent->floatForced;
				// this should never hit, map should always be restarted first in SV_Frame
				if ( ctx->cl.parseEntitiesNum >= 0x7FFFFFFE ) {
					Com_Error(ERR_FATAL, "ctx->cl.parseEntitiesNum wrapped");
				}
				frame->numEntities++;
			}
		}

		snap.messageNum = ctx->clc.serverMessageSequence;
		snap.serverCommandNum = ctx->clc.serverCommandSequence;
		snap.valid = qtrue;
		snap.serverTime = frameTime;

		// copy new snap into structs
		ctx->cl.snap = ctx->cl.snapshots[snap.messageNum & PACKET_MASK] = snap;

		if ( framesSaved > 0 ) {
			writeMergedDeltaSnapshot( firstServerCommand, outFile, qfalse, serverCommandOffset );
			framesSaved++;
		} else {
			//writeMergedDemoHeader( outFile );
			writeMergedDeltaSnapshot( firstServerCommand, outFile, qtrue, serverCommandOffset );
			// copy rest
			framesSaved = 1;
		}

		//printf( "Wrote frame at time %d [%d:%02d.%04d]\n", ctx->cl.snap.serverTime, getCurrentTime() / 1000 / 60, (getCurrentTime() / 1000) % 60, getCurrentTime() % 1000 );
		/*printf( "Wrote frame at time %d clients ", ctx->cl.snap.serverTime );
		for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
			printf( "%d ", matches[matchIdx] );
		}
		printf( "\n" );*/

advanceLoop:
		// for all demos that have this frame, advance to the next frame.
		for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
			idx = mergedCtx.matches[matchIdx];
			if ( !entryList[idx].eos ) {
				ctx = entryList[idx].ctx;
				msg_t *msg = ReadNextMessage( &entryList[idx] );
				if ( msg == nullptr ) {
					entryList[idx].eos = qtrue;
					continue;
				}
				FreeMsg( msg );
			}
		}
	}

	{
		// finish up
		int len = -1;
		fwrite (&len, 4, 1, outFile);
		fwrite (&len, 4, 1, outFile);
	}

	for ( idx = 0; idx < numDemos; idx++ ) {
		FS_FCloseFile( entryList[idx].fp );
		free( entryList[idx].ctx );
	}
	free( entryList );
	fclose( outFile );

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

#define MAX_CUTS 1000

int main(int argc, char** argv)
{
	if ( argc < 4 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" demo1.dm_26 demo2.dm_26 ... demoN.dm_26 outfile.dm_26\n"
				"Note: all demoI.dm_26 should be from the same game\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	int numDemos = argc - 2;
	char **demos = &argv[1];
	char *outFile = argv[argc - 1];
	cl_shownet->integer = 3;
	RunMerge( demos, numDemos, outFile );
}
