// contains utility methods for dealing with combined demos

#include "deps.h"
#include "client/client.h"
#include "demo_common.h"
#include "combined_demo_common.h"

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

void writeMergedDemoHeader( FILE* fp ) {
	byte		bufData[MAX_MSGLEN];
	msg_t	buf;
	int			i;
	int			len;
	entityState_t* ent;
	entityState_t	nullstate;
	char* s;

	// write out the gamestate message
	MSG_Init( &buf, bufData, sizeof( bufData ) );
	MSG_Bitstream( &buf );

	// NOTE, MRE: all server->client messages now acknowledge
	int reliableAcknowledgeMask = cctx->matchedClients;
	for ( int i = 0; ( 1 << i ) <= reliableAcknowledgeMask; i++ ) {
		if ( reliableAcknowledgeMask & ( 1 << i ) ) {
			int psIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
			int prev = cctx->reliableAcknowledge[psIdx ^ 1][i];
			int cur = cctx->reliableAcknowledge[psIdx][i];
			if ( cur == prev ) {
				// no change, skip
				reliableAcknowledgeMask ^= ( 1 << i );
			}
		}
	}
	MSG_WriteLong( &buf, reliableAcknowledgeMask );
	for ( int i = 0; ( 1 << i ) <= reliableAcknowledgeMask; i++ ) {
		if ( reliableAcknowledgeMask & ( 1 << i ) ) {
			int psIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
			int *prev = &cctx->reliableAcknowledge[psIdx ^ 1][i];
			int cur = cctx->reliableAcknowledge[psIdx][i];
			MSG_WriteLong( &buf, cur - *prev );
			*prev = cur;  // hack since we write a snap message after this
		}
	}
	//MSG_WriteLong( &buf, ctx->clc.reliableSequence );

	MSG_WriteByte( &buf, svc_gamestate );
	// hack - subtract out - MAX_RELIABLE_COMMANDS + 1 from command sequence so it still executes commands from first snapshot
	//MSG_WriteLong( &buf, ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 1 );
	// new hack - write the serverReliableAcknowledge in the gamestate for all clients, even if they don't appear in the first snapshot
	// works since we parse the first gamestate and snapshot of each client at the beginning?
	// actually not quite - we only copied it in if it's a matched client.  need to fix that.
	int mask = cctx->initialServerReliableAcknowledgeMask;
	MSG_WriteLong( &buf, mask );
	for ( int i = 0; ( 1 << i ) <= mask; i++ ) {
		if ( mask & ( 1 << i ) ) {
			MSG_WriteLong( &buf, cctx->initialServerReliableAcknowledge[i] );
			MSG_WriteLong( &buf, cctx->initialServerMessageSequence[i] );
			MSG_WriteLong( &buf, cctx->initialServerCommandSequence[i] );
		}
	}

	// configstrings
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( !ctx->cl.gameState.stringOffsets[i] ) {
			continue;
		}
		s = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[i];
		MSG_WriteByte( &buf, svc_configstring );
		MSG_WriteShort( &buf, i );
		MSG_WriteBigString( &buf, s );
	}

	// baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		ent = &ctx->cl.entityBaselines[i];
		if ( !ent->number ) {
			continue;
		}
		MSG_WriteByte( &buf, svc_baseline );
		MSG_WriteDeltaEntity( &buf, &nullstate, ent, qtrue );
	}

	MSG_WriteByte( &buf, svc_EOF );

	// finished writing the gamestate stuff

	// write the client num
	MSG_WriteLong( &buf, ctx->clc.clientNum );
	// write the checksum feed
	MSG_WriteLong( &buf, ctx->clc.checksumFeed );

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
		MSG_WriteShort( &buf, 0 );
	}

	// finished writing the client packet
	MSG_WriteByte( &buf, svc_EOF );

	// write it to the demo file
	len = LittleLong( ctx->clc.serverMessageSequence - 1 );
	fwrite( &len, 1, 4, fp );

	len = LittleLong( buf.cursize );
	fwrite( &len, 4, 1, fp );
	fwrite( buf.data, 1, buf.cursize, fp );

	// the rest of the demo file will be copied from net messages
}

