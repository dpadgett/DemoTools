// demo_merger.cpp : Defines the entry point for the console application.
//

#include "deps.h"
#include "client/client.h"
#include "demo_utils.h"
#include "demo_common.h"
#include "utils.h"

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

typedef struct {
	char filename[MAX_OSPATH];
	fileHandle_t fp;
	int startMillis;
	int endMillis;
	demoContext_t *ctx;
	int firstServerCommand;
	int framesSaved;
	qboolean firstRecord;
	char *lastTeamInfo;
	int targetMap;
	int *currentMap;
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
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		int lastServerId = ctx->cl.serverId;
		try {
			CL_ParseServerMessage( msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			FreeMsg( msg );
			return nullptr;
		}

		if ( !ctx->cl.newSnapshots ) {
			FreeMsg( msg );
			continue;
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
				CL_ConfigstringModified();
			} else if ( !strcmp( cmd, "tinfo" ) ) {
				Q_strncpyz( demo->lastTeamInfo, command, MAX_STRING_CHARS );
			}
		}
		if ( demo->firstServerCommand == ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10 && firstNonEmptyCommand > 0 ) {
			demo->firstServerCommand = firstNonEmptyCommand;
		}
		ctx->cl.serverId = atoi( Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SYSTEMINFO ], "sv_serverid" ) );
		if ( lastSnapValid && /*( ( lastSnapFlags ^ ctx->cl.snap.snapFlags ) & SNAPFLAG_SERVERCOUNT ) != 0*/ ctx->cl.serverId != lastServerId ) {
			(*demo->currentMap)++;
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
	while ( ( msg = ReadNextMessage( demo ) ) != nullptr ) {
		if ( getCurrentTime() > demo->startMillis && *demo->currentMap >= demo->targetMap ) {
			ctx = oldCtx;
			return qtrue;
		}
		FreeMsg( msg );
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

int RunMerge(int argc, char** argv)
{
	int entryListSize = -1;
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");
	if ( argc % 3 != 2 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" [filename.dm_26 mapidx starthh:mm:ss.sss] outfile.dm_26\n"
				"Note: inputs should be sorted in increasing order of start time\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	entryListSize = argc / 3;
	demoEntry_t *entryList = (demoEntry_t *) calloc( entryListSize, sizeof( demoEntry_t ) );
	{ // debugger fucks up idx without this
		for ( int idx = 0; idx < entryListSize; idx++ ) {
			int startTime = ParseTime( argv[idx * 3 + 3] );
			if ( startTime == -1 ) {
				return -1;
			}
			entryList[idx].startMillis = startTime;
			if ( idx > 0 ) {
				entryList[idx - 1].endMillis = startTime;
			}
			entryList[idx].targetMap = atoi( argv[idx * 3 + 2] );
			Q_strncpyz( entryList[idx].filename, argv[idx * 3 + 1], sizeof( entryList[idx].filename ) );
			// search for any already existing instance of this demo
			for ( int j = 0; j < idx; j++ ) {
				if ( !strcmp( entryList[j].filename, entryList[idx].filename ) ) {
					entryList[idx].fp = entryList[j].fp;
					entryList[idx].ctx = entryList[j].ctx;
					entryList[idx].lastTeamInfo = entryList[j].lastTeamInfo;
					entryList[idx].currentMap = entryList[j].currentMap;
					break;
				}
			}
			if ( !entryList[idx].fp ) {
				FS_FOpenFileRead( entryList[idx].filename, &entryList[idx].fp, qfalse );
				if ( !entryList[idx].fp ) {
					printf( "Failed to open file %s\n", entryList[idx].filename );
					return -1;
				}
				entryList[idx].ctx = (demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
				entryList[idx].lastTeamInfo = (char *) calloc( MAX_STRING_CHARS, 1 );
				entryList[idx].currentMap = (int *) calloc( 1, sizeof( int ) );
				entryList[idx].firstRecord = qtrue;
			}
		}
	}

	char *outFilename = argv[argc - 1];
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
	int idx = 0;
	if ( !ParseDemoContext( &entryList[idx] ) ) {
		printf( "Failed to parse demo context for demo %s, index %d\n", entryList[idx].filename, idx );
		return -1;
	}
	demoEntry_t *entry = &entryList[idx];
	ctx = entry->ctx;
	writeDemoHeader( outFile );
	// needed to keep sequencing
	int serverMessageSequence = ctx->clc.serverMessageSequence;
	int reliableAcknowledge = ctx->clc.reliableAcknowledge;
	int serverCommandOffset = 0;
	while ( idx < entryListSize ) {
		entry = &entryList[idx];
		ctx = entry->ctx;
		if ( ( entry->endMillis > 0 && getCurrentTime() > entry->endMillis ) || *entry->currentMap != entry->targetMap ) {
			idx++;
			if ( idx >= entryListSize ) break;
			if ( !ParseDemoContext( &entryList[idx] ) ) {
				printf( "Failed to parse demo context for demo %s, index %d\n", entryList[idx].filename, idx );
				return -1;
			}
			//printf( "Processing cut %d\n", idx );
			continue;
		}
		int origMessageSequence = ctx->clc.serverMessageSequence;
		ctx->clc.serverMessageSequence = ++serverMessageSequence;
		int origReliableAcknowledge = ctx->clc.reliableAcknowledge;
		ctx->clc.reliableAcknowledge = reliableAcknowledge; // this doesn't need to be bumped
		int origPmFlags = ctx->cl.snap.ps.pm_flags;
		ctx->cl.snap.ps.pm_flags |= PMF_FOLLOW;
		if ( entry->framesSaved > 0 ) {
			writeDeltaSnapshot( entry->firstServerCommand, outFile, qfalse, serverCommandOffset );
			entry->framesSaved++;
		} else {
			if ( idx > 0 ) {
				demoEntry_t *lastEntry = &entryList[idx - 1];
				demoContext_t *lastCtx = lastEntry->ctx;
				// send the last tinfo update if we have one
				if ( *entry->lastTeamInfo ) {
					Q_strncpyz( ctx->clc.serverCommands[ ( entry->firstServerCommand - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 ) ], entry->lastTeamInfo, sizeof( *ctx->clc.serverCommands ));
					entry->firstServerCommand--;
				}
				// configstrings might need to change
				// configstrings
				for ( int i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
					if ( !ctx->cl.gameState.stringOffsets[i] ) {
						continue;
					}
					char *s = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[i];
					char *lasts = lastCtx->cl.gameState.stringData + lastCtx->cl.gameState.stringOffsets[i];
					if ( strcmp( s, lasts ) ) {
						Com_sprintf( ctx->clc.serverCommands[ ( entry->firstServerCommand - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 ) ], sizeof( *ctx->clc.serverCommands ), "cs %d %s", i, s );
						entry->firstServerCommand--;
					}
				}
				serverCommandOffset += lastEntry->firstServerCommand - entry->firstServerCommand;
			}
			writeDeltaSnapshot( entry->firstServerCommand, outFile, qtrue, serverCommandOffset );
			// copy rest
			entry->framesSaved = 1;
		}
		// reset the sequencing for parsing
		ctx->clc.serverMessageSequence = origMessageSequence;
		ctx->clc.reliableAcknowledge = origReliableAcknowledge;
		ctx->cl.snap.ps.pm_flags = origPmFlags;
		msg_t *msg = ReadNextMessage( entry );
		if ( msg == nullptr ) {
			break;
		}
		FreeMsg( msg );
	}

	{
		// finish up
		int len = -1;
		fwrite (&len, 4, 1, outFile);
		fwrite (&len, 4, 1, outFile);
	}

	for ( idx = 0; idx < entryListSize; idx++ ) {
		if ( entryList[idx].firstRecord ) {
			FS_FCloseFile( entryList[idx].fp );
			free( entryList[idx].ctx );
			free( entryList[idx].lastTeamInfo );
			free( entryList[idx].currentMap );
		}
	}
	free( entryList );
	fclose( outFile );

	return 0;
}

#define MAX_CUTS 1000

int main(int argc, char** argv)
{
	if ( argc != 3 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" infilescript.dm_script outfile.dm_26\n"
				"Script format: [\"filename.dm_26\" starthh:mm:ss.sss]\n"
				"Note: inputs should be sorted in increasing order of start time\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	FILE *script = fopen( argv[1], "r" );
	if ( !script ) {
		printf( "Failed to open script file %s\n", argv[1] );
		return -1;
	}
	char *cutargs[MAX_CUTS * 2 + 2];
	cutargs[0] = argv[0];
	int numCuts;
	for ( numCuts = 0; numCuts < MAX_CUTS && !feof( script ); numCuts++ ) {
		char *line = (char *) calloc( MAX_STRING_CHARS, sizeof(char) );
		fgets( line, MAX_STRING_CHARS, script );
		if ( !*line ) {
			break;
		}
		line[strlen(line) - 1] = 0;
		int linelen = strlen( line );
		line[linelen - 13] = 0;
		char *mapidx = strrchr( line, ' ' );
		if ( mapidx == nullptr ) {
			printf( "Invalid line %s, expected map index offset\n", line );
			return -1;
		}
		mapidx[0] = 0;
		cutargs[numCuts * 3 + 1] = line;
		cutargs[numCuts * 3 + 2] = &mapidx[1];
		cutargs[numCuts * 3 + 3] = &line[linelen - 12];
	}
	fclose( script );
	cutargs[numCuts * 3 + 1] = argv[2];
	RunMerge( numCuts * 3 + 2, cutargs );
	for ( int cutIdx = 0; cutIdx < numCuts; cutIdx++ ) {
		free( cutargs[cutIdx * 3 + 1] );
	}
}
