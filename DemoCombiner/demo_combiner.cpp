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
	gamestateMetadata_t* initialGamestates;
	gameState_t* initialGameStates;
	gamestateMetadata_t** pendingGamestatePtrs;
	int numInitialGamestates;
	int maxInitialGamestates;
	gameState_t splitterGameState;
	qboolean splitterSeqInitialized;
} demoEntry_t;

static msg_t s_msgPool[4];
static byte s_msgDataPool[4][MAX_MSGLEN];
static int s_msgPoolIdx = 0;

void FreeMsg( msg_t *msg ) {
}

static void ReadRemainingDemoTail( demoEntry_t *demo ) {
	int chunkSize = 65536;
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
	int prevServerCommandSequence = ctx->clc.serverCommandSequence;
	ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence + 1;
	// only append the gamestates if a frame is found after them.  otherwise, the splitter won't write them out
	gamestateMetadata_t* prevLastGamestate = demo->metadata->lastGamestate;
	int pendingIdx = 0;
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		int prevChecksumFeed = ctx->clc.checksumFeed;
		int prevClientNum = ctx->clc.clientNum;
		try {
			CL_ParseServerMessage( msg );
			if ( ctx->hadDownload ) {
				Com_Error( ERR_DROP, "CL_ParseDownload: download in demo message" );
			}
			if ( ctx->deltaFromInvalidFrame ) {
				Com_Error( ERR_DROP, "CL_ParseSnapshot: delta from invalid frame" );
			}
			if ( ctx->hadUnsupportedSvc ) {
				Com_Error( ERR_DROP, "CL_ParseServerMessage: unsupported svc in demo message" );
			}
			if ( ctx->newGamestate ) {
				if ( cctx->ctx.clc.clientNum == -1 ) {
					if ( ctx->clc.checksumFeed != cctx->ctx.clc.checksumFeed
						|| ( demo->metadata->clientnum != -1 && ctx->clc.clientNum != demo->metadata->clientnum )
						|| memcmp( ctx->cl.entityBaselines, cctx->ctx.cl.entityBaselines, sizeof( ctx->cl.entityBaselines ) ) != 0
						|| memcmp( ctx->entityBaselinesFloatForced, cctx->ctx.entityBaselinesFloatForced, sizeof( ctx->entityBaselinesFloatForced ) ) != 0 ) {
						Com_Error( ERR_DROP, "CL_ParseGamestate: gamestate mismatch with combined header" );
					}
				} else if ( pendingIdx > 0 ) {
					if ( ctx->clc.checksumFeed != prevChecksumFeed || ctx->clc.clientNum != prevClientNum ) {
						Com_Error( ERR_DROP, "CL_ParseGamestate: initial gamestate mismatch" );
					}
				}
			}
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			ctx->cl.newSnapshots = qfalse;
			demo->metadata->lastGamestate = prevLastGamestate;
			ReadRemainingDemoTail( demo );
			return nullptr;
		}

		if ( demo->metadata->lastGamestate == nullptr && prevServerCommandSequence == 0 && ctx->clc.serverCommandSequence > 0 ) {
			ctx->clc.lastExecutedServerCommand = ctx->serverReliableAcknowledge + 1;
		} else if ( demo->framesSaved == 0 && pendingIdx > 0 && ctx->cl.newSnapshots && ctx->clc.serverCommandSequence > 0 && ctx->serverReliableAcknowledge + 1 < ctx->clc.lastExecutedServerCommand ) {
			ctx->clc.lastExecutedServerCommand = ctx->serverReliableAcknowledge + 1;
		}
		prevServerCommandSequence = ctx->clc.serverCommandSequence;
		if ( ctx->clc.serverCommandSequence - ctx->clc.lastExecutedServerCommand > MAX_BUFFERED_SERVER_COMMANDS ) {
			ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence - MAX_BUFFERED_SERVER_COMMANDS + 10; // fudge factor
			for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
				if ( ctx->serverCommandSeq[ctx->clc.lastExecutedServerCommand & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] == ctx->clc.lastExecutedServerCommand + 1 ) {
					break;
				}
			}
		}

		if ( !ctx->cl.newSnapshots ) {
			if ( ctx->newGamestate ) {
				if ( pendingIdx >= demo->maxInitialGamestates ) {
					int newMax = demo->maxInitialGamestates ? demo->maxInitialGamestates * 2 : 16;
					demo->initialGamestates = (gamestateMetadata_t*) realloc( demo->initialGamestates, newMax * sizeof( gamestateMetadata_t ) );
					demo->initialGameStates = (gameState_t*) realloc( demo->initialGameStates, newMax * sizeof( gameState_t ) );
					demo->pendingGamestatePtrs = (gamestateMetadata_t**) realloc( demo->pendingGamestatePtrs, newMax * sizeof( gamestateMetadata_t* ) );
					demo->maxInitialGamestates = newMax;
				}
				demo->metadata->clientnum = ctx->clc.clientNum;
				gamestateMetadata_t* gamestateMetadata = &demo->initialGamestates[pendingIdx];
				Com_Memset( gamestateMetadata, 0, sizeof( *gamestateMetadata ) );
				gamestateMetadata->demoIdx = demo->metadata - cctx->demos;
				gamestateMetadata->serverMessageSequence = ctx->clc.serverMessageSequence;
				gamestateMetadata->serverReliableAcknowledge = ctx->serverReliableAcknowledge;
				gamestateMetadata->serverCommandSequence = ctx->clc.serverCommandSequence;
				gamestateMetadata->reliableAcknowledge = ctx->clc.reliableAcknowledge;
				gamestateMetadata->messageExtraByte = ctx->messageExtraByte;
				demo->initialGameStates[pendingIdx] = ctx->cl.gameState;
				demo->metadata->lastGamestate = gamestateMetadata;
				pendingIdx++;
			}
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
		demo->numInitialGamestates = pendingIdx;
		return msg;
	}
	// we didn't find a snapshot.  any other messages are concat into the demo tail.
	demo->metadata->lastGamestate = prevLastGamestate;
	return lastmsg;
}

