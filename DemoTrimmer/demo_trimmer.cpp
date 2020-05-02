// DemoTrimmer.cpp : Defines the entry point for the console application.
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

int main(int argc, char** argv)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");
	if ( argc < 5 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" filename.dm_26 outfile.dm_26 starthh:mm:ss.sss endhh:mm:ss.sss\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	if ( strlen( argv[3] ) != 12 ) {
		printf( "Invalid start time format %s.  Must be hh:mm:ss.sss\n", argv[3] );
		return -1;
	}
	argv[3][2] = argv[3][5]  = argv[3][8] = '\0';
	int startTime = atoi( argv[3] );
	startTime = ( startTime * 60 ) + atoi( argv[3] + 3 );
	startTime = ( startTime * 60 ) + atoi( argv[3] + 6 );
	startTime = ( startTime * 1000 ) + atoi( argv[3] + 9 );

	if ( strlen( argv[4] ) != 12 ) {
		printf( "Invalid end time format %s.  Must be hh:mm:ss.sss\n", argv[4] );
		return -1;
	}
	argv[4][2] = argv[4][5]  = argv[4][8] = '\0';
	int endTime = atoi( argv[4] );
	endTime = ( endTime * 60 ) + atoi( argv[4] + 3 );
	endTime = ( endTime * 60 ) + atoi( argv[4] + 6 );
	endTime = ( endTime * 1000 ) + atoi( argv[4] + 9 );

	char *filename = argv[1];
	fileHandle_t fp;
	FS_FOpenFileRead( filename, &fp, qfalse );
	if ( !fp ) {
		printf( "File %s not found.\n", filename );
		//system( "pause" );
		return -1;
	}

	char *outFilename = argv[2];
	FILE *metaFile;
	if ( !Q_strncmp( outFilename, "-", 2 ) ) {
		metaFile = stdout;
#ifdef WIN32
		setmode(fileno(stdout), O_BINARY);
#else
		//freopen( NULL, "wb", stdout );
    // apparently this isn't necessary on linux :?
#endif
	} else {
		metaFile = fopen( outFilename, "wb" );
	}
	if ( !metaFile ) {
		printf( "Couldn't open output file\n" );
		return -1;
	}

	int framesSaved = 0;
	qboolean demoFinished = qfalse;
	while ( !demoFinished ) {
		msg_t msg;
		byte msgData[ MAX_MSGLEN ];
		MSG_Init( &msg, msgData, sizeof( msgData ) );
		demoFinished = CL_ReadDemoMessage( fp, &msg ) ? qfalse : qtrue;
		if ( demoFinished ) {
			break;
		}
		try {
			CL_ParseServerMessage( &msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			break;
		}

		if ( !ctx->cl.newSnapshots ) {
			continue;
		}
		int firstServerCommand = ctx->clc.lastExecutedServerCommand;
		// process any new server commands
		for ( ; ctx->clc.lastExecutedServerCommand < ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
			char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( cmd[0] ) {
				firstServerCommand = ctx->clc.lastExecutedServerCommand;
			}
			if ( !strcmp( cmd, "cs" ) ) {
				CL_ConfigstringModified();
			}
		}

		if ( getCurrentTime() > endTime ) {
			// finish up
			int len = -1;
			fwrite (&len, 4, 1, metaFile);
			fwrite (&len, 4, 1, metaFile);
			break;
		} else if ( framesSaved > 0 ) {
			if ( framesSaved > Q_max( 10, ctx->cl.snap.messageNum - ctx->cl.snap.deltaNum ) ) {
				CL_WriteDemoMessage( &msg, 0, metaFile );
			} else {
				writeDeltaSnapshot( firstServerCommand, metaFile, qfalse );
			}
			framesSaved++;
		} else if ( getCurrentTime() > startTime ) {
			writeDemoHeader( metaFile );
			writeDeltaSnapshot( firstServerCommand, metaFile, qtrue );
			// copy rest
			framesSaved = 1;
		}
	}

	/*int bufSize = 1024 * 10;
	char *buf = (char *) calloc( bufSize, 1 );
	int lenRead = 0;

	// the rest should just work.  in theory lol.
	while ( ( lenRead = FS_Read( buf, bufSize, fp ) ) > 0 ) {
		fwrite( buf, 1, lenRead, metaFile );
	}

	free( buf );*/

	FS_FCloseFile( fp );
	fclose( metaFile );

	return 0;
}

