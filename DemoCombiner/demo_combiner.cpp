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

#include <sys/types.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>
#endif

#ifdef WIN32
#define stat _stat
#endif


typedef struct {
	char filename[MAX_OSPATH];
	fileHandle_t fp;
	demoContext_t *ctx;
	int framesSaved;
	int currentMap;
	qboolean eos;
	// for server commands sent in the gamestate message, since we only loop on snaps
	//int gamestateServerReliableAcknowledge;
	//int gamestateServerCommandSequence;
	// idx of merged server command seq where the last server cmd from this demo was
	int serverCommandSyncIdx;
	demoMetadata_t* metadata;
	gamestateMetadata_t initialGamestates[16];
	int numInitialGamestates;
} demoEntry_t;

static msg_t s_msgPool[4];
static byte s_msgDataPool[4][MAX_MSGLEN];
static int s_msgPoolIdx = 0;

void FreeMsg( msg_t *msg ) {
}

static void ReadRemainingDemoTail( demoEntry_t *demo ) {
	int chunkSize = 1024;
	int readCount = 0;
	for (
		demo->metadata->tail = (byte*) realloc( demo->metadata->tail, demo->metadata->tailLen + chunkSize );
		( readCount = FS_Read( demo->metadata->tail + demo->metadata->tailLen, chunkSize, demo->fp ) ) > 0;
		demo->metadata->tailLen += readCount,
		demo->metadata->tail = (byte*) realloc( demo->metadata->tail, demo->metadata->tailLen + chunkSize ) ) {}
}

// returns demoFinished
// caller must free demo tail if not wanted
msg_t *ReadNextMessageRaw( demoEntry_t *demo ) {
	int slot = ( s_msgPoolIdx++ ) & 3;
	msg_t *msg = &s_msgPool[slot];
	byte *msgData = s_msgDataPool[slot];
	MSG_Init( msg, msgData, MAX_MSGLEN );
	if ( !CL_ReadDemoMessageWithBuf( demo->fp, msg, &demo->metadata->tailLen, &demo->metadata->tail ) ) {
		// make sure we reach the end of the file.  the message parsing bails at the first point it can't parse which could leave extra data.
		ReadRemainingDemoTail( demo );

		return nullptr;
	}
	return msg;
}

msg_t *ReadNextMessage( demoEntry_t *demo ) {
	msg_t *msg;
	msg_t* lastmsg = nullptr;
	int entityEventSequence = 0;
	if ( ctx->cl.snap.valid ) {
		entityEventSequence = ctx->cl.snap.ps.entityEventSequence;
	}
	ctx->cl.newSnapshots = qfalse;
	ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence + 1;
	// only append the gamestates if a frame is found after them.  otherwise, the splitter won't write them out
	int gamestateIdx = cctx->numGamestates;
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		try {
			CL_ParseServerMessage( msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			ReadRemainingDemoTail( demo );
			return msg;
		}

		if ( demo->metadata->lastGamestate == nullptr && !ctx->cl.newSnapshots && ctx->clc.serverCommandSequence > 0 ) {
			ctx->clc.lastExecutedServerCommand = ctx->serverReliableAcknowledge + 1;
		}
		if ( ctx->clc.serverCommandSequence - ctx->clc.lastExecutedServerCommand > MAX_RELIABLE_COMMANDS ) {
			ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10; // fudge factor
			for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
				char* command = ctx->clc.serverCommands[ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 )];
				if ( command[0] ) {
					break;
				}
			}
		}

		if ( !ctx->cl.newSnapshots ) {
			demo->metadata->clientnum = ctx->clc.clientNum;
			gamestateMetadata_t* gamestateMetadata = &cctx->gamestates[gamestateIdx++];
			gamestateMetadata->demoIdx = demo->metadata - cctx->demos;
			gamestateMetadata->serverMessageSequence = ctx->clc.serverMessageSequence;
			gamestateMetadata->serverReliableAcknowledge = ctx->serverReliableAcknowledge;
			gamestateMetadata->serverCommandSequence = ctx->clc.serverCommandSequence;
			gamestateMetadata->reliableAcknowledge = ctx->clc.reliableAcknowledge;
			gamestateMetadata->messageExtraByte = ctx->messageExtraByte;
			demo->metadata->lastGamestate = gamestateMetadata;
			FreeMsg( lastmsg );
			lastmsg = msg;
			continue;
		}
		ctx->cl.snap.ps.entityEventSequence = entityEventSequence;
		if ( lastSnapValid && ( ( lastSnapFlags ^ ctx->cl.snap.snapFlags ) & SNAPFLAG_SERVERCOUNT ) != 0 ) {
			demo->currentMap++;
		}

		// we found a snapshot, so delete the message buffer
		free( demo->metadata->tail );
		demo->metadata->tail = NULL;
		demo->metadata->tailLen = 0;

		// commit gamestates
		cctx->numGamestates = gamestateIdx;
		return msg;
	}
	// we didn't find a snapshot.  any other messages are concat into the demo tail.
	return lastmsg;
}