// returns true if demo context could be parsed
qboolean ParseDemoContext( demoEntry_t *demo ) {
	// clear any existing state
	demoContext_t *oldCtx;
	oldCtx = ctx;
	ctx = demo->ctx;
	demo->metadata->clientnum = -1;
	// load initial state from demo
	msg_t *msg;
	if ( ( msg = ReadNextMessage( demo ) ) != nullptr ) {
		demo->metadata->firstFrameTime = ctx->cl.snap.serverTime;
		if ( !ctx->cl.newSnapshots ) {
			demo->metadata->firstFrameTime = -1;
			demo->eos = qtrue;
			demo->metadata->eos = qtrue;
			demo->numInitialGamestates = 0;
		}
		demo->metadata->lastGamestate = demo->numInitialGamestates > 0 ? &demo->initialGamestates[demo->numInitialGamestates - 1] : nullptr;
		ctx = oldCtx;
		FreeMsg( msg );
		return qtrue;
	}
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
extern netField_t entityStateFields[132];


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
	qboolean needRefCtx = qfalse;
	for ( idx = 0; idx < numDemos; idx++ ) {
		if ( !entryList[idx].eos && entryList[idx].numInitialGamestates == 0 ) {
			needRefCtx = qtrue;
			break;
		}
	}
	if ( needRefCtx ) {
		int refIdx = -1;
		int refMinTime = (int) ( ( (unsigned int) -1 ) >> 1 );
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( !entryList[idx].eos && entryList[idx].numInitialGamestates > 0 && entryList[idx].ctx->cl.snap.serverTime < refMinTime ) {
				refMinTime = entryList[idx].ctx->cl.snap.serverTime;
				refIdx = idx;
			}
		}
		demoContext_t *refCtx = refIdx != -1 ? entryList[refIdx].ctx : nullptr;
		demoContext_t *scanCtx = nullptr;
		if ( !refCtx ) {
			scanCtx = (demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
			demoContext_t *oldCtx = ctx;
			ctx = scanCtx;
			for ( idx = 0; idx < numDemos && !refCtx; idx++ ) {
				fileHandle_t scanFp = 0;
				FS_FOpenFileRead( entryList[idx].filename, &scanFp, qfalse );
				if ( !scanFp ) {
					continue;
				}
				Com_Memset( scanCtx, 0, sizeof( demoContext_t ) );
				msg_t scanMsg;
				byte scanMsgData[MAX_MSGLEN];
				int dummyTailLen = 0;
				byte *dummyTail = NULL;
				while ( CL_ReadDemoMessageWithBuf( scanFp, ( MSG_Init( &scanMsg, scanMsgData, MAX_MSGLEN ), &scanMsg ), &dummyTailLen, &dummyTail ) ) {
					free( dummyTail );
					dummyTail = NULL;
					dummyTailLen = 0;
					try {
						CL_ParseServerMessage( &scanMsg );
					} catch ( int ) {
						break;
					}
					if ( scanCtx->newGamestate ) {
						refCtx = scanCtx;
						break;
					}
				}
				free( dummyTail );
				FS_FCloseFile( scanFp );
			}
			if ( !refCtx ) {
				Com_Memset( scanCtx, 0, sizeof( demoContext_t ) );
				while ( 1 ) {
					int minTime = (int) ( ( (unsigned int) -1 ) >> 1 );
					for ( idx = 0; idx < numDemos; idx++ ) {
						if ( !entryList[idx].eos && entryList[idx].ctx->cl.snap.serverTime < minTime ) {
							minTime = entryList[idx].ctx->cl.snap.serverTime;
						}
					}
					if ( minTime == (int) ( ( (unsigned int) -1 ) >> 1 ) ) {
						break;
					}
					entityState_t *firstEnt[MAX_GENTITIES] = { 0 };
					for ( idx = 0; idx < numDemos; idx++ ) {
						if ( !entryList[idx].eos && entryList[idx].ctx->cl.snap.serverTime == minTime ) {
							clSnapshot_t *snap = &entryList[idx].ctx->cl.snap;
							for ( int j = 0; j < snap->numEntities; j++ ) {
								entityState_t *ent = entryList[idx].ctx->parseEntityPtr[( snap->parseEntitiesNum + j ) & ( MAX_PARSE_ENTITIES - 1 )];
								int num = ent->number;
								if ( !firstEnt[num] ) {
									firstEnt[num] = ent;
								} else if ( memcmp( firstEnt[num], ent, sizeof( entityState_t ) ) != 0 ) {
									for ( size_t off = 0; off < sizeof( entityState_t ); off += 4 ) {
										int v1 = *(int *)( (byte *)firstEnt[num] + off );
										int v2 = *(int *)( (byte *)ent + off );
										if ( v1 != v2 ) {
											int nz = v1 != 0 ? v1 : v2;
											int *dst = (int *)( (byte *)&scanCtx->cl.entityBaselines[num] + off );
											if ( *dst != nz ) {
												*dst = nz;
												scanCtx->cl.entityBaselines[num].number = num;
											}
										}
									}
								}
							}
						}
					}
					for ( idx = 0; idx < numDemos; idx++ ) {
						if ( !entryList[idx].eos && entryList[idx].ctx->cl.snap.serverTime == minTime ) {
							ctx = entryList[idx].ctx;
							msg_t *msg = ReadNextMessage( &entryList[idx] );
							if ( msg == nullptr || !entryList[idx].ctx->cl.newSnapshots ) {
								entryList[idx].eos = qtrue;
							} else {
								FreeMsg( msg );
							}
						}
					}
				}
				refCtx = scanCtx;
				for ( idx = 0; idx < numDemos; idx++ ) {
					if ( entryList[idx].metadata->firstFrameTime != -1 ) {
						entryList[idx].eos = qfalse;
					}
				}
			}
			ctx = oldCtx;
		}
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( !entryList[idx].eos && entryList[idx].numInitialGamestates == 0 ) {
				int clientNum = entryList[idx].ctx->cl.snap.ps.clientNum;
				if ( entryList[idx].metadata->filename[0] >= '0' && entryList[idx].metadata->filename[0] <= '9' ) {
					clientNum = atoi( entryList[idx].metadata->filename );
				}
				if ( refCtx ) {
					free( entryList[idx].metadata->tail );
					entryList[idx].metadata->tail = NULL;
					entryList[idx].metadata->tailLen = 0;
					Com_Memset( entryList[idx].ctx, 0, sizeof( demoContext_t ) );
					entryList[idx].ctx->cl.gameState = refCtx->cl.gameState;
					memcpy( entryList[idx].ctx->cl.entityBaselines, refCtx->cl.entityBaselines, sizeof( entryList[idx].ctx->cl.entityBaselines ) );
					memcpy( entryList[idx].ctx->entityBaselinesFloatForced, refCtx->entityBaselinesFloatForced, sizeof( entryList[idx].ctx->entityBaselinesFloatForced ) );
					entryList[idx].ctx->clc.checksumFeed = refCtx->clc.checksumFeed;
					entryList[idx].currentMap = 0;
					entryList[idx].eos = qfalse;
					entryList[idx].metadata->eos = qfalse;
					FS_FCloseFile( entryList[idx].fp );
					FS_FOpenFileRead( entryList[idx].filename, &entryList[idx].fp, qfalse );
					ParseDemoContext( &entryList[idx] );
				}
				entryList[idx].ctx->clc.clientNum = clientNum;
				entryList[idx].metadata->clientnum = clientNum;
			}
		}
		free( scanCtx );
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
		entryList[idx].metadata->lastGamestate = entryList[idx].numInitialGamestates > 0 ? &entryList[idx].initialGamestates[entryList[idx].numInitialGamestates - 1] : nullptr;
	}
	mergedCtx.numDemos = 0;
	// start with a base ctx
	// should be the earliest one to pick up earliest configstrings
	// note this could actually be wrong cuz sometimes serverTime resets on map restart
	{
		int minTime = (int) ( ( (unsigned int) -1 ) >> 1 );
		int minIdx = -1;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( !entryList[idx].eos && entryList[idx].numInitialGamestates > 0 && entryList[idx].ctx->cl.snap.serverTime < minTime ) {
				minTime = entryList[idx].ctx->cl.snap.serverTime;
				minIdx = idx;
			}
		}
		if ( minIdx == -1 ) {
			for ( idx = 0; idx < numDemos; idx++ ) {
				if ( !entryList[idx].eos && entryList[idx].ctx->cl.snap.serverTime < minTime ) {
					minTime = entryList[idx].ctx->cl.snap.serverTime;
					minIdx = idx;
				}
			}
		}
		if ( minIdx == -1 ) {
			minIdx = 0;
		}
		mergedCtx.ctx.cl.gameState = entryList[minIdx].ctx->cl.gameState;
		memcpy( mergedCtx.ctx.cl.entityBaselines, entryList[minIdx].ctx->cl.entityBaselines, sizeof( mergedCtx.ctx.cl.entityBaselines ) );
		memcpy( mergedCtx.ctx.entityBaselinesFloatForced, entryList[minIdx].ctx->entityBaselinesFloatForced, sizeof( mergedCtx.ctx.entityBaselinesFloatForced ) );
		mergedCtx.ctx.clc = entryList[minIdx].ctx->clc;
		mergedCtx.ctx.cls = entryList[minIdx].ctx->cls;
		mergedCtx.ctx.serverReliableAcknowledge = entryList[minIdx].ctx->serverReliableAcknowledge;
		mergedCtx.ctx.areabytes = entryList[minIdx].ctx->areabytes;
		mergedCtx.ctx.messageExtraByte = entryList[minIdx].ctx->messageExtraByte;
	}
	mergedCtx.ctx.clc.lastExecutedServerCommand = mergedCtx.ctx.clc.serverCommandSequence = 0; // entryList[0].firstServerCommand;
	mergedCtx.ctx.clc.serverCommandSequence--;
	mergedCtx.ctx.clc.clientNum = -1;
	for ( idx = 0; idx < numDemos; idx++ ) {
		if ( !entryList[idx].eos && entryList[idx].numInitialGamestates > 0 ) {
			if ( entryList[idx].ctx->clc.checksumFeed != mergedCtx.ctx.clc.checksumFeed
				|| memcmp( entryList[idx].ctx->cl.entityBaselines, mergedCtx.ctx.cl.entityBaselines, sizeof( mergedCtx.ctx.cl.entityBaselines ) ) != 0
				|| memcmp( entryList[idx].ctx->entityBaselinesFloatForced, mergedCtx.ctx.entityBaselinesFloatForced, sizeof( mergedCtx.ctx.entityBaselinesFloatForced ) ) != 0 ) {
				FS_FCloseFile( entryList[idx].fp );
				FS_FOpenFileRead( entryList[idx].filename, &entryList[idx].fp, qfalse );
				free( entryList[idx].metadata->tail );
				entryList[idx].metadata->tail = NULL;
				entryList[idx].metadata->tailLen = 0;
				ReadRemainingDemoTail( &entryList[idx] );
				entryList[idx].eos = qtrue;
				entryList[idx].metadata->eos = qtrue;
				entryList[idx].metadata->firstFrameTime = -1;
				entryList[idx].metadata->lastGamestate = nullptr;
				entryList[idx].numInitialGamestates = 0;
			}
		}
	}
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
			while ( cctx->numDemos < numDemos ) {
				int newIdx = cctx->numDemos++;
				for ( int g = 0; g < entryList[newIdx].numInitialGamestates; g++ ) {
					gamestateMetadata_t *gs = AllocCombinedGamestate( cctx );
					*gs = entryList[newIdx].initialGamestates[g];
					gs->demoIdx = newIdx;
					entryList[newIdx].metadata->lastGamestate = gs;
				}
			}
			if ( framesSaved == 0 && cctx->numDemos > 0 ) {
				ctx->clc.serverMessageSequence++;
				writeMergedDemoHeader( outFile );
			}
			if ( cctx->numDemos > cctx->numHandledDemos || cctx->numGamestates > cctx->numHandledGamestates ) {
				writeMergedGamestateData( outFile );
			}
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
			if ( entryList[newIdx].eos ) {
				for ( int g = 0; g < entryList[newIdx].numInitialGamestates; g++ ) {
					gamestateMetadata_t *gs = AllocCombinedGamestate( cctx );
					*gs = entryList[newIdx].initialGamestates[g];
					gs->demoIdx = newIdx;
					entryList[newIdx].pendingGamestatePtrs[g] = gs;
					entryList[newIdx].metadata->lastGamestate = gs;
				}
			}
		}
		// find all demos that got this frame
		mergedCtx.numMatches = 0;
		Com_Memset( mergedCtx.matches, 0, sizeof( mergedCtx.matches ) );
		mergedCtx.matchedClients = 0;
		Com_Memset( mergedCtx.deltasnap, 0, sizeof( mergedCtx.deltasnap ) );
		//int numNonMatches = 0;
		//int nonMatches[MAX_CLIENTS];
		auto demoHasDisconnect = []( demoContext_t *dctx ) -> qboolean {
			for ( int c = dctx->clc.lastExecutedServerCommand; c <= dctx->clc.serverCommandSequence; c++ ) {
				if ( dctx->serverCommandSeq[c & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] == c + 1 ) {
					if ( !Q_strncmp( dctx->serverCommands[c & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )], "disconnect", 10 ) ) {
						return qtrue;
					}
				}
			}
			return qfalse;
		};
		qboolean anyNonDisconnectAtFrame = qfalse;
		for ( idx = 0; idx < cctx->numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos && !demoHasDisconnect( entryList[idx].ctx ) ) {
				anyNonDisconnectAtFrame = qtrue;
				break;
			}
		}
		for ( idx = 0; idx < cctx->numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos && ( !anyNonDisconnectAtFrame || !demoHasDisconnect( entryList[idx].ctx ) ) ) {
				for ( int g = 0; g < entryList[idx].numInitialGamestates; g++ ) {
					gamestateMetadata_t *gs = AllocCombinedGamestate( cctx );
					*gs = entryList[idx].initialGamestates[g];
					gs->demoIdx = idx;
					entryList[idx].pendingGamestatePtrs[g] = gs;
					entryList[idx].metadata->lastGamestate = gs;
				}
				mergedCtx.matches[mergedCtx.numMatches++] = idx;
				int clientNum = entryList[idx].ctx->clc.clientNum;
				if ( !( mergedCtx.matchedClients & ( 1 << clientNum ) ) ) {
					mergedCtx.matchedClients |= 1 << clientNum;
					int deltaNum = entryList[idx].ctx->cl.snap.deltaNum;
					int rawLastFrame = entryList[idx].ctx->snapLastFrame;
					qboolean needSra = ( deltaNum <= 0
						|| entryList[idx].ctx->cl.snapshots[deltaNum & PACKET_MASK].messageNum != deltaNum
						|| entryList[idx].ctx->cl.snapshots[deltaNum & PACKET_MASK].serverCommandNum != entryList[idx].ctx->serverReliableAcknowledge ) ? qtrue : qfalse;
					mergedCtx.deltasnap[clientNum] = rawLastFrame | ( needSra ? 0x100 : 0 );
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
					mergedCtx.areabytes[clientNum] = (byte) entryList[idx].ctx->areabytes;
					Com_Memcpy( mergedCtx.areamask[clientNum], entryList[idx].ctx->cl.snap.areamask, entryList[idx].ctx->areabytes );
					mergedCtx.ctx.areabytes = entryList[idx].ctx->areabytes;
					mergedCtx.messageExtraByte[clientNum] = entryList[idx].ctx->messageExtraByte;
				}
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
				if ( entryCtx->serverCommandSeq[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] != commandNum + 1 )
					continue;
				char* command = entryCtx->serverCommands[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
				// search for place to insert it
				bool newCommand = qtrue;
				int start = Q_max( firstServerCommand, entryList[idx].serverCommandSyncIdx );
				if ( start == 0 ) {
					// normally start at sync idx even if it's in a previous frame, to account for frame drops
					start = firstServerCommand;
				}
				bool isGamestateCmd = ( entryList[idx].metadata->lastGamestate != nullptr && commandNum <= entryList[idx].metadata->lastGamestate->serverCommandSequence && framesSaved == 0 );
				int searchEnd = isGamestateCmd ? ( firstServerCommandToExecute - 1 ) : ctx->clc.serverCommandSequence;
				for ( int snapCmdNum = start; snapCmdNum <= searchEnd; snapCmdNum++ ) {
					if ( !Q_strncmp( command, ctx->serverCommands[snapCmdNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )], MAX_STRING_CHARS ) ) {
						newCommand = qfalse;
						mergedCtx.serverCommandBitmask[snapCmdNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] |= 1 << entryList[idx].ctx->clc.clientNum;
						curCommand = snapCmdNum + 1;
						entryList[idx].serverCommandSyncIdx = curCommand;
						break;
					}
				}
				if ( newCommand ) {
					// insert it at index curCommand
					ctx->clc.serverCommandSequence++;
					ctx->clc.lastExecutedServerCommand++;
					if ( isGamestateCmd ) {
						// don't execute this one, it arrived with the gamestate
						firstServerCommandToExecute++;
						if ( curCommand >= firstServerCommandToExecute ) {
							Com_Printf( "Command ordering is wrong!\n" );
						}
					}
					for ( int snapCmdNum = ctx->clc.serverCommandSequence; snapCmdNum > curCommand; snapCmdNum-- ) {
						{
							char* dest = ctx->serverCommands[snapCmdNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
							char* src = ctx->serverCommands[( snapCmdNum - 1 ) & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
							memmove( dest, src, MAX_STRING_CHARS );
						}
						{
							int* maskDest = &mergedCtx.serverCommandBitmask[snapCmdNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
							int* maskSrc = &mergedCtx.serverCommandBitmask[( snapCmdNum - 1 ) & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
							*maskDest = *maskSrc;
						}
					}
					Q_strncpyz( ctx->serverCommands[curCommand & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )], command, MAX_STRING_CHARS );
					mergedCtx.serverCommandBitmask[curCommand & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] = 1 << entryList[idx].ctx->clc.clientNum;
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
		auto applyCsCommandToGameState = []( gameState_t *gs, char *command, char *bigConfigString ) {
			if ( !( ( command[0] == 'c' && command[1] == 's' ) || ( command[0] == 'b' && command[1] == 'c' && command[2] == 's' ) ) ) {
				return;
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
				gameState_t savedGs = ctx->cl.gameState;
				ctx->cl.gameState = *gs;
				CL_ConfigstringModified();
				*gs = ctx->cl.gameState;
				ctx->cl.gameState = savedGs;
			}
		};
		for ( int matchIdx = 0; matchIdx < mergedCtx.numMatches; matchIdx++ ) {
			idx = mergedCtx.matches[matchIdx];
			demoContext_t* entryCtx = entryList[idx].ctx;
			if ( !entryList[idx].splitterSeqInitialized ) {
				entryList[idx].splitterGameState = mergedCtx.ctx.cl.gameState;
				entryList[idx].splitterSeqInitialized = qtrue;
			}
			int firstDemoCmd = entryCtx->clc.lastExecutedServerCommand;
			char demoBigConfigString[BIG_INFO_STRING];
			demoBigConfigString[0] = '\0';
			for ( int g = 0; g < entryList[idx].numInitialGamestates; g++ ) {
				gamestateMetadata_t* gsMeta = entryList[idx].pendingGamestatePtrs[g];
				gameState_t* targetGs = &entryList[idx].initialGameStates[g];
				for ( int commandNum = firstDemoCmd; commandNum <= gsMeta->serverCommandSequence; commandNum++, firstDemoCmd++ ) {
					if ( entryList[idx].framesSaved > 0 && entryCtx->serverCommandSeq[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] == commandNum + 1 ) {
						char* command = entryCtx->serverCommands[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
						applyCsCommandToGameState( &entryList[idx].splitterGameState, command, demoBigConfigString );
					}
				}
				if ( entryList[idx].splitterGameState.dataCount != targetGs->dataCount
					|| memcmp( entryList[idx].splitterGameState.stringData, targetGs->stringData, entryList[idx].splitterGameState.dataCount )
					|| memcmp( entryList[idx].splitterGameState.stringOffsets, targetGs->stringOffsets, sizeof( entryList[idx].splitterGameState.stringOffsets ) ) ) {
					Com_Printf( "configstrings differ!\n" );
					for ( int i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
						char* orig = targetGs->stringData + targetGs->stringOffsets[i];
						char* combined = entryList[idx].splitterGameState.stringData + entryList[idx].splitterGameState.stringOffsets[i];
						if ( Q_strncmp( orig, combined, MAX_STRING_CHARS ) ) {
							Com_Printf( "configstring %d: orig %s, but we have %s\n", i, orig, combined );
							AddConfigStringOverride( gsMeta, i, orig );
							gameState_t savedGs = ctx->cl.gameState;
							ctx->cl.gameState = entryList[idx].splitterGameState;
							Cmd_TokenizeString( va( "cs %d \"%s\"\n", i, orig ) );
							CL_ConfigstringModified();
							entryList[idx].splitterGameState = ctx->cl.gameState;
							ctx->cl.gameState = savedGs;
						}
					}
				}
			}
			entryList[idx].numInitialGamestates = 0;
			for ( int commandNum = firstDemoCmd; commandNum <= entryCtx->clc.serverCommandSequence; commandNum++ ) {
				if ( entryCtx->serverCommandSeq[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )] == commandNum + 1 ) {
					char* command = entryCtx->serverCommands[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
					applyCsCommandToGameState( &entryList[idx].splitterGameState, command, demoBigConfigString );
				}
			}
			entryList[idx].framesSaved++;
		}
		if ( cctx->numDemos > cctx->numHandledDemos || cctx->numGamestates > cctx->numHandledGamestates ) {
			writeMergedGamestateData( outFile );
		}
		char bigConfigString[BIG_INFO_STRING];
		for ( int commandNum = firstServerCommand; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
			char* command = ctx->serverCommands[commandNum & ( MAX_BUFFERED_SERVER_COMMANDS - 1 )];
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
			static int					snapNoDeltaForced[MAX_GENTITIES];

			int32_t *owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];
			mergedCtx.ent_owner_idx ^= 1;
			int32_t *prev_owners = mergedCtx.ent_owners[mergedCtx.ent_owner_idx];

			// clear everything in this snapshot
			Com_Memset( frame->areamask, 0, sizeof( frame->areamask ) );
			memset( owners, 0, sizeof( *owners ) * MAX_GENTITIES );

			frame->numEntities = 0;
			mergedCtx.numEntityOverrides = 0;
			mergedCtx.entityOverrideClientMask = 0;

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
						snapNoDeltaForced[entNum] = 0;
					} else if ( !( owners[entNum] & clientBit ) && memcmp( ent, snapEnts[entNum], sizeof( entityState_t ) ) != 0 ) {
						AddEntityOverride( &mergedCtx, clientNum, entNum, snapEnts[entNum], ent );
					}
					owners[entNum] |= clientBit;
					if ( floatForced->number ) {
						entityState_t *toFloatForced = &snapFloatForced[entNum];
						if ( floatForced->number == 2 ) {
							snapNoDeltaForced[entNum] |= clientBit;
						} else {
							for ( int i = 0; i < (int) ARRAY_LEN( entityStateFields ); i++ ) {
								size_t offset = entityStateFields[i].offset;
								if ( *(int*)( (byte*)floatForced + offset ) == 1 ) {
									*(int*)( (byte*)toFloatForced + offset ) |= clientBit;
								}
							}
						}
						toFloatForced->number |= clientBit;
					}
				}
				for ( int r = 0; r < dctx->numRemovedEntities; r++ ) {
					int remEnt = dctx->removedEntities[r];
					AddEntityOverride( &mergedCtx, clientNum, remEnt, &mergedCtx.ctx.cl.entityBaselines[remEnt], NULL );
				}
				dctx->numRemovedEntities = 0;

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
						cctx->parseEntitiesNoDeltaForced[dstIdx] = snapNoDeltaForced[entIdx];
					} else {
						if ( floatForced->number ) {
							Com_Memset( floatForced, 0, sizeof( *floatForced ) );
						}
						cctx->parseEntitiesNoDeltaForced[dstIdx] = 0;
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

		for ( int tIdx = 0; tIdx < cctx->numDemos; tIdx++ ) {
			if ( cctx->demos[tIdx].eos && !cctx->demos[tIdx].tailSent && cctx->demos[tIdx].tailLen > 65536 ) {
				writeTailData( outFile );
				break;
			}
		}
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
		free( entryList[idx].initialGamestates );
		free( entryList[idx].initialGameStates );
		free( entryList[idx].pendingGamestatePtrs );
	}
	free( entryList );
	FreeCombinedGamestates( cctx );
	free( cctx->demos );
	cctx->demos = NULL;
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
