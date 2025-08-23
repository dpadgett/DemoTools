// skeletal version.  populates snapshots and that's about it
#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "client/client.h"
#include "demo_common.h"

extern	cvar_t	*cl_shownet;

const char *svc_strings[256] = {
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

void SHOWNET( msg_t *msg, const char *s) {
	if ( cl_shownet->integer >= 2 ) {
		Com_Printf ("%3i:%s\n", msg->readcount-1, s);
	}
}

/*
=====================
CL_ParseCommandString

Command strings are just saved off until cgame asks for them
when it transitions a snapshot
=====================
*/
void CL_ParseCommandString( msg_t *msg, qboolean firstServerCommandInMessage ) {
	char	*s;
	int		seq;
	int		index;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	if ( firstServerCommandInMessage ) {
		ctx->serverReliableAcknowledge = seq - 1;
	}

	if ( cl_shownet->integer >= 1 /* && ctx->clc.clientNum == 2 */ ) {
		Com_Printf( "Parsed server command %d: %s\n", seq, s );
	}

	// see if we have already executed stored it off
	if ( ctx->clc.serverCommandSequence >= seq ) {
		return;
	}
	ctx->clc.serverCommandSequence = seq;

	index = seq & (MAX_RELIABLE_COMMANDS-1);
	Q_strncpyz( ctx->clc.serverCommands[ index ], s, sizeof( ctx->clc.serverCommands[ index ] ) );
}

void CL_ParseRMG ( msg_t* msg )
{
	short rmgHeightMapSize = (unsigned short)MSG_ReadShort ( msg );
	if ( !rmgHeightMapSize )
	{
		return;
	}

	Com_Error( ERR_DROP, "RMG loading not supported" );
}

/*
==================
CL_ParseGamestate
==================
*/
void CL_ParseGamestate( msg_t *msg,	qboolean firstServerCommandInMessage ) {
	int				i;
	entityState_t	*es;
	int				newnum;
	entityState_t	nullstate;
	int				cmd;
	char			*s;

	ctx->clc.connectPacketCount = 0;

	Com_Memset( &ctx->cl.gameState, 0, sizeof( ctx->cl.gameState ) );
	Com_Memset( &ctx->cl.entityBaselines, 0, sizeof( ctx->cl.entityBaselines ) );

	// a gamestate always marks a server command sequence
	ctx->clc.serverCommandSequence = MSG_ReadLong( msg );
	if ( firstServerCommandInMessage ) {
		ctx->serverReliableAcknowledge = ctx->clc.serverCommandSequence - 1;
	}

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

			if (cl_shownet->integer >= 2)
			{
				Com_Printf("%3i: %d: %s\n", start, i, s);
			}

			len = strlen( s );

			if ( len + 1 + ctx->cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
			}

			// append it to the gameState string buffer
			ctx->cl.gameState.stringOffsets[ i ] = ctx->cl.gameState.dataCount;
			Com_Memcpy( ctx->cl.gameState.stringData + ctx->cl.gameState.dataCount, s, len + 1 );
			ctx->cl.gameState.dataCount += len + 1;
		} else if ( cmd == svc_baseline ) {
			int start = msg->readcount;
			newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
			if ( newnum < 0 || newnum >= MAX_GENTITIES ) {
				Com_Error( ERR_DROP, "Baseline number out of range: %i", newnum );
			}
			Com_Memset (&nullstate, 0, sizeof(nullstate));
			es = &ctx->cl.entityBaselines[ newnum ];
			MSG_ReadDeltaEntity( msg, &nullstate, es, newnum );
		} else {
			Com_Error( ERR_DROP, "CL_ParseGamestate: bad command byte" );
		}
	}

	ctx->clc.clientNum = MSG_ReadLong(msg);
	// read the checksum feed
	ctx->clc.checksumFeed = MSG_ReadLong( msg );

	CL_ParseRMG ( msg ); //rwwRMG - get info for it from the server
}

/*
==================
CL_DeltaEntity

Parses deltas from the given base and adds the resulting entity
to the current frame
==================
*/
void MSG_ReadDeltaEntityWithFloats( msg_t* msg, entityState_t* from, entityState_t* to, entityState_t* floatForced,
	int number, qboolean isFloatForced );