// returns true if demo context could be parsed
qboolean ParseDemoContext( demoEntry_t *demo ) {
	// clear any existing state
	demoContext_t *oldCtx;
	oldCtx = ctx;
	ctx = demo->ctx;
	demo->metadata->clientnum = -1;
	int prevNumGamestates = cctx->numGamestates;
	// load initial state from demo
	msg_t *msg;
	if ( ( msg = ReadNextMessage( demo ) ) != nullptr ) {
		demo->metadata->firstFrameTime = ctx->cl.snap.serverTime;
		if ( !ctx->cl.newSnapshots ) {
			demo->metadata->firstFrameTime = -1;
			demo->eos = qtrue;
			demo->metadata->eos = qtrue;
		}
		demo->numInitialGamestates = cctx->numGamestates - prevNumGamestates;
		for ( int i = 0; i < demo->numInitialGamestates; i++ ) {
			demo->initialGamestates[i] = cctx->gamestates[prevNumGamestates + i];
		}
		cctx->numGamestates = prevNumGamestates;
		demo->metadata->lastGamestate = demo->numInitialGamestates > 0 ? &demo->initialGamestates[demo->numInitialGamestates - 1] : nullptr;
		ctx = oldCtx;
		FreeMsg( msg );
		return qtrue;
	}
	cctx->numGamestates = prevNumGamestates;
	demo->metadata->firstFrameTime = -1;
	demo->eos = qtrue;
	demo->metadata->eos = qtrue;
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


typedef struct netField_s {
	const char* name;
	size_t	offset;
	int		bits;		// 0 = float
#ifndef FINAL_BUILD
	unsigned	mCount;
#endif
} netField_t;
extern netField_t entityStateFields[];


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

char* basename( char* filename ) {
	char* name = strrchr( filename, '/' );
	if ( name != NULL ) {
		return &name[1];
	}
	name = strrchr( filename, '\\' );
	if ( name != NULL ) {
		return &name[1];
	}
	return filename;
}

extern bool sendFullNegativeZero;

static combinedDemoContext_t mergedCtx;
combinedDemoContext_t* cctx = &mergedCtx;
// bitmask of which clients had this ent in cur/prev snaps.
int RunMerge(char **demos, int numDemos, char *outFilename)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");

	Com_Memset( &mergedCtx, 0, sizeof( mergedCtx ) );

	demoEntry_t *entryList = (demoEntry_t *) calloc( numDemos, sizeof( demoEntry_t ) );
	mergedCtx.demos = (demoMetadata_t*) calloc( numDemos, sizeof( demoMetadata_t ) );
	mergedCtx.numDemos = numDemos;
	{ // debugger fucks up idx without this
		for ( int idx = 0; idx < numDemos; idx++ ) {
			Q_strncpyz( entryList[idx].filename, demos[idx], sizeof( entryList[idx].filename ) );
			Q_strncpyz( mergedCtx.demos[idx].filename, basename(demos[idx]), sizeof( mergedCtx.demos[idx].filename ) );
			struct stat result;
			if ( stat( demos[idx], &result ) == 0 ) {
				mergedCtx.demos[idx].fileMtime = result.st_mtime;
			}
			entryList[idx].metadata = &mergedCtx.demos[idx];
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

	qboolean demoFinished = qfalse;
	int idx;
	for ( idx = 0; idx < numDemos; idx++ ) {
		if ( !ParseDemoContext( &entryList[idx] ) ) {
			printf( "Failed to parse demo context for demo %s, index %d\n", entryList[idx].filename, idx );
			// return -1;
		}
	}
	// Sort demos by firstFrameTime so metadata and gamestates can be streamed chronologically as clients join
	for ( int i = 1; i < numDemos; i++ ) {
		demoEntry_t keyEntry = entryList[i];
		demoMetadata_t keyMeta = mergedCtx.demos[i];
		int j = i - 1;
		while ( j >= 0 && mergedCtx.demos[j].firstFrameTime > keyMeta.firstFrameTime ) {
			entryList[j + 1] = entryList[j];
			mergedCtx.demos[j + 1] = mergedCtx.demos[j];
			j--;
		}
		entryList[j + 1] = keyEntry;
		mergedCtx.demos[j + 1] = keyMeta;
	}
	for ( idx = 0; idx < numDemos; idx++ ) {
		entryList[idx].metadata = &mergedCtx.demos[idx];
		if ( entryList[idx].numInitialGamestates > 0 ) {
			entryList[idx].metadata->lastGamestate = &entryList[idx].initialGamestates[entryList[idx].numInitialGamestates - 1];
		}
	}
	mergedCtx.numDemos = 0;
	// start with a base ctx
	// should be the earliest one to pick up earliest configstrings
	// note this could actually be wrong cuz sometimes serverTime resets on map restart
	{
		int minTime = (int) ( ( (unsigned int) -1 ) >> 1 );
		int minIdx = 0;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( !entryList[idx].eos && entryList[idx].ctx->cl.snap.serverTime < minTime ) {
				minTime = entryList[idx].ctx->cl.snap.serverTime;
				minIdx = idx;
			}
		}
		mergedCtx.ctx.cl.gameState = entryList[minIdx].ctx->cl.gameState;
		memcpy( mergedCtx.ctx.cl.entityBaselines, entryList[minIdx].ctx->cl.entityBaselines, sizeof( mergedCtx.ctx.cl.entityBaselines ) );
		mergedCtx.ctx.clc = entryList[minIdx].ctx->clc;
		mergedCtx.ctx.cls = entryList[minIdx].ctx->cls;
		mergedCtx.ctx.serverReliableAcknowledge = entryList[minIdx].ctx->serverReliableAcknowledge;
		mergedCtx.ctx.areabytes = entryList[minIdx].ctx->areabytes;
		mergedCtx.ctx.messageExtraByte = entryList[minIdx].ctx->messageExtraByte;
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
		while ( cctx->numDemos < numDemos && entryList[cctx->numDemos].metadata->firstFrameTime <= frameTime ) {
			int newIdx = cctx->numDemos++;
			for ( int g = 0; g < entryList[newIdx].numInitialGamestates; g++ ) {
				gamestateMetadata_t *gs = &cctx->gamestates[cctx->numGamestates++];
				*gs = entryList[newIdx].initialGamestates[g];
				gs->demoIdx = newIdx;
				entryList[newIdx].metadata->lastGamestate = gs;
			}
		}
		// find all demos that got this frame
		mergedCtx.numMatches = 0;
		Com_Memset( mergedCtx.matches, 0, sizeof( mergedCtx.matches ) );
		mergedCtx.matchedClients = 0;
		Com_Memset( mergedCtx.deltasnap, 0, sizeof( mergedCtx.deltasnap ) );
		//int numNonMatches = 0;
		//int nonMatches[MAX_CLIENTS];
		for ( idx = 0; idx < cctx->numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos ) {
				mergedCtx.matches[mergedCtx.numMatches++] = idx;
				int clientNum = entryList[idx].ctx->clc.clientNum;
				mergedCtx.matchedClients |= 1 << clientNum;
				int deltaNum = entryList[idx].ctx->cl.snap.deltaNum;
				int messageNum = entryList[idx].ctx->cl.snap.messageNum;
				mergedCtx.deltasnap[clientNum] = deltaNum == -1 ? 0 : messageNum - deltaNum;
				if ( deltaNum != -1 && entryList[idx].ctx->cl.snapshots[deltaNum & PACKET_MASK].serverCommandNum != entryList[idx].ctx->serverReliableAcknowledge ) {
					mergedCtx.deltasnap[clientNum] |= 0x80;
				}
				int psIdx = mergedCtx.curPlayerStateIdxMask & ( 1 << clientNum ) ? 1 : 0;
				mergedCtx.playerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.ps;
				const playerState_t &srcPsff = entryList[idx].ctx->playerStateForcedFields[entryList[idx].ctx->cl.snap.messageNum & PACKET_MASK];
				playerState_t &dstPsff = mergedCtx.playerStatesForcedFields[psIdx][clientNum];
				if ( srcPsff.ping ) {
					dstPsff = srcPsff;
				} else if ( dstPsff.ping ) {
					Com_Memset( &dstPsff, 0, sizeof( dstPsff ) );
				}
				if ( mergedCtx.playerStates[psIdx][clientNum].m_iVehicleNum ) {
					mergedCtx.vehPlayerStates[psIdx][clientNum] = entryList[idx].ctx->cl.snap.vps;
					const playerState_t &srcVpsff = entryList[idx].ctx->vehPlayerStateForcedFields[entryList[idx].ctx->cl.snap.messageNum & PACKET_MASK];
					playerState_t &dstVpsff = mergedCtx.vehPlayerStatesForcedFields[psIdx][clientNum];
					if ( srcVpsff.ping ) {
						dstVpsff = srcVpsff;
					} else if ( dstVpsff.ping ) {
						Com_Memset( &dstVpsff, 0, sizeof( dstVpsff ) );
					}
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
		//ctx = entryList[mergedCtx.matches[0]].ctx;
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
			for ( int commandNum = entryCtx->clc.lastExecutedServerCommand; commandNum <= entryCtx->clc.serverCommandSequence; commandNum++ ) {
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
					if ( !Q_strncmp( command, ctx->clc.serverCommands[snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 )], MAX_STRING_CHARS ) ) {
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
					if ( commandNum <= entryList[idx].metadata->lastGamestate->serverCommandSequence && framesSaved == 0 ) {
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
		if ( cctx->numDemos > cctx->numHandledDemos || cctx->numGamestates > cctx->numHandledGamestates ) {
			writeMergedGamestateData( outFile );
		}
		for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
			idx = mergedCtx.matches[matchIdx];
			demoMetadata_t* metadata = entryList[idx].metadata;
			if ( metadata->firstFrameTime != frameTime ) {
				continue;
			}
			demoContext_t* entryCtx = entryList[idx].ctx;
			// this is this demo's first frame, make sure that the configstrings match the demo's gamestate
			// (might be different if some packets are lost)
			if ( ctx->cl.gameState.dataCount != entryCtx->cl.gameState.dataCount
				|| memcmp( ctx->cl.gameState.stringData, entryCtx->cl.gameState.stringData, ctx->cl.gameState.dataCount )
				|| memcmp( ctx->cl.gameState.stringOffsets, entryCtx->cl.gameState.stringOffsets, sizeof( ctx->cl.gameState.stringOffsets ) ) ) {
				// TODO: we might need to execute any configstring server commands this client got in its gamestate message on the merged ctx to reduce the number of discrepancies here
				Com_Printf( "configstrings differ!\n" );
				for ( int i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
					char* orig = entryCtx->cl.gameState.stringData + entryCtx->cl.gameState.stringOffsets[i];
					char* combined = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[i];
					if ( Q_strncmp( orig, combined, MAX_STRING_CHARS ) ) {
						Com_Printf( "configstring %d: orig %s, but we have %s\n", i, orig, combined );
						int overrideIdx = metadata->lastGamestate->numConfigStringOverrides++;
						metadata->lastGamestate->configStringOverrideIndex[overrideIdx] = i;
						metadata->lastGamestate->configStringOverride[overrideIdx] = orig;
					}
				}
			}
		}
		char bigConfigString[BIG_INFO_STRING];
		for ( int commandNum = firstServerCommand; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
			char* command = ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
			if ( !( ( command[0] == 'c' && command[1] == 's' ) || ( command[0] == 'b' && command[1] == 'c' && command[2] == 's' ) ) ) {
				continue;
			}
			Cmd_TokenizeString( command );
			char* cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "bcs0" ) ) {
				Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv( 1 ), Cmd_Argv( 2 ) );
			} else if ( !strcmp( cmd, "bcs1" ) ) {
				char *s = Cmd_Argv( 2 );
				if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
					Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
				}
				strcat( bigConfigString, s );
			} else if ( !strcmp( cmd, "bcs2" ) ) {
				char *s = Cmd_Argv( 2 );
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
			entityState_t				*snapEnts[MAX_GENTITIES];
			uint32_t					snapEntVer[MAX_GENTITIES];
			static entityState_t		snapFloatForced[MAX_GENTITIES];

			int32_t *owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];
			mergedCtx.ent_owner_idx ^= 1;
			int32_t *prev_owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];

			// clear everything in this snapshot
			Com_Memset( frame->areamask, 0, sizeof( frame->areamask ) );
			memset( owners, 0, sizeof( *owners ) * MAX_GENTITIES );

			frame->numEntities = 0;

			int matchesMask = 0;
			for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
				idx = mergedCtx.matches[matchIdx];
				demoContext_t *dctx = entryList[idx].ctx;
				clSnapshot_t *dsnap = &dctx->cl.snap;
				int clientNum = dctx->clc.clientNum;
				int clientBit = 1 << clientNum;

				cctx->snapFlags[clientNum] = dsnap->snapFlags;

				// add all the entities
				for ( int entIdx = dsnap->parseEntitiesNum; entIdx < dsnap->parseEntitiesNum + dsnap->numEntities; entIdx++ ) {
					int parseIdx = entIdx & ( MAX_PARSE_ENTITIES - 1 );
					entityState_t *ent = dctx->parseEntityPtr[parseIdx];
					entityState_t *floatForced = &dctx->parseEntitiesFloatForced[parseIdx];
					int entNum = ent->number;
					if ( !owners[entNum] ) {
						snapEnts[entNum] = ent;
						snapEntVer[entNum] = dctx->parseEntityVersion[parseIdx];
						if ( snapFloatForced[entNum].number ) {
							Com_Memset( &snapFloatForced[entNum], 0, sizeof( entityState_t ) );
						}
					}
					owners[entNum] |= clientBit;
					if ( floatForced->number ) {
						entityState_t *toFloatForced = &snapFloatForced[entNum];
						for ( int i = 0; i < (int)( sizeof( entityState_t ) / 4 ) - 1; i++ ) {
							size_t offset = entityStateFields[i].offset;
							if ( *(int*)( (byte*)floatForced + offset ) == 1 ) {
								*(int*)( (byte*)toFloatForced + offset ) |= clientBit;
							}
						}
						toFloatForced->number |= clientBit;
					}
				}

				snap.ps = dsnap->ps;
				matchesMask |= clientBit;
			}

			// copy the entity states out in sorted entNum order
			frame->numEntities = 0;
			for ( int entIdx = 0; entIdx < MAX_GENTITIES; entIdx++ ) {
				if ( owners[entIdx] != 0 ) {
					owners[entIdx] |= ( prev_owners[entIdx] & ~matchesMask );
					int dstIdx = ctx->cl.parseEntitiesNum & ( MAX_PARSE_ENTITIES - 1 );
					entityState_t *floatForced = &cctx->parseEntitiesFloatForced[dstIdx];
					ctx->cl.parseEntitiesNum++;
					ctx->parseEntityPtr[dstIdx] = snapEnts[entIdx];
					ctx->parseEntityVersion[dstIdx] = snapEntVer[entIdx];
					if ( snapFloatForced[entIdx].number ) {
						*floatForced = snapFloatForced[entIdx];
					} else if ( floatForced->number ) {
						Com_Memset( floatForced, 0, sizeof( *floatForced ) );
					}
					if ( ctx->cl.parseEntitiesNum >= 0x7FFFFFFE ) {
						Com_Error( ERR_FATAL, "ctx->cl.parseEntitiesNum wrapped" );
					}
					frame->numEntities++;
				}
			}
		}

		snap.messageNum = ctx->clc.serverMessageSequence;
		snap.serverCommandNum = ctx->clc.serverCommandSequence;
		snap.valid = qtrue;
		snap.serverTime = frameTime;

		// clear the valid flags of any snapshots between the last received and this one
		{
			int oldMessageNum = ctx->cl.snap.messageNum + 1;
			if ( snap.messageNum - oldMessageNum >= PACKET_BACKUP ) {
				oldMessageNum = snap.messageNum - ( PACKET_BACKUP - 1 );
			}
			for ( ; oldMessageNum < snap.messageNum; oldMessageNum++ ) {
				ctx->cl.snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
			}
		}
		// copy new snap into structs
		CopyClSnapshot( &ctx->cl.snap, &snap );
		CopyClSnapshot( &ctx->cl.snapshots[snap.messageNum & PACKET_MASK], &snap );

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
				if ( msg == nullptr || !ctx->cl.newSnapshots ) {
					FreeMsg( msg );
					entryList[idx].eos = qtrue;
					entryList[idx].metadata->eos = qtrue;
					continue;
				}
				FreeMsg( msg );
			}
		}
	}

	writeTailData( outFile );

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
	if ( argc < 3 ) {
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
