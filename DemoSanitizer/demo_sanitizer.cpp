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



/*
==================
Info_SetValueForKey_Big

Changes or adds a key/value pair
Includes and retains zero-length values
==================
*/
void Info_SetValueForKey_BigInplace( char *s, const char *key, const char *newvalue ) {
	char	newi[BIG_INFO_STRING], orig[BIG_INFO_STRING];
	const char* blacklist = "\\;\"";

	if ( strlen( s ) >= BIG_INFO_STRING ) {
		Com_Error( ERR_DROP, "Info_SetValueForKey_Big: oversize infostring" );
	}

	for(; *blacklist; ++blacklist)
	{
		if (strchr (key, *blacklist) || strchr (newvalue, *blacklist))
		{
			Com_Printf (S_COLOR_YELLOW "Can't use keys or values with a '%c': %s = %s\n", *blacklist, key, newvalue);
			return;
		}
	}

	//Info_RemoveKey_Big (s, key);
	//if (!newvalue)
	//	return;

	Com_sprintf (newi, sizeof(newi), "\\%s\\%s", key, newvalue);


	char	*start, *param;
	static char	pkey[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	char	*o;

	pkey[0] = value[0] = '\0';

	if ( strlen( s ) >= BIG_INFO_STRING ) {
		Com_Error( ERR_DROP, "Info_RemoveKey_Big: oversize infostring" );
		return;
	}

  Q_strncpyz(orig, s, sizeof(orig));
  param = s;

	if (strchr (key, '\\')) {
		return;
	}

	while (1)
	{
		start = s;
		if (*s == '\\')
			s++;
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return;
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value;
		while (*s != '\\' && *s)
		{
			if (!*s)
				return;
			*o++ = *s++;
		}
		*o = 0;

		//OJKNOTE: static analysis pointed out pkey may not be null-terminated
		if (!strcmp (key, pkey) )
		{
      if (strlen(orig) - (s - start) + strlen(newi) >= BIG_INFO_STRING) {
        Com_Printf ("BIG Info string length exceeded\n");
        // just remove the original
        memmove(start, s, strlen(s) + 1);	// remove this part
        return;
      }
      memmove(start, newi, strlen(newi));
      memmove(start + strlen(newi), orig + (s - param), strlen(orig + (s - param)) + 1);
			return;
		}

		if (!*s)
			return;
	}


	if (strlen(newi) + strlen(s) >= BIG_INFO_STRING)
	{
		Com_Printf ("BIG Info string length exceeded\n");
		return;
	}

  strcat (s, newi);
}


extern	cvar_t	*cl_shownet;

static char *svc_strings[256] = {
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

static void SHOWNET( msg_t *msg, char *s) {
	if ( cl_shownet->integer >= 2) {
		Com_Printf ("%3i:%s\n", msg->readcount-1, s);
	}
}

// Strips the IP hash out of the given infostring.
// Returns true if the hash was found and stripped out.
qboolean StripIpHash(char *s) {
  char *id = Info_ValueForKey(s, "id");
  if (id[0] == '\0') {
    return qfalse;
  }
  fprintf( stderr, "Found id %s\n", id);
  char *p = NULL;
  uint64_t uniqueId = strtoull( id, &p, 10 );
  if ( errno == ERANGE || *p != 0 || p == id ) {
    uniqueId = 0;
  }
  int guid_hash = uniqueId & 0xFFFFFFFF;
  int ip_hash = uniqueId >> 32;
  fprintf( stderr, "Found ip %d, guid %d\n", ip_hash, guid_hash);
  ip_hash = 0;
  uniqueId = guid_hash & 0xFFFFFFFFull;
  fprintf( stderr, "Changing id to %llu\n", uniqueId);
  Info_SetValueForKey_BigInplace(s, "id", va("%llu", uniqueId));
  return qtrue;
}

// Replace a string in the given message.  msg's read position should be
// at the end of the string to be replaced.
void RewriteStrInMsg(msg_t *msg, char *s, int strstart, int strstartbit) {
  msg_t newmsg = *msg;
  unsigned char newdata[MAX_MSGLEN];
  newmsg.cursize = strstart;
  newmsg.bit = strstartbit;
  newmsg.data = newdata;
  memcpy(newdata, msg->data, msg->maxsize);
  MSG_WriteBigString( &newmsg, s );
  fprintf( stderr, "Old CS, new readcount %d, bit %d\n", msg->readcount, msg->bit);
  fprintf( stderr, "Wrote new CS, new cursize %d, new bit %d\n", newmsg.cursize, newmsg.bit);
  //int bitdiff = msg->bit - newmsg.bit;
  //printf("Bit diff %d modulus %d\n", bitdiff, bitdiff % 8);
  /*printf("Old: %d %d %d %d %x %x %x %x %x %x\n",
    msg->readcount,
    msg->bit,
    msg->cursize,
    msg->maxsize,
    msg->data[msg->readcount - 1],
    msg->data[msg->readcount],
    msg->data[msg->readcount + 1],
    msg->data[msg->readcount + 2], 
    msg->data[msg->readcount + 3], 
    msg->data[msg->readcount + 4]);*/
  msg_t tmpnew, tmpold;
  tmpnew = newmsg;
  tmpold = *msg;
  //for (int bitoff = tmpold.bit; bitoff < tmpold.cursize << 3; bitoff++) {
  for (int bitoff = tmpold.bit; bitoff < tmpold.cursize << 3 && (bitoff & 7) != 0; bitoff++) {
    int value = Huff_getBit( tmpold.data, &tmpold.bit );
    Huff_putBit((value&1), tmpnew.data, &tmpnew.bit);
  }
  // below added to speed up bit copying.  copy one byte at a time instead of one bit at a time.
  unsigned char startmask = (1 << (tmpnew.bit & 7)) - 1;
  unsigned char value = tmpnew.data[tmpnew.bit >> 3] & startmask;
  unsigned char newbitoff = tmpnew.bit & 7;
  while (tmpold.bit < (tmpold.cursize + (newbitoff > 0 ? 1 : 0)) << 3) {
    unsigned char newvalue = tmpold.data[tmpold.bit >> 3];
    tmpnew.data[tmpnew.bit >> 3] = value | (newvalue << newbitoff);
    tmpnew.bit += 8;
    tmpold.bit += 8;
    value = newvalue >> (8 - newbitoff);
  }
  tmpold.readcount = (tmpold.bit >> 3) + 1;
  tmpnew.cursize = (tmpnew.bit >> 3) + 1;
  memcpy(msg->data, tmpnew.data, msg->maxsize);
  msg->cursize = tmpnew.cursize;
  msg->readcount = newmsg.cursize;
  msg->bit = newmsg.bit;
  /*printf("New: %d %d %d %d %x %x %x %x %x %x\n",
    msg->readcount,
    msg->bit,
    msg->cursize,
    msg->maxsize,
    msg->data[msg->readcount - 1],
    msg->data[msg->readcount],
    msg->data[msg->readcount + 1],
    msg->data[msg->readcount + 2], 
    msg->data[msg->readcount + 3], 
    msg->data[msg->readcount + 4]);*/
}

/*
=====================
CL_ParseCommandString

Command strings are just saved off until cgame asks for them
when it transitions a snapshot
=====================
*/
void SanitizeCommandString( msg_t *msg ) {
	char	*cs, *s;
	int		seq;
	int		index;

	seq = MSG_ReadLong( msg );
  int strstart = msg->readcount;
  int strstartbit = msg->bit;
	s = MSG_ReadString( msg );
  //printf("Received command string %s\n", s);
  Cmd_TokenizeString( s );
  char *cmd = Cmd_Argv( 0 );
  if ( strcmp( cmd, "cs" ) ) {
    // not a cs command
    return;
  }
	index = atoi( Cmd_Argv(1) );
	// get everything after "cs <num>"
	cs = Cmd_ArgsFrom(2);
  if (index >= CS_PLAYERS && index < CS_PLAYERS+MAX_CLIENTS) {
    fprintf( stderr, "Received CS for client %i: %s (strlen: %d msglen: %d bits: %d)\n", index - CS_PLAYERS, cs, strlen(cs), msg->readcount - strstart, (msg->bit - strstartbit) >> 3);
    if (StripIpHash( cs )) {
      RewriteStrInMsg( msg, va( "cs %d \"%s\"\n", index, cs ), strstart, strstartbit );
    }
  }

	// see if we have already executed stored it off
	//if ( ctx->clc.serverCommandSequence >= seq ) {
	//	return;
	//}
	//ctx->clc.serverCommandSequence = seq;

	//index = seq & (MAX_RELIABLE_COMMANDS-1);
	//Q_strncpyz( ctx->clc.serverCommands[ index ], s, sizeof( ctx->clc.serverCommands[ index ] ) );
}

/*
==================
CL_ParseGamestate
==================
*/
void SanitizeGamestate( msg_t *msg ) {
	int				i;
	entityState_t	*es;
	int				newnum;
	entityState_t	nullstate, dummystate;
	int				cmd;
	char			*s;

	//ctx->clc.connectPacketCount = 0;

	//Com_Memset( &ctx->cl.gameState, 0, sizeof( ctx->cl.gameState ) );
	//Com_Memset( &ctx->cl.entityBaselines, 0, sizeof( ctx->cl.entityBaselines ) );

	// a gamestate always marks a server command sequence
	/*ctx->clc.serverCommandSequence =*/ MSG_ReadLong( msg );

	// parse all the configstrings and baselines
	//ctx->cl.gameState.dataCount = 1;	// leave a 0 at the beginning for uninitialized configstrings
	while ( 1 ) {
		cmd = MSG_ReadByte( msg );

		if ( cmd == svc_EOF ) {
			break;
		}

		if ( cmd == svc_configstring ) {
			int		len, start, strstart, strstartbit;

			start = msg->readcount;

			i = MSG_ReadShort( msg );
			if ( i < 0 || i >= MAX_CONFIGSTRINGS ) {
				Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );
			}
			strstart = msg->readcount;
			strstartbit = msg->bit;
			s = MSG_ReadBigString( msg );

			if (cl_shownet->integer >= 2)
			{
				Com_Printf("%3i: %d: %s\n", start, i, s);
			}

			len = strlen( s );

			if ( len + 1 + ctx->cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
			}

      if (i >= CS_PLAYERS && i < CS_PLAYERS+MAX_CLIENTS) {
        fprintf( stderr, "Received CS for client %i: %s (strlen: %d msglen: %d bits: %d)\n", i - CS_PLAYERS, s, len, msg->readcount - strstart, (msg->bit - strstartbit) >> 3);
        if (StripIpHash( s )) {
          RewriteStrInMsg( msg, s, strstart, strstartbit );
        }
      }
			// append it to the gameState string buffer
			//ctx->cl.gameState.stringOffsets[ i ] = ctx->cl.gameState.dataCount;
			//Com_Memcpy( ctx->cl.gameState.stringData + ctx->cl.gameState.dataCount, s, len + 1 );
			//ctx->cl.gameState.dataCount += len + 1;
		} else if ( cmd == svc_baseline ) {
			newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
			//if ( newnum < 0 || newnum >= MAX_GENTITIES ) {
			//	Com_Error( ERR_DROP, "Baseline number out of range: %i", newnum );
			//}
			Com_Memset (&nullstate, 0, sizeof(nullstate));
			//es = &ctx->cl.entityBaselines[ newnum ];
			MSG_ReadDeltaEntity( msg, &nullstate, &dummystate, newnum );
		} else {
			Com_Error( ERR_DROP, "CL_ParseGamestate: bad command byte" );
		}
	}

	/*ctx->clc.clientNum =*/ MSG_ReadLong(msg);
	// read the checksum feed
	/*ctx->clc.checksumFeed =*/ MSG_ReadLong( msg );

	//CL_ParseRMG ( msg ); //rwwRMG - get info for it from the server
	/*short rmgHeightMapSize = (unsigned short)*/MSG_ReadShort ( msg );
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void SanitizeServerMessage( msg_t *msg ) {
	int			cmd;

	if ( cl_shownet->integer == 1 ) {
		Com_Printf ("%i ",msg->cursize);
	} else if ( cl_shownet->integer >= 2 ) {
		Com_Printf ("------------------\n");
	}

	MSG_Bitstream(msg);

	// get the reliable sequence acknowledge number
	long reliableAcknowledge = MSG_ReadLong( msg );

	//
	// parse the message
	//
	while ( 1 ) {
		if ( msg->readcount > msg->cursize ) {
			Com_Error (ERR_DROP,"CL_ParseServerMessage: read past end of server message");
			break;
		}

		cmd = MSG_ReadByte( msg );

		if ( cmd == svc_EOF) {
			SHOWNET( msg, "END OF MESSAGE" );
			break;
		}

		if ( cl_shownet->integer >= 2 ) {
			if ( !svc_strings[cmd] ) {
				Com_Printf( "%3i:BAD CMD %i\n", msg->readcount-1, cmd );
			} else {
				SHOWNET( msg, svc_strings[cmd] );
			}
		}

	// other commands
		switch ( cmd ) {
		default:
      fprintf( stderr, "Unknown command %d\n", cmd);
			Com_Error (ERR_DROP,"CL_ParseServerMessage: Illegible server message\n");
			break;
		case svc_nop:
		case svc_mapchange:
			break;
		case svc_serverCommand:
			SanitizeCommandString( msg );
			break;
		case svc_gamestate:
			SanitizeGamestate( msg );
			break;
		case svc_snapshot:
		case svc_setgame:
		case svc_download:
			return;
		}
	}
}

int main(int argc, char** argv)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");
	if ( argc < 3 ) {
		fprintf( stderr, "No file specified.\n"
				"Usage: \"%s\" filename.dm_26 outfile.dm_26 starthh:mm:ss.sss endhh:mm:ss.sss\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	char *filename = argv[1];
	fileHandle_t fp;
	FS_FOpenFileRead( filename, &fp, qfalse );
	if ( !fp ) {
		fprintf( stderr, "File %s not found.\n", filename );
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
		fprintf( stderr, "Couldn't open output file\n" );
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
			SanitizeServerMessage( &msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			break;
		}

		CL_WriteDemoMessage( &msg, 0, metaFile );
	}

			// finish up
			int len = -1;
			fwrite (&len, 4, 1, metaFile);
			fwrite (&len, 4, 1, metaFile);

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

