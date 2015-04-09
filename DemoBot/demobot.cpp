// DemoTrimmer.cpp : Defines the entry point for the console application.
//

#include "deps.h"
#include "moredeps.h"
#include "client/client.h"

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

void writeDemoHeader(FILE *fp) {
	byte		bufData[MAX_MSGLEN];
	msg_t	buf;
	int			i;
	int			len;
	entityState_t	*ent;
	entityState_t	nullstate;
	char		*s;

	// write out the gamestate message
	MSG_Init (&buf, bufData, sizeof(bufData));
	MSG_Bitstream(&buf);

	// NOTE, MRE: all server->client messages now acknowledge
	MSG_WriteLong( &buf, clc.reliableSequence );

	MSG_WriteByte (&buf, svc_gamestate);
	MSG_WriteLong (&buf, clc.serverCommandSequence );

	// configstrings
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( !cl.gameState.stringOffsets[i] ) {
			continue;
		}
		s = cl.gameState.stringData + cl.gameState.stringOffsets[i];
		MSG_WriteByte (&buf, svc_configstring);
		MSG_WriteShort (&buf, i);
		MSG_WriteBigString (&buf, s);
	}

	// baselines
	Com_Memset (&nullstate, 0, sizeof(nullstate));
	for ( i = 0; i < MAX_GENTITIES ; i++ ) {
		ent = &cl.entityBaselines[i];
		if ( !ent->number ) {
			continue;
		}
		MSG_WriteByte (&buf, svc_baseline);
		MSG_WriteDeltaEntity (&buf, &nullstate, ent, qtrue );
	}

	MSG_WriteByte( &buf, svc_EOF );

	// finished writing the gamestate stuff

	// write the client num
	MSG_WriteLong(&buf, clc.clientNum);
	// write the checksum feed
	MSG_WriteLong(&buf, clc.checksumFeed);

	// RMG stuff
	/*if ( clc.rmgHeightMapSize )
	{
		int i;

		// Height map
		MSG_WriteShort ( &buf, (unsigned short)clc.rmgHeightMapSize );
		MSG_WriteBits ( &buf, 0, 1 );
		MSG_WriteData( &buf, clc.rmgHeightMap, clc.rmgHeightMapSize );

		// Flatten map
		MSG_WriteShort ( &buf, (unsigned short)clc.rmgHeightMapSize );
		MSG_WriteBits ( &buf, 0, 1 );
		MSG_WriteData( &buf, clc.rmgFlattenMap, clc.rmgHeightMapSize );

		// Seed
		MSG_WriteLong ( &buf, clc.rmgSeed );

		// Automap symbols
		MSG_WriteShort ( &buf, (unsigned short)clc.rmgAutomapSymbolCount );
		for ( i = 0; i < clc.rmgAutomapSymbolCount; i ++ )
		{
			MSG_WriteByte ( &buf, (unsigned char)clc.rmgAutomapSymbols[i].mType );
			MSG_WriteByte ( &buf, (unsigned char)clc.rmgAutomapSymbols[i].mSide );
			MSG_WriteLong ( &buf, (long)clc.rmgAutomapSymbols[i].mOrigin[0] );
			MSG_WriteLong ( &buf, (long)clc.rmgAutomapSymbols[i].mOrigin[1] );
		}
	}
	else*/
	{
		MSG_WriteShort ( &buf, 0 );
	}

	// finished writing the client packet
	MSG_WriteByte( &buf, svc_EOF );

	// write it to the demo file
	len = LittleLong( clc.serverMessageSequence - 1 );
	fwrite (&len, 1, 4, fp);

	len = LittleLong (buf.cursize);
	fwrite (&len, 4, 1, fp);
	fwrite (buf.data, 1, buf.cursize, fp);

	// the rest of the demo file will be copied from net messages
}

