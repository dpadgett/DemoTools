// contains utility methods for dealing with demos

#include "deps.h"
#include "client/client.h"
#include "demo_common.h"

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
	MSG_WriteLong( &buf, ctx->clc.reliableSequence );

	MSG_WriteByte (&buf, svc_gamestate);
	// hack - subtract out - MAX_RELIABLE_COMMANDS + 1 from command sequence so it still executes commands from first snapshot
	MSG_WriteLong( &buf, ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 1 );

	// configstrings
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( !ctx->cl.gameState.stringOffsets[i] ) {
			continue;
		}
		s = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[i];
		MSG_WriteByte (&buf, svc_configstring);
		MSG_WriteShort (&buf, i);
		MSG_WriteBigString (&buf, s);
	}

	// baselines
	Com_Memset (&nullstate, 0, sizeof(nullstate));
	for ( i = 0; i < MAX_GENTITIES ; i++ ) {
		ent = &ctx->cl.entityBaselines[i];
		if ( !ent->number ) {
			continue;
		}
		MSG_WriteByte (&buf, svc_baseline);
		MSG_WriteDeltaEntity (&buf, &nullstate, ent, qtrue );
	}

	MSG_WriteByte( &buf, svc_EOF );

	// finished writing the gamestate stuff

	// write the client num
	MSG_WriteLong(&buf, ctx->clc.clientNum);
	// write the checksum feed
	MSG_WriteLong(&buf, ctx->clc.checksumFeed);

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
	len = LittleLong( ctx->clc.serverMessageSequence - 1 );
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
			newent = &ctx->cl.parseEntities[(to->parseEntitiesNum+newindex) & (MAX_PARSE_ENTITIES-1)];
			newnum = newent->number;
		}

		if ( oldindex >= from_num_entities ) {
			oldnum = 9999;
		} else {
			oldent = &ctx->cl.parseEntities[(from->parseEntitiesNum+oldindex) & (MAX_PARSE_ENTITIES-1)];
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
			MSG_WriteDeltaEntity (msg, &ctx->cl.entityBaselines[newnum], newent, qtrue );
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

	static long wroteBytes = 0;

	// write the packet sequence
	len = ctx->clc.serverMessageSequence;
	swlen = LittleLong( len );
	fwrite (&swlen, 4, 1, fp);

	wroteBytes += 4;

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong(len);
	fwrite (&swlen, 4, 1, fp);

	wroteBytes += 4;

	fwrite ( msg->data + headerBytes, 1, len, fp );

	wroteBytes += len;
}

void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta, int serverCommandOffset ) {
	msg_t				msgImpl, *msg = &msgImpl;
	byte				msgData[MAX_MSGLEN];
	clSnapshot_t		*frame, *oldframe;
	int					lastframe = 0;
	int					snapFlags;

	MSG_Init( msg, msgData, sizeof( msgData ) );
	MSG_Bitstream( msg );

	MSG_WriteLong( msg, ctx->clc.reliableSequence );

	// copy over any commands
	for ( int serverCommand = firstServerCommand; serverCommand <= ctx->clc.serverCommandSequence; serverCommand++ ) {
		char *command = ctx->clc.serverCommands[ serverCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		MSG_WriteByte( msg, svc_serverCommand );
		MSG_WriteLong( msg, serverCommand + serverCommandOffset );
		MSG_WriteString( msg, command );
	}

	// this is the snapshot we are creating
	frame = &ctx->cl.snap;
	if ( ctx->cl.snap.messageNum > 0 && !forceNonDelta ) {
		lastframe = 1;
		oldframe = &ctx->cl.snapshots[(ctx->cl.snap.messageNum - 1) & PACKET_MASK]; // 1 frame previous
		if ( !oldframe->valid ) {
			// not yet set
			lastframe = 0;
			oldframe = NULL;
		}
	} else {
		lastframe = 0;
		oldframe = NULL;
	}

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
	if ( oldframe ) {
#ifdef _ONEBIT_COMBO
		MSG_WriteDeltaPlayerstate( msg, &oldframe->ps, &frame->ps, frame->pDeltaOneBit, frame->pDeltaNumBit );
#else
		MSG_WriteDeltaPlayerstate( msg, &oldframe->ps, &frame->ps );
#endif
		if (frame->ps.m_iVehicleNum)
		{ //then write the vehicle's playerstate too
			if (!oldframe->ps.m_iVehicleNum)
			{ //if last frame didn't have vehicle, then the old vps isn't gonna delta
				//properly (because our vps on the client could be anything)
#ifdef _ONEBIT_COMBO
				MSG_WriteDeltaPlayerstate( msg, NULL, &frame->vps, NULL, NULL, qtrue );
#else
				MSG_WriteDeltaPlayerstate( msg, NULL, &frame->vps, qtrue );
#endif
			}
			else
			{
#ifdef _ONEBIT_COMBO
				MSG_WriteDeltaPlayerstate( msg, &oldframe->vps, &frame->vps, frame->pDeltaOneBitVeh, frame->pDeltaNumBitVeh, qtrue );
#else
				MSG_WriteDeltaPlayerstate( msg, &oldframe->vps, &frame->vps, qtrue );
#endif
			}
		}
	} else {
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
	}

	// delta encode the entities
	SV_EmitPacketEntities (oldframe, frame, msg);

	MSG_WriteByte( msg, svc_EOF );

	CL_WriteDemoMessage( msg, 0, fp );
}

void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta ) {
	writeDeltaSnapshot( firstServerCommand, fp, forceNonDelta, 0 );
}