/*
=============
SV_EmitPacketEntities

Writes a delta update of an entityState_t list to the message.
=============
*/
static void SV_EmitPacketEntities( clSnapshot_t* from, clSnapshot_t* to, int* owners, int* prev_owners, msg_t* msg ) {
	entityState_t* oldent, * newent;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		from_num_entities;

	// generate the delta update
	if ( !from ) {
		from_num_entities = 0;
	}
	else {
		from_num_entities = from->numEntities;
	}

	newent = NULL;
	oldent = NULL;
	newindex = 0;
	oldindex = 0;
	if ( cl_shownet->integer >= 1 ) {
		Com_Printf( "owner_delta: " );
	}
	while ( newindex < to->numEntities || oldindex < from_num_entities ) {
		if ( newindex >= to->numEntities ) {
			newnum = 9999;
		}
		else {
			newent = &ctx->cl.parseEntities[( to->parseEntitiesNum + newindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			newnum = newent->number;
		}

		if ( oldindex >= from_num_entities ) {
			oldnum = 9999;
		}
		else {
			oldent = &ctx->cl.parseEntities[( from->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldent->number;
		}

		int entnum = Q_min( oldnum, newnum );
		int owner_delta = owner_delta = owners[entnum] ^ prev_owners[entnum];

		if ( newnum == oldnum ) {
			// need to check if MSG_WriteDeltaEntity will write anything or not.
			msg_t tmp = *msg;
			// delta update from old position
			// because the force parm is qfalse, this will not result
			// in any bytes being emited if the entity has not changed at all
			MSG_WriteDeltaEntity( msg, oldent, newent, owner_delta != 0 ? qtrue : qfalse );
			if ( tmp.cursize == msg->cursize && tmp.bit == msg->bit ) {
				// nothing was written, don't need to write ownership info since it didn't change
			} else {
				// write owner bitmask
				if ( cl_shownet->integer >= 1 ) {
					Com_Printf( "%d %d ", entnum, owner_delta );
				}
				if ( owner_delta == 0 ) {
					MSG_WriteBits( msg, 0, 1 );
				} else {
					MSG_WriteBits( msg, 1, 1 );
					MSG_WriteLong( msg, owner_delta );
				}
			}
			oldindex++;
			newindex++;
			continue;
		}

		if ( newnum < oldnum ) {
			// this is a new entity, send it from the baseline
			MSG_WriteDeltaEntity( msg, &ctx->cl.entityBaselines[newnum], newent, qtrue );
			// write owner bitmask
			if ( cl_shownet->integer >= 1 ) {
				Com_Printf( "%d %d ", entnum, owner_delta );
			}
			if ( owner_delta == 0 ) {
				MSG_WriteBits( msg, 0, 1 );
			} else {
				MSG_WriteBits( msg, 1, 1 );
				MSG_WriteLong( msg, owner_delta );
			}
			newindex++;
			continue;
		}

		if ( newnum > oldnum ) {
			// the old entity isn't present in the new message
			MSG_WriteDeltaEntity( msg, oldent, NULL, qtrue );
			if ( oldent != NULL ) {
				// write owner bitmask
				// null oldent won't write anything at all
				if ( cl_shownet->integer >= 1 ) {
					Com_Printf( "%d %d ", entnum, owner_delta );
				}
				if ( owner_delta == 0 ) {
					MSG_WriteBits( msg, 0, 1 );
				} else {
					MSG_WriteBits( msg, 1, 1 );
					MSG_WriteLong( msg, owner_delta );
				}
			}
			else {
				// since we can't transmit current owners, set it to 0
				if ( owners[entnum] != 0 ) {
					owners[entnum] = 0;
				}
			}
			oldindex++;
			continue;
		}
	}

	MSG_WriteBits( msg, ( MAX_GENTITIES - 1 ), GENTITYNUM_BITS );	// end of packetentities
	if ( cl_shownet->integer >= 1 ) {
		Com_Printf( "\n" );
	}
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length
====================
*/
static void CL_WriteDemoMessage( msg_t* msg, int headerBytes, FILE* fp ) {
	int		len, swlen;

	static long wroteBytes = 0;

	// write the packet sequence
	len = ctx->clc.serverMessageSequence;
	swlen = LittleLong( len );
	fwrite( &swlen, 4, 1, fp );

	wroteBytes += 4;

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong( len );
	fwrite( &swlen, 4, 1, fp );

	wroteBytes += 4;

	fwrite( msg->data + headerBytes, 1, len, fp );

	wroteBytes += len;
}

void writeMergedDeltaSnapshot( int firstServerCommand, FILE* fp, qboolean forceNonDelta, int serverCommandOffset ) {
	msg_t				msgImpl, * msg = &msgImpl;
	byte				msgData[MAX_MSGLEN];
	clSnapshot_t* frame, * oldframe;
	int					lastframe = 0;
	int					snapFlags;

	MSG_Init( msg, msgData, sizeof( msgData ) );
	MSG_Bitstream( msg );

	int mask = cctx->matchedClients;
	for ( int i = 0; ( 1 << i ) <= mask; i++ ) {
		if ( mask & ( 1 << i ) ) {
			int psIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
			int prev = cctx->reliableAcknowledge[psIdx ^ 1][i];
			int cur = cctx->reliableAcknowledge[psIdx][i];
			if ( cur == prev ) {
				// no change, skip
				mask ^= ( 1 << i );
			}
		}
	}
	MSG_WriteLong( msg, mask );
	for ( int i = 0; ( 1 << i ) <= mask; i++ ) {
		if ( mask & ( 1 << i ) ) {
			int psIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
			int prev = cctx->reliableAcknowledge[psIdx ^ 1][i];
			int cur = cctx->reliableAcknowledge[psIdx][i];
			MSG_WriteLong( msg, cur - prev );
		}
	}
	//MSG_WriteLong( msg, ctx->clc.reliableSequence );

	// copy over any commands
	for ( int serverCommand = firstServerCommand; serverCommand <= ctx->clc.serverCommandSequence; serverCommand++ ) {
		char* command = ctx->clc.serverCommands[serverCommand & ( MAX_RELIABLE_COMMANDS - 1 )];
		MSG_WriteByte( msg, svc_serverCommand );
		MSG_WriteLong( msg, serverCommand + serverCommandOffset );
		// write bitmask of who should get this command
		int bitmask = cctx->serverCommandBitmask[serverCommand & ( MAX_RELIABLE_COMMANDS - 1 )];
		MSG_WriteLong( msg, bitmask );
		qboolean buffered = qfalse;
		for ( int commandIdx = Q_max( 0, cctx->serverCommandBufferCount - 1024 ); commandIdx < cctx->serverCommandBufferCount; commandIdx++ ) {
			char* bufferedCommand = cctx->serverCommandBuffer[commandIdx % 1024];
			if ( !Q_strncmp( command, bufferedCommand, MAX_STRING_CHARS ) ) {
				// this command already exists in the buffer
				MSG_WriteBits( msg, 1, 1 );
				MSG_WriteShort( msg, cctx->serverCommandBufferCount - commandIdx );
				buffered = qtrue;
			}
		}
		if ( buffered ) {
			continue;
		}
		// new commmand, add to the buffer
		Q_strncpyz( cctx->serverCommandBuffer[cctx->serverCommandBufferCount % 1024], command, MAX_STRING_CHARS );
		cctx->serverCommandBufferCount++;
		MSG_WriteBits( msg, 0, 1 );
		MSG_WriteString( msg, command );
	}

	// this is the snapshot we are creating
	frame = &ctx->cl.snap;
	if ( ctx->cl.snap.messageNum > 0 && !forceNonDelta ) {
		lastframe = 1;
		oldframe = &ctx->cl.snapshots[( ctx->cl.snap.messageNum - 1 ) & PACKET_MASK]; // 1 frame previous
		if ( !oldframe->valid ) {
			// not yet set
			lastframe = 0;
			oldframe = NULL;
		}
	}
	else {
		lastframe = 0;
		oldframe = NULL;
	}

	MSG_WriteByte( msg, svc_snapshot );

	// NOTE, MRE: now sent at the start of every message from server to client
	// let the client know which reliable clientCommands we have received
	//MSG_WriteLong( msg, client->lastClientCommand );

	// send over the current server time so the client can drift
	// its view of time to try to match
	MSG_WriteLong( msg, frame->serverTime );

	// send bitmask of matches
	MSG_WriteLong( msg, cctx->matchedClients );
	// send original delta frame for each match
	for ( int i = 0; i < MAX_CLIENTS; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			MSG_WriteByte( msg, cctx->deltasnap[i] );
			if ( cctx->deltasnap[i] == 0 ) {
				// non-delta frame, so write the reliable acknowledge for this client
				int raIdx = cctx->reliableAcknowledgeIdxMask & ( 1 << i ) ? 0 : 1;
				MSG_WriteLong( msg, cctx->serverReliableAcknowledge[raIdx][i] );
			}
		}
	}

	// what we are delta'ing from
	MSG_WriteByte( msg, lastframe );

	snapFlags = frame->snapFlags;
	MSG_WriteByte( msg, snapFlags );

	// send over the areabits
	MSG_WriteByte( msg, sizeof( frame->areamask ) );
	MSG_WriteData( msg, frame->areamask, sizeof( frame->areamask ) );

	// delta encode the playerstates
	for ( int i = 0; i < MAX_CLIENTS; i++ ) {
		if ( cctx->matchedClients & ( 1 << i ) ) {
			playerState_t *ps = NULL, *oldps = NULL, *vps = NULL, *oldvps = NULL;
			int psIdx = cctx->curPlayerStateIdxMask & ( 1 << i ) ? 0 : 1;
			ps = &cctx->playerStates[psIdx][i];
			vps = &cctx->vehPlayerStates[psIdx][i];
			if ( cctx->playerStateValidMask & ( 1 << i ) ) {
				oldps = &cctx->playerStates[psIdx ^ 1][i];
				oldvps = &cctx->vehPlayerStates[psIdx ^ 1][i];
			}
			if ( oldps ) {
		#ifdef _ONEBIT_COMBO
				MSG_WriteDeltaPlayerstate( msg, &oldframe->ps, &frame->ps, frame->pDeltaOneBit, frame->pDeltaNumBit );
		#else
				MSG_WriteDeltaPlayerstate( msg, oldps, ps );
		#endif
				if ( ps->m_iVehicleNum )
				{ //then write the vehicle's playerstate too
					if ( !oldps->m_iVehicleNum )
					{ //if last frame didn't have vehicle, then the old vps isn't gonna delta
						//properly (because our vps on the client could be anything)
		#ifdef _ONEBIT_COMBO
						MSG_WriteDeltaPlayerstate( msg, NULL, vps, NULL, NULL, qtrue );
		#else
						MSG_WriteDeltaPlayerstate( msg, NULL, vps, qtrue );
		#endif
					}
					else
					{
		#ifdef _ONEBIT_COMBO
						MSG_WriteDeltaPlayerstate( msg, &oldframe->vps, &frame->vps, frame->pDeltaOneBitVeh, frame->pDeltaNumBitVeh, qtrue );
		#else
						MSG_WriteDeltaPlayerstate( msg, oldvps, vps, qtrue );
		#endif
					}
				}
			}
			else {
		#ifdef _ONEBIT_COMBO
				MSG_WriteDeltaPlayerstate( msg, NULL, &frame->ps, NULL, NULL );
		#else
				MSG_WriteDeltaPlayerstate( msg, NULL, ps );
		#endif
				if ( ps->m_iVehicleNum )
				{ //then write the vehicle's playerstate too
		#ifdef _ONEBIT_COMBO
					MSG_WriteDeltaPlayerstate( msg, NULL, &frame->vps, NULL, NULL, qtrue );
		#else
					MSG_WriteDeltaPlayerstate( msg, NULL, vps, qtrue );
		#endif
				}
			}
		}
	}

	// delta encode the entities
	SV_EmitPacketEntities( oldframe, frame, cctx->ent_owners[cctx->ent_owner_idx ^ 1], cctx->ent_owners[cctx->ent_owner_idx], msg );

	MSG_WriteByte( msg, svc_EOF );

	CL_WriteDemoMessage( msg, 0, fp );
}

void writeMergedDeltaSnapshot( int firstServerCommand, FILE* fp, qboolean forceNonDelta ) {
	writeMergedDeltaSnapshot( firstServerCommand, fp, forceNonDelta, 0 );
}