void CL_DeltaEntity (msg_t *msg, clSnapshot_t *frame, int newnum, entityState_t *old,
					 qboolean unchanged) {
	entityState_t	*state, *floatForced;

	// save the parsed entity state into the big circular buffer so
	// it can be used as the source for a later delta
	state = &ctx->cl.parseEntities[ctx->cl.parseEntitiesNum & (MAX_PARSE_ENTITIES-1)];
	floatForced = &ctx->parseEntitiesFloatForced[ctx->cl.parseEntitiesNum & ( MAX_PARSE_ENTITIES - 1 )];

	if ( frame->serverTime == 413135 && newnum == 3 && frame->ps.commandTime == 413076 ) {
		Com_Printf( "WTF8\n" );
	}

	if ( unchanged )
	{
		*state = *old;
		Com_Memset( floatForced, 0, sizeof( *floatForced ) );
	}
	else
	{
		MSG_ReadDeltaEntityWithFloats( msg, old, state, floatForced, newnum, qfalse );
	}

	if ( state->number == (MAX_GENTITIES-1) ) {
		return;		// entity was delta removed
	}
	ctx->cl.parseEntitiesNum++;
	frame->numEntities++;
}

/*
==================
CL_ParsePacketEntities

==================
*/
void CL_ParsePacketEntities( msg_t *msg, clSnapshot_t *oldframe, clSnapshot_t *newframe) {
	int			newnum;
	entityState_t	*oldstate;
	int			oldindex, oldnum;

	newframe->parseEntitiesNum = ctx->cl.parseEntitiesNum;
	newframe->numEntities = 0;

	// delta from the entities present in oldframe
	oldindex = 0;
	oldstate = NULL;
	if (!oldframe) {
		oldnum = 99999;
	} else {
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &ctx->cl.parseEntities[
				(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
			oldnum = oldstate->number;
		}
	}

	while ( 1 ) {
		// read the entity index number
		newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );

		if ( newnum == (MAX_GENTITIES-1) ) {
			break;
		}

		if ( msg->readcount > msg->cursize ) {
			Com_Error (ERR_DROP,"CL_ParsePacketEntities: end of message");
		}

		while ( oldnum < newnum ) {
			// one or more entities from the old packet are unchanged
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  unchanged: %i\n", msg->readcount, oldnum);
			}
			if ( oldnum == 244 ) {
				//Com_Printf( "%d 244 unchanged at %d deltanum %d\n", ctx->clc.clientNum, newframe->serverTime, newframe->messageNum - oldframe->messageNum );
			}
			CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &ctx->cl.parseEntities[
					(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
				oldnum = oldstate->number;
			}
		}
		if (oldnum == newnum) {
			// delta from previous state
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  delta: %i\n", msg->readcount, newnum);
			}
			if ( newnum == 244 ) {
				//Com_Printf( "%d 244 delta at %d deltanum %d\n", ctx->clc.clientNum, newframe->serverTime, newframe->messageNum - oldframe->messageNum );
			}
			CL_DeltaEntity( msg, newframe, newnum, oldstate, qfalse );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &ctx->cl.parseEntities[
					(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
				oldnum = oldstate->number;
			}
			continue;
		}

		if ( oldnum > newnum ) {
			// delta from baseline
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  baseline: %i\n", msg->readcount, newnum);
			}
			if ( newnum == 244 ) {
				//Com_Printf( "%d 244 baseline at %d deltanum %d\n", ctx->clc.clientNum, newframe->serverTime, oldframe == NULL ? -1 : newframe->messageNum - oldframe->messageNum );
			}
			CL_DeltaEntity( msg, newframe, newnum, &ctx->cl.entityBaselines[newnum], qfalse );
			continue;
		}

	}

	// any remaining entities in the old frame are copied over
	while ( oldnum != 99999 ) {
		// one or more entities from the old packet are unchanged
		if ( cl_shownet->integer == 3 ) {
			Com_Printf ("%3i:  unchanged: %i\n", msg->readcount, oldnum);
		}
		if ( oldnum == 244 ) {
			//Com_Printf( "%d 244 unchanged at %d deltanum %d\n", ctx->clc.clientNum, newframe->serverTime, newframe->messageNum - oldframe->messageNum );
		}
		CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

		oldindex++;

		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &ctx->cl.parseEntities[
				(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
			oldnum = oldstate->number;
		}
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
void MSG_ReadDeltaPlayerstateWithForcedFields( msg_t* msg, playerState_t* from, playerState_t* to, playerState_t* forcedFields, qboolean isVehiclePS );
void CL_ParseSnapshot( msg_t *msg ) {
	int			len;
	clSnapshot_t	*old;
	clSnapshot_t	newSnap;
	int			deltaNum;
	int			oldMessageNum;
	int			i, packetNum;

	// get the reliable sequence acknowledge number
	// NOTE: now sent with all server to client messages
	//clc.reliableAcknowledge = MSG_ReadLong( msg );

	// read in the new snapshot to a temporary buffer
	// we will only copy to cl.snap if it is valid
	Com_Memset (&newSnap, 0, sizeof(newSnap));

	// we will have read any new server commands in this
	// message before we got to svc_snapshot
	newSnap.serverCommandNum = ctx->clc.serverCommandSequence;

	newSnap.serverTime = MSG_ReadLong( msg );
	SHOWNET( msg, va( "frame->serverTime %d", newSnap.serverTime ) );

	// if we were just unpaused, we can only *now* really let the
	// change come into effect or the client hangs.
	//cl_paused->modified = qfalse;

	newSnap.messageNum = ctx->clc.serverMessageSequence;

	deltaNum = MSG_ReadByte( msg );
	SHOWNET( msg, va( "lastframe %d", deltaNum ) );
	if ( !deltaNum ) {
		newSnap.deltaNum = -1;
	} else {
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	newSnap.snapFlags = MSG_ReadByte( msg );
	SHOWNET( msg, va( "snapFlags %d", newSnap.snapFlags ) );

	// If the frame is delta compressed from data that we
	// no longer have available, we must suck up the rest of
	// the frame, but not use it, then ask for a non-compressed
	// message
	if ( newSnap.deltaNum <= 0 ) {
		newSnap.valid = qtrue;		// uncompressed frame
		old = NULL;
		ctx->clc.demowaiting = qfalse;	// we can start recording now
	} else {
		old = &ctx->cl.snapshots[newSnap.deltaNum & PACKET_MASK];
		if ( !old->valid ) {
			// should never happen
			Com_Printf ("Delta from invalid frame (not supposed to happen!).\n");
			while ( ( newSnap.deltaNum & PACKET_MASK ) != ( newSnap.messageNum & PACKET_MASK ) && !old->valid ) {
				newSnap.deltaNum++;
				old = &ctx->cl.snapshots[newSnap.deltaNum & PACKET_MASK];
			}
			if ( old->valid ) {
				Com_Printf ("Found more recent frame to delta from.\n");
			}
		}
		if ( !old->valid ) {
			Com_Printf ("Failed to find more recent frame to delta from.\n");
		} else if ( old->messageNum != newSnap.deltaNum ) {
			// The frame that the server did the delta from
			// is too old, so we can't reconstruct it properly.
			Com_Printf ("Delta frame too old.\n");
		} else if ( ctx->cl.parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES-128 ) {
			Com_Printf ("Delta parseEntitiesNum too old.\n");
		} else {
			newSnap.valid = qtrue;	// valid delta parse
		}
	}

	// read areamask
	len = MSG_ReadByte( msg );

	if((unsigned)len > sizeof(newSnap.areamask))
	{
		Com_Error (ERR_DROP,"CL_ParseSnapshot: Invalid size %d for areamask", len);
		return;
	}

	ctx->areabytes = len;
	MSG_ReadData( msg, &newSnap.areamask, len);
	SHOWNET( msg, va( "frame->areamask %d", len ) );

	playerState_t* playerStateForcedFields = &ctx->playerStateForcedFields[newSnap.messageNum & PACKET_MASK];
	playerState_t* vehPlayerStateForcedFields = &ctx->vehPlayerStateForcedFields[newSnap.messageNum & PACKET_MASK];

	// read playerinfo
	SHOWNET( msg, "playerstate" );
	if ( old ) {
		MSG_ReadDeltaPlayerstateWithForcedFields( msg, &old->ps, &newSnap.ps, playerStateForcedFields, qfalse );
		if (newSnap.ps.m_iVehicleNum)
		{ //this means we must have written our vehicle's ps too
			MSG_ReadDeltaPlayerstateWithForcedFields( msg, &old->vps, &newSnap.vps, vehPlayerStateForcedFields, qtrue );
		}
	} else {
		MSG_ReadDeltaPlayerstateWithForcedFields( msg, NULL, &newSnap.ps, playerStateForcedFields, qfalse );
		if (newSnap.ps.m_iVehicleNum)
		{ //this means we must have written our vehicle's ps too
			MSG_ReadDeltaPlayerstateWithForcedFields( msg, NULL, &newSnap.vps, vehPlayerStateForcedFields, qtrue );
		}
	}

	// read packet entities
	SHOWNET( msg, "packet entities" );
	CL_ParsePacketEntities( msg, old, &newSnap );

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
	for ( ; oldMessageNum < newSnap.messageNum ; oldMessageNum++ ) {
		ctx->cl.snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
	}

	// copy to the current good spot
	ctx->cl.snap = newSnap;
	ctx->cl.snap.ping = 999;
	// calculate ping time
	for ( i = 0 ; i < PACKET_BACKUP ; i++ ) {
		packetNum = ( ctx->clc.netchan.outgoingSequence - 1 - i ) & PACKET_MASK;
		if ( ctx->cl.snap.ps.commandTime >= ctx->cl.outPackets[ packetNum ].p_serverTime ) {
			ctx->cl.snap.ping = ctx->cls.realtime - ctx->cl.outPackets[ packetNum ].p_realtime;
			break;
		}
	}
	// save the frame off in the backup array for later delta comparisons
	ctx->cl.snapshots[ctx->cl.snap.messageNum & PACKET_MASK] = ctx->cl.snap;

	if (cl_shownet->integer == 3) {
		Com_Printf( "   snapshot:%i  delta:%i  ping:%i\n", ctx->cl.snap.messageNum,
		ctx->cl.snap.deltaNum, ctx->cl.snap.ping );
	}

	ctx->cl.newSnapshots = qtrue;
}

// this one is dumb, so skip it
void CL_ParseSetGame( msg_t *msg )
{
	int i = 0;
	char next = 1;

	while (i < MAX_QPATH && next)
	{
		next = MSG_ReadByte( msg );
		i++;
	}
}

void CL_ParseDownload ( msg_t *msg ) {
	// pointless, we don't want to download anything...
	msg_t copy = *msg;
	MSG_WriteByte( &copy, svc_EOF );
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage( msg_t *msg ) {
	int			cmd;

	if ( cl_shownet->integer == 1 ) {
		Com_Printf ("%i ",msg->cursize);
	} else if ( cl_shownet->integer >= 2 ) {
		Com_Printf ("------------------\n");
	}

	if ( cl_shownet->integer >= 1 && ctx->clc.clientNum == 2 ) {
		Com_Printf( "Parsing message %d\n", ctx->clc.serverMessageSequence );
	}

	MSG_Bitstream(msg);

	// get the reliable sequence acknowledge number
	ctx->clc.reliableAcknowledge = MSG_ReadLong( msg );
	qboolean firstServerCommandInMessage = qtrue;

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
			int readBits = msg->bit & 7;
			int mask = ( ( 1 << readBits ) - 1 );
			mask ^= 0xff;
			ctx->messageExtraByte = msg->data[msg->readcount - 1] & mask;
			if ( cl_shownet->integer >= 3 ) {
				Com_Printf( "extraByte: %d\n", ctx->messageExtraByte );
			}
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
			Com_Error (ERR_DROP,"CL_ParseServerMessage: Illegible server message\n");
			break;
		case svc_nop:
			break;
		case svc_serverCommand:
			CL_ParseCommandString( msg, firstServerCommandInMessage );
			firstServerCommandInMessage = qfalse;
			break;
		case svc_gamestate:
			CL_ParseGamestate( msg, firstServerCommandInMessage );
			firstServerCommandInMessage = qfalse;
			break;
		case svc_snapshot:
			CL_ParseSnapshot( msg );
			if ( cl_shownet->integer >= 1 && ctx->clc.clientNum == 2 ) {
				Com_Printf( "Parsed snapshot delta %d, numEntities %d\n", ctx->cl.snap.deltaNum == -1 ? 0 : ctx->cl.snap.messageNum - ctx->cl.snap.deltaNum, ctx->cl.snap.numEntities );
				for ( int i = 0; i < ctx->cl.snap.numEntities; i++ ) {
					Com_Printf( "%d ", ctx->cl.parseEntities[( ctx->cl.snap.parseEntitiesNum + i ) % MAX_PARSE_ENTITIES].number );
				}
				Com_Printf( "\n" );
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
		}
	}
}