/*
=============
SV_EmitPacketEntities

Writes a delta update of an entityState_t list to the message.
=============
*/
static void SV_EmitPacketEntities( clSnapshot_t *from, clSnapshot_t *to, msg_t *msg ) {
	entityState_t	*oldent, *newent;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		from_num_entities;

	// generate the delta update
	if ( !from ) {
		from_num_entities = 0;
	} else {
		from_num_entities = from->numEntities;
	}

	newent = NULL;
	oldent = NULL;
	newindex = 0;
	oldindex = 0;
	while ( newindex < to->numEntities || oldindex < from_num_entities ) {
		if ( newindex >= to->numEntities ) {
			newnum = 9999;
		} else {
			newent = &cl.parseEntities[(to->parseEntitiesNum+newindex) & (MAX_PARSE_ENTITIES-1)];
			newnum = newent->number;
		}

		if ( oldindex >= from_num_entities ) {
			oldnum = 9999;
		} else {
			oldent = &cl.parseEntities[(from->parseEntitiesNum+oldindex) & (MAX_PARSE_ENTITIES-1)];
			oldnum = oldent->number;
		}

		if ( newnum == oldnum ) {
			// delta update from old position
			// because the force parm is qfalse, this will not result
			// in any bytes being emited if the entity has not changed at all
			MSG_WriteDeltaEntity (msg, oldent, newent, qfalse );
			oldindex++;
			newindex++;
			continue;
		}

		if ( newnum < oldnum ) {
			// this is a new entity, send it from the baseline
			MSG_WriteDeltaEntity (msg, &cl.entityBaselines[newnum], newent, qtrue );
			newindex++;
			continue;
		}

		if ( newnum > oldnum ) {
			// the old entity isn't present in the new message
			MSG_WriteDeltaEntity (msg, oldent, NULL, qtrue );
			oldindex++;
			continue;
		}
	}

	MSG_WriteBits( msg, (MAX_GENTITIES-1), GENTITYNUM_BITS );	// end of packetentities
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length
====================
*/
void CL_WriteDemoMessage ( msg_t *msg, int headerBytes, FILE *fp ) {
	int		len, swlen;

	// write the packet sequence
	len = clc.serverMessageSequence;
	swlen = LittleLong( len );
	fwrite (&swlen, 4, 1, fp);

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong(len);
	fwrite (&swlen, 4, 1, fp);
	fwrite ( msg->data + headerBytes, 1, len, fp );
}

void writeNonDeltaSnapshot(int firstServerCommand, FILE *fp) {
	msg_t				msgImpl, *msg = &msgImpl;
	byte				msgData[MAX_MSGLEN];
	clSnapshot_t		*frame;
	int					lastframe = 0;
	int					snapFlags;

	MSG_Init( msg, msgData, sizeof( msgData ) );
	MSG_Bitstream( msg );

	MSG_WriteLong( msg, clc.reliableSequence );

	// copy over any commands
	for ( int serverCommand = firstServerCommand; serverCommand < clc.serverCommandSequence; serverCommand++ ) {
		char *command = clc.serverCommands[ serverCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		MSG_WriteByte( msg, svc_serverCommand );
		MSG_WriteLong( msg, serverCommand );
		MSG_WriteString( msg, command );
	}

	// this is the snapshot we are creating
	frame = &cl.snap;

	MSG_WriteByte (msg, svc_snapshot);

	// NOTE, MRE: now sent at the start of every message from server to client
	// let the client know which reliable clientCommands we have received
	//MSG_WriteLong( msg, client->lastClientCommand );

	// send over the current server time so the client can drift
	// its view of time to try to match
	MSG_WriteLong (msg, frame->serverTime);

	// what we are delta'ing from
	MSG_WriteByte (msg, lastframe);

	snapFlags = frame->snapFlags;
	MSG_WriteByte (msg, snapFlags);

	// send over the areabits
	MSG_WriteByte (msg, sizeof( frame->areamask ));
	MSG_WriteData (msg, frame->areamask, sizeof( frame->areamask ));

	// delta encode the playerstate
#ifdef _ONEBIT_COMBO
	MSG_WriteDeltaPlayerstate( msg, NULL, &frame->ps, NULL, NULL );
#else
	MSG_WriteDeltaPlayerstate( msg, NULL, &frame->ps );
#endif
	if (frame->ps.m_iVehicleNum)
	{ //then write the vehicle's playerstate too
#ifdef _ONEBIT_COMBO
		MSG_WriteDeltaPlayerstate( msg, NULL, &frame->vps, NULL, NULL, qtrue );
#else
		MSG_WriteDeltaPlayerstate( msg, NULL, &frame->vps, qtrue );
#endif
	}

	// delta encode the entities
	SV_EmitPacketEntities (NULL, frame, msg);

	MSG_WriteByte( msg, svc_EOF );

	CL_WriteDemoMessage( msg, 0, fp );
}

long getCurrentTime() {
	return cl.snap.serverTime - atoi( cl.gameState.stringData + cl.gameState.stringOffsets[ CS_LEVEL_START_TIME ] );
}

void connect(char *server) {
	char command[MAX_STRING_CHARS];
	Com_sprintf( command, sizeof( command ), "connect %s", server );
	Cmd_TokenizeString( command );
	CL_Connect_f();
	while( cls.state != CA_CONNECTED ) {
		CL_CheckForResend();
	}
}

int main(int argc, char** argv)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");
	if ( argc < 3 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" server-ip:port outdir\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	int qport;
	// Pick a random port value
	Com_RandomBytes( (byte*)&qport, sizeof(int) );
	Netchan_Init( qport & 0xffff );	// pick a port value that should be nice and random

	char *server = argv[1];
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
		freopen( NULL, "wb", stdout );
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

		if ( !cl.newSnapshots ) {
			continue;
		}
		int firstServerCommand = clc.lastExecutedServerCommand;
		// process any new server commands
		for ( ; clc.lastExecutedServerCommand < clc.serverCommandSequence; clc.lastExecutedServerCommand++ ) {
			char *command = clc.serverCommands[ clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
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
			if ( framesSaved > 10 ) {
				CL_WriteDemoMessage( &msg, 0, metaFile );
			} else {
				writeNonDeltaSnapshot( firstServerCommand, metaFile );
			}
			framesSaved++;
		} else if ( getCurrentTime() > startTime ) {
			writeDemoHeader( metaFile );
			writeNonDeltaSnapshot( firstServerCommand, metaFile );
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
