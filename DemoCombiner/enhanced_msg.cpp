#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "server/server.h"

// these are defined in msg.cc.
// must keep the array sizes in sync for correct operation.

typedef struct netField_s {
	const char* name;
	size_t	offset;
	int		bits;		// 0 = float
#ifndef FINAL_BUILD
	unsigned	mCount;
#endif
} netField_t;
extern netField_t entityStateFields[132];
extern int oldsize;
extern int overflows;


// if (int)f == f and (int)f + ( 1<<(FLOAT_INT_BITS-1) ) < ( 1 << FLOAT_INT_BITS )
// the float will be sent with FLOAT_INT_BITS, otherwise all 32 bits will be sent
#define	FLOAT_INT_BITS	13
#define	FLOAT_INT_BIAS	(1<<(FLOAT_INT_BITS-1))

extern	cvar_t* cl_shownet;

#define	LOG(x) if( cl_shownet && cl_shownet->integer == 4 ) { Com_Printf("%s ", x ); };

/*
==================
MSG_WriteDeltaEntity

Writes part of a packetentities message, including the entity number.
Can delta from either a baseline or a previous packet_entity
If to is NULL, a remove entity update will be sent
If force is not set, then nothing at all will be generated if the entity is
identical, under the assumption that the in-order delta code will catch it.
==================
*/
bool sendFullNegativeZero = false;
void MSG_WriteDeltaEntityWithFloatsForcedOrFloatForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to, struct entityState_s* floatForced,
	qboolean force, qboolean isFloatForced ) {
	int			i, lc;
	int			numFields;
	netField_t* field;
	int			trunc;
	float		fullFloat;
	int* fromF, * toF;
	int* floatForcedF;

	numFields = (int) ARRAY_LEN( entityStateFields );

	// all fields should be 32 bits to avoid any compiler packing issues
	// the "number" field is not part of the field list
	// if this assert fails, someone added a field to the entityState_t
	// struct without updating the message fields
	assert( numFields + 1 == sizeof( *from ) / 4 );

	// a NULL to is a delta remove message
	if ( to == NULL ) {
		if ( from == NULL ) {
			return;
		}
		if ( !isFloatForced ) {
			MSG_WriteBits( msg, from->number, GENTITYNUM_BITS );
		}
		MSG_WriteBits( msg, 1, 1 );
		return;
	}

	if ( to->number < 0 || to->number >= MAX_GENTITIES ) {
		Com_Error( ERR_FATAL, "MSG_WriteDeltaEntity: Bad entity number: %i", to->number );
	}

	lc = 0;
	// build the change vector as bytes so it is endian independent
	for ( i = 0, field = entityStateFields; i < numFields; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		qboolean forceFloat = qfalse;
		if ( floatForced ) {
			floatForcedF = (int*) ( (byte*) floatForced + field->offset );
			forceFloat = *floatForcedF == 1 /* && *toF == 0 */ ? qtrue : qfalse;
		}
		if ( *fromF != *toF || forceFloat ) {
			lc = i + 1;
#ifndef FINAL_BUILD
			field->mCount++;
#endif
		}
	}

	if ( lc == 0 ) {
		// nothing at all changed
		if ( !force ) {
			return;		// nothing at all
		}
		// write two bits for no change
		if ( !isFloatForced ) {
			MSG_WriteBits( msg, to->number, GENTITYNUM_BITS );
		}
		MSG_WriteBits( msg, 0, 1 );		// not removed
		MSG_WriteBits( msg, 0, 1 );		// no delta
		return;
	}

	if ( !isFloatForced ) {
		MSG_WriteBits( msg, to->number, GENTITYNUM_BITS );
	}
	MSG_WriteBits( msg, 0, 1 );			// not removed
	MSG_WriteBits( msg, 1, 1 );			// we have a delta

	MSG_WriteByte( msg, lc );	// # of changes

	oldsize += numFields;

	for ( i = 0, field = entityStateFields; i < lc; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );

		if ( to->pos.trTime >= 198558 && to->number == 7 && !Q_strncmp( field->name, "angles2[1]", MAX_STRING_CHARS ) ) {
			//Com_Printf( "what's up?\n" );
		}

		if ( /* to->pos.trTime == 198829 */ to->angles2[1] == 1.0f && to->number == 7 && !Q_strncmp( field->name, "apos.trBase[1]", MAX_STRING_CHARS ) ) {
			//Com_Printf( "what's up?\n" );
		}

		qboolean forceFloat = qfalse;
		if ( floatForced ) {
			floatForcedF = (int*) ( (byte*) floatForced + field->offset );
			forceFloat = *floatForcedF == 1 /* && *toF == 0 */ ? qtrue : qfalse;
		}

		if ( forceFloat ) {
			Com_Printf( "Forcing float for %s\n", field->name );
		}

		if ( *fromF == *toF && !forceFloat ) {
			MSG_WriteBits( msg, 0, 1 );	// no change
			continue;
		}

		int fieldBits = isFloatForced ? 32 : field->bits;
		if ( false && field->bits == 0 ) {
			if ( *toF == -2147483648 /* -0.0f */ ) {
				if ( sendFullNegativeZero ) {
					// must send -0.0f as a full float to avoid ambiguity
					//Com_Printf( "Sending -0.0f\n" );
					// send as full floating point value
					MSG_WriteBits( msg, 1, 1 );	// changed
					MSG_WriteBits( msg, 1, 1 ); // nonzero
					MSG_WriteBits( msg, 1, 1 ); // fullvalue
					MSG_WriteBits( msg, *toF, 32 );
					continue;
				}
				if ( *fromF == 0 && !force ) {
					// non-force means this is a delta from a non-baseline.
					// going to pull a hail mary here.
					//MSG_WriteBits( msg, 0, 1 );	// no change
					//continue;
				}
			}
		}

		MSG_WriteBits( msg, 1, 1 );	// changed

		if ( fieldBits == 0 ) {
			// float
			fullFloat = *(float*) toF;
			trunc = (int) fullFloat;

			if ( fullFloat == 0.0f ) {
				MSG_WriteBits( msg, 0, 1 );
				oldsize += FLOAT_INT_BITS;
			}
			else {
				MSG_WriteBits( msg, 1, 1 );
				if ( trunc == fullFloat && trunc + FLOAT_INT_BIAS >= 0 &&
					trunc + FLOAT_INT_BIAS < ( 1 << FLOAT_INT_BITS ) ) {
					// send as small integer
					MSG_WriteBits( msg, 0, 1 );
					MSG_WriteBits( msg, trunc + FLOAT_INT_BIAS, FLOAT_INT_BITS );
				}
				else {
					// send as full floating point value
					MSG_WriteBits( msg, 1, 1 );
					MSG_WriteBits( msg, *toF, 32 );
				}
			}
		}
		else {
			if ( *toF == 0 ) {
				MSG_WriteBits( msg, 0, 1 );
			}
			else {
				MSG_WriteBits( msg, 1, 1 );
				// integer
				int of = overflows;
				MSG_WriteBits( msg, *toF, fieldBits );
				if ( overflows > of ) {
					Com_Printf( "Overflow for ent %d %s:%d\n", to->number, field->name, *toF );
				}
			}
		}
	}
}

void MSG_WriteDeltaEntityWithFloatsForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to, struct entityState_s* floatForced,
	qboolean force ) {
	MSG_WriteDeltaEntityWithFloatsForcedOrFloatForced( msg, from, to, floatForced, force, qfalse );
}

void MSG_WriteDeltaEntityOrFloatForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to,
	qboolean force, qboolean isFloatForced ) {
	MSG_WriteDeltaEntityWithFloatsForcedOrFloatForced( msg, from, to, NULL, force, isFloatForced );
}

/*
==================
MSG_ReadDeltaEntity

The entity number has already been read from the message, which
is how the from state is identified.

If the delta removes the entity, entityState_t->number will be set to MAX_GENTITIES-1

Can go from either a baseline or a previous packet_entity
==================
*/
void MSG_ReadDeltaEntityWithFloats( msg_t* msg, entityState_t* from, entityState_t* to, entityState_t* floatForced,
	int number, qboolean isFloatForced ) {
	int			i, lc;
	int			numFields;
	netField_t* field;
	int* fromF, * toF;
	int* floatForcedF;
	int			print;
	int			trunc;
	int			startBit, endBit;

	if ( number < 0 || number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "Bad delta entity number: %i", number );
	}

	startBit = msg->bit;

	// check for a remove
	if ( MSG_ReadBits( msg, 1 ) == 1 ) {
		Com_Memset( to, 0, sizeof( *to ) );
		to->number = MAX_GENTITIES - 1;
		if ( cl_shownet && ( cl_shownet->integer >= 2 || cl_shownet->integer == -1 ) ) {
			Com_Printf( "%3i: #%-3i remove\n", msg->readcount, number );
		}
		return;
	}

	// check for no delta
	if ( MSG_ReadBits( msg, 1 ) == 0 ) {
		*to = *from;
		to->number = number;
		if ( floatForced ) {
			Com_Memset( floatForced, 0, sizeof( *floatForced ) );
		}
		return;
	}

	numFields = (int) ARRAY_LEN( entityStateFields );
	lc = MSG_ReadByte( msg );

	if ( lc > numFields || lc < 0 )
		Com_Error( ERR_DROP, "invalid entityState field count (got: %i, expecting: %i)", lc, numFields );

	// shownet 2/3 will interleave with other printed info, -1 will
	// just print the delta records`
	if ( cl_shownet && ( cl_shownet->integer >= 2 || cl_shownet->integer == -1 ) ) {
		print = 1;
		if ( sv.state )
		{
			Com_Printf( "%3i: #%-3i (%s) ", msg->readcount, number, SV_GentityNum( number )->classname );
		}
		else
		{
			Com_Printf( "%3i: #%-3i ", msg->readcount, number );
		}
	}
	else {
		print = 0;
	}

	to->number = number;

	int offset = msg->readcount;
	for ( i = 0, field = entityStateFields; i < lc; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		if ( floatForced ) {
			floatForcedF = (int*) ( (byte*) floatForced + field->offset );
			*floatForcedF = 0;
		}

		if ( offset == 98 && number == 3 && *(int*) &to->pos.trDelta[0] == 1126983978 && !Q_strncmp( "eventParm", field->name, MAX_STRING_CHARS ) ) {
			Com_Printf( "fixme\n" );
		}

		if ( number == 7 && from->pos.trTime >= 198558 && !Q_strncmp( "angles2[1]", field->name, MAX_STRING_CHARS ) ) {
			//Com_Printf( "7 @%d\n", to->pos.trTime );
			//printhuff = true;
		}

		int fieldBits = isFloatForced ? 32 : field->bits;
		if ( !MSG_ReadBits( msg, 1 ) ) {
			// no change
			*toF = *fromF;
			if ( print ) {
				Com_Printf( "%s:unchanged @%d (@%d) ", field->name, *toF, msg->bit - startBit );
			}
		}
		else {
			if ( fieldBits == 0 ) {
				// float
				if ( number == 244 && !Q_strncmp( "pos.trDelta[1]", field->name, sizeof( field->name ) ) ) {
					//Com_Printf( "244" );
					//print = true;
				}
				if ( MSG_ReadBits( msg, 1 ) == 0 ) {
					*(float*) toF = 0.0f;
					if ( print ) {
						Com_Printf( "%s:%i(1bit) (from %i) (@%d) ", field->name, *toF, *fromF, msg->bit - startBit );
					}
					if ( *toF == *fromF /* && from->number == number */ ) {
						// zero was sent instead of no-change, could happen if it flipped from 0 to -0??
						if ( floatForced ) {
							*floatForcedF = 1;
						}
						//if ( print ) {
						else {
							Com_Printf( "Flipping zero: %i on ent %p field %s ", *toF, to, field->name );
						}
						//}
						//	*(float*) toF = -0.0f; //*= -0.0f;
						//if ( ( *(float*) fromF ) == 0.0f ) {
						//	*(float*) toF = -( *(float*) fromF );
						//}
						//if ( print ) {
						//	Com_Printf( "%i (agreed: %d)\n", *toF, *(float*) toF == 0.0f );
						//}
					}
				}
				else {
					if ( MSG_ReadBits( msg, 1 ) == 0 ) {
						// integral float
						trunc = MSG_ReadBits( msg, FLOAT_INT_BITS );
						// bias to allow equal parts positive and negative
						trunc -= FLOAT_INT_BIAS;
						*(float*) toF = trunc;
						if ( print ) {
							Com_Printf( "%s:%i(intfloat) (@%d) ", field->name, trunc, msg->bit - startBit );
						}
					}
					else {
						// full floating point value
						*toF = MSG_ReadBits( msg, 32 );
						if ( print ) {
							Com_Printf( "%s:%f(fullfloat) [%d] (@%d) ", field->name, *(float*) toF, *toF, msg->bit - startBit );
						}
						/*if ( *toF == -2147483648 ) {
							Com_Printf( "%s:%f(fullfloat) [%d] (@%d) ", field->name, *(float*) toF, *toF, msg->bit - startBit );
						}*/
					}
				}
			}
			else {
				if ( MSG_ReadBits( msg, 1 ) == 0 ) {
					*toF = 0;
					if ( *toF == *fromF ) {
						Com_Printf( "WTF? %s:%i (from %i) (@%d)\n", field->name, *toF, *fromF, msg->bit - startBit );
						if ( floatForced ) {
							*floatForcedF = 1;
						}
					}
					if ( print ) {
						Com_Printf( "%s:%i (from %i) (@%d) ", field->name, *toF, *fromF, msg->bit - startBit );
					}
				}
				else {
					// integer
					*toF = MSG_ReadBits( msg, fieldBits );
					if ( *toF == *fromF ) {
						//Com_Printf( "WTF2? %s:%i (from %i) (@%d)\n", field->name, *toF, *fromF, msg->bit - startBit );
						if ( floatForced ) {
							*floatForcedF = 1;
						}
					}
					if ( *toF == 0 ) {
						Com_Printf( "WTF3? ent %d %s:%i (from %i) (@%d)\n", number, field->name, *toF, *fromF, msg->bit - startBit );
						*toF = ( 1 << fieldBits );
					}
					if ( print ) {
						Com_Printf( "%s:%i (from %i) (@%d) ", field->name, *toF, *fromF, msg->bit - startBit );
					}
				}
			}
		}
	}
	for ( i = lc, field = &entityStateFields[lc]; i < numFields; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		if ( floatForced ) {
			floatForcedF = (int*) ( (byte*) floatForced + field->offset );
			*floatForcedF = 0;
		}
		// no change
		*toF = *fromF;
	}

	if ( number == 7 && from->pos.trTime >= 198558 /* && !Q_strncmp("angles2[1]", field->name, MAX_STRING_CHARS) */ ) {
		//Com_Printf( "7 @%d\n", to->pos.trTime );
		//printhuff = true;
	}

	if ( print ) {
		endBit = msg->bit;
		Com_Printf( " (%i bits)\n", endBit - startBit );
	}
}

//MAKE SURE THIS MATCHES THE ENUM IN BG_PUBLIC.H!!!
//This is in caps, because it is important.
#define STAT_WEAPONS 4

extern netField_t playerStateFields[137];
extern netField_t vehPlayerStateFields[69];
extern netField_t pilotPlayerStateFields[140];

#ifndef FINAL_BUILD
extern int gLastBitIndex;
#endif

/*
=============
MSG_WriteDeltaPlayerstate

=============
*/
#ifdef _ONEBIT_COMBO
void MSG_WriteDeltaPlayerstate( msg_t* msg, struct playerState_s* from, struct playerState_s* to, int* bitComboDelta, int* bitNumDelta, qboolean isVehiclePS ) {
#else
void MSG_WriteDeltaPlayerstateWithFieldsForcedOrWriteForcedFields( msg_t * msg, struct playerState_s* from, struct playerState_s* to, struct playerState_s* forcedFields, qboolean isVehiclePS, qboolean isForcedFields ) {
#endif
	int				i;
	playerState_t	dummy;
	int				statsbits;
	int				persistantbits;
	int				ammobits;
	int				powerupbits;
	int				numFields;
	netField_t* field;
	netField_t* PSFields = playerStateFields;
	int* fromF, * toF, * forcedFieldF;
	float			fullFloat;
	int				trunc, lc;
#ifdef _ONEBIT_COMBO
	int				bitComboMask = 0;
	int				numBitsInMask = 0;
#endif

	if ( !from ) {
		from = &dummy;
		Com_Memset( &dummy, 0, sizeof( dummy ) );
	}

	//=====_OPTIMIZED_VEHICLE_NETWORKING=======================================================================
#ifdef _OPTIMIZED_VEHICLE_NETWORKING
	if ( isVehiclePS )
	{//a vehicle playerstate
		numFields = (int) ARRAY_LEN( vehPlayerStateFields );
		PSFields = vehPlayerStateFields;
	}
	else
	{//regular client playerstate
		if ( to->m_iVehicleNum
			&& ( to->eFlags & EF_NODRAW ) )
		{//pilot riding *inside* a vehicle!
			MSG_WriteBits( msg, 1, 1 );	// Pilot player state
			numFields = (int) ARRAY_LEN( pilotPlayerStateFields );
			PSFields = pilotPlayerStateFields;
		}
		else
		{//normal client
			MSG_WriteBits( msg, 0, 1 );	// Normal player state
			numFields = (int) ARRAY_LEN( playerStateFields );
		}
	}
	//=====_OPTIMIZED_VEHICLE_NETWORKING=======================================================================
#else// _OPTIMIZED_VEHICLE_NETWORKING
	numFields = (int) ARRAY_LEN( playerStateFields );
#endif// _OPTIMIZED_VEHICLE_NETWORKING

	lc = 0;
	for ( i = 0, field = PSFields; i < numFields; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		qboolean isForced = qfalse;
		if ( forcedFields ) {
			forcedFieldF = (int*) ( (byte*) forcedFields + field->offset );
			isForced = *forcedFieldF == 0 ? qfalse : qtrue;
		}
		if ( *fromF != *toF || isForced ) {
			lc = i + 1;
#ifndef FINAL_BUILD
			field->mCount++;
#endif
		}
	}

	MSG_WriteByte( msg, lc );	// # of changes

#ifndef FINAL_BUILD
	gLastBitIndex = lc;
#endif

	oldsize += numFields - lc;

	if ( from->commandTime == 765062 ) {
		Com_Printf( "WTF4?\n" );
	}

	for ( i = 0, field = PSFields; i < lc; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );

#ifdef _ONEBIT_COMBO
		if ( numBitsInMask < 32 &&
			field->bits == 1 )
		{
			bitComboMask |= ( *toF ) << numBitsInMask;
			numBitsInMask++;
			continue;
		}
#endif

		qboolean isForced = qfalse;
		if ( forcedFields ) {
			forcedFieldF = (int*) ( (byte*) forcedFields + field->offset );
			isForced = *forcedFieldF == 0 ? qfalse : qtrue;
			if ( isForced ) {
				Com_Printf( "Forcing playerState field %s:%d\n", field->name, *toF );
			}
		}

		if ( *fromF == *toF && !isForced ) {
			MSG_WriteBits( msg, 0, 1 );	// no change
			continue;
		}

		MSG_WriteBits( msg, 1, 1 );	// changed

		if ( field->bits == 0 ) {
			// float
			fullFloat = *(float*) toF;
			trunc = (int) fullFloat;

			if ( trunc == fullFloat && trunc + FLOAT_INT_BIAS >= 0 &&
				trunc + FLOAT_INT_BIAS < ( 1 << FLOAT_INT_BITS ) ) {
				// send as small integer
				MSG_WriteBits( msg, 0, 1 );
				MSG_WriteBits( msg, trunc + FLOAT_INT_BIAS, FLOAT_INT_BITS );
			}
			else {
				// send as full floating point value
				MSG_WriteBits( msg, 1, 1 );
				MSG_WriteBits( msg, *toF, 32 );
			}
		}
		else {
			// integer
			MSG_WriteBits( msg, *toF, field->bits );
		}
	}


	//
	// send the arrays
	//
	statsbits = 0;
	for ( i = 0; i < MAX_STATS; i++ ) {
		if ( forcedFields && forcedFields->stats[i] ) {
			Com_Printf( "Forcing playerState stat %d\n", i );
		}
		if ( to->stats[i] != from->stats[i] || ( forcedFields && forcedFields->stats[i] ) ) {
			statsbits |= 1 << i;
		}
	}
	persistantbits = 0;
	for ( i = 0; i < MAX_PERSISTANT; i++ ) {
		if ( forcedFields && forcedFields->persistant[i] ) {
			Com_Printf( "Forcing playerState persistant %d\n", i );
		}
		if ( to->persistant[i] != from->persistant[i] || ( forcedFields && forcedFields->persistant[i] ) ) {
			persistantbits |= 1 << i;
		}
	}
	ammobits = 0;
	for ( i = 0; i < MAX_AMMO_TRANSMIT; i++ ) {
		if ( forcedFields && forcedFields->ammo[i] ) {
			Com_Printf( "Forcing playerState ammo %d\n", i );
		}
		if ( to->ammo[i] != from->ammo[i] || ( forcedFields && forcedFields->ammo[i] ) ) {
			ammobits |= 1 << i;
		}
	}
	powerupbits = 0;
	for ( i = 0; i < MAX_POWERUPS; i++ ) {
		if ( forcedFields && forcedFields->powerups[i] ) {
			Com_Printf( "Forcing playerState powerups %d\n", i );
		}
		if ( to->powerups[i] != from->powerups[i] || ( forcedFields && forcedFields->powerups[i] ) ) {
			powerupbits |= 1 << i;
		}
	}

	if ( !statsbits && !persistantbits && !ammobits && !powerupbits ) {
		MSG_WriteBits( msg, 0, 1 );	// no change
		oldsize += 4;
#ifdef _ONEBIT_COMBO
		goto sendBitMask;
#else
		return;
#endif
	}
	MSG_WriteBits( msg, 1, 1 );	// changed

	if ( statsbits ) {
		MSG_WriteBits( msg, 1, 1 );	// changed
		MSG_WriteBits( msg, statsbits, MAX_STATS );
		for ( i = 0; i < MAX_STATS; i++ )
		{
			if ( statsbits & ( 1 << i ) )
			{
				if ( i == STAT_WEAPONS )
				{ //ugly.. but we're gonna need it anyway -rww
					//(just send this one in MAX_WEAPONS bits, so that we can add up to MAX_WEAPONS weaps without hassle)
					MSG_WriteBits( msg, to->stats[i], MAX_WEAPONS );
				}
				else
				{
					MSG_WriteShort( msg, to->stats[i] );
				}
			}
		}
	}
	else {
		MSG_WriteBits( msg, 0, 1 );	// no change
	}


	if ( persistantbits ) {
		MSG_WriteBits( msg, 1, 1 );	// changed
		MSG_WriteBits( msg, persistantbits, MAX_PERSISTANT );
		for ( i = 0; i < MAX_PERSISTANT; i++ )
			if ( persistantbits & ( 1 << i ) )
				MSG_WriteShort( msg, to->persistant[i] );
	}
	else {
		MSG_WriteBits( msg, 0, 1 );	// no change
	}


	if ( ammobits ) {
		MSG_WriteBits( msg, 1, 1 );	// changed
		MSG_WriteBits( msg, ammobits, MAX_AMMO_TRANSMIT );
		for ( i = 0; i < MAX_AMMO_TRANSMIT; i++ )
			if ( ammobits & ( 1 << i ) )
				MSG_WriteShort( msg, to->ammo[i] );
	}
	else {
		MSG_WriteBits( msg, 0, 1 );	// no change
	}


	if ( powerupbits ) {
		MSG_WriteBits( msg, 1, 1 );	// changed
		MSG_WriteBits( msg, powerupbits, MAX_POWERUPS );
		for ( i = 0; i < MAX_POWERUPS; i++ )
			if ( powerupbits & ( 1 << i ) )
				MSG_WriteLong( msg, to->powerups[i] );
	}
	else {
		MSG_WriteBits( msg, 0, 1 );	// no change
	}

#ifdef _ONEBIT_COMBO
	sendBitMask :
	if ( numBitsInMask )
	{ //don't need to send at all if we didn't pass any 1bit values
		if ( !bitComboDelta ||
			bitComboMask != *bitComboDelta ||
			numBitsInMask != *bitNumDelta )
		{ //send the mask, it changed
			MSG_WriteBits( msg, 1, 1 );
			MSG_WriteBits( msg, bitComboMask, numBitsInMask );
			if ( bitComboDelta )
			{
				*bitComboDelta = bitComboMask;
				*bitNumDelta = numBitsInMask;
			}
		}
		else
		{ //send 1 bit 0 to indicate no change
			MSG_WriteBits( msg, 0, 1 );
		}
	}
#endif
}

void MSG_WriteDeltaPlayerstateWithFieldsForced( msg_t * msg, struct playerState_s* from, struct playerState_s* to, struct playerState_s* forcedFields, qboolean isVehiclePS ) {
	MSG_WriteDeltaPlayerstateWithFieldsForcedOrWriteForcedFields( msg, from, to, forcedFields, isVehiclePS, qfalse );
}

void MSG_WriteDeltaPlayerstateForcedFields( msg_t * msg, struct playerState_s* from, struct playerState_s* to, qboolean isVehiclePS ) {
	MSG_WriteDeltaPlayerstateWithFieldsForcedOrWriteForcedFields( msg, from, to, NULL, isVehiclePS, qtrue );
}

/*
===================
MSG_ReadDeltaPlayerstate
===================
*/
void MSG_ReadDeltaPlayerstateWithForcedFields( msg_t * msg, playerState_t * from, playerState_t * to, playerState_t * forcedFields, qboolean isVehiclePS ) {
	int			i, lc;
	int			bits;
	netField_t* field;
	netField_t* PSFields = playerStateFields;
	int			numFields;
	int			startBit, endBit;
	int			print;
	int* fromF, * toF, * forcedFieldF;
	int			trunc;
#ifdef _ONEBIT_COMBO
	int			numBitsInMask = 0;
#endif
	playerState_t	dummy;

	if ( !from ) {
		from = &dummy;
		Com_Memset( &dummy, 0, sizeof( dummy ) );
	}
	*to = *from;

	if ( from->commandTime == 765134 ) {
		Com_Printf( "WTF4?\n" );
	}

	if ( msg->bit == 0 ) {
		startBit = msg->readcount * 8 - GENTITYNUM_BITS;
	}
	else {
		startBit = ( msg->readcount - 1 ) * 8 + msg->bit - GENTITYNUM_BITS;
	}

	// shownet 2/3 will interleave with other printed info, -2 will
	// just print the delta records
	if ( cl_shownet && ( cl_shownet->integer >= 2 || cl_shownet->integer == -2 ) ) {
		print = 1;
		Com_Printf( "%3i: playerstate ", msg->readcount );
	}
	else {
		print = 0;
	}

	//=====_OPTIMIZED_VEHICLE_NETWORKING=======================================================================
#ifdef _OPTIMIZED_VEHICLE_NETWORKING
	if ( isVehiclePS )
	{//a vehicle playerstate
		numFields = (int) ARRAY_LEN( vehPlayerStateFields );
		PSFields = vehPlayerStateFields;
	}
	else
	{
		int isPilot = MSG_ReadBits( msg, 1 );
		if ( isPilot )
		{//pilot riding *inside* a vehicle!
			numFields = (int) ARRAY_LEN( pilotPlayerStateFields );
			PSFields = pilotPlayerStateFields;
		}
		else
		{//normal client
			numFields = (int) ARRAY_LEN( playerStateFields );
		}
	}
	//=====_OPTIMIZED_VEHICLE_NETWORKING=======================================================================
#else//_OPTIMIZED_VEHICLE_NETWORKING
	numFields = (int) ARRAY_LEN( playerStateFields );
#endif//_OPTIMIZED_VEHICLE_NETWORKING

	lc = MSG_ReadByte( msg );

	if ( lc > numFields || lc < 0 )
		Com_Error( ERR_DROP, "invalid playerState field count (got: %i, expecting: %i)", lc, numFields );

	for ( i = 0, field = PSFields; i < lc; i++, field++ ) {
		if ( i > 0 && *toF == 765062 ) {
			Com_Printf( "WTF5?\n" );
		}
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		if ( forcedFields ) {
			forcedFieldF = (int*) ( (byte*) forcedFields + field->offset );
			*forcedFieldF = 0;
		}

#ifdef _ONEBIT_COMBO
		if ( numBitsInMask < 32 &&
			field->bits == 1 )
		{
			*toF = *fromF;
			numBitsInMask++;
			continue;
		}
#endif

		if ( !MSG_ReadBits( msg, 1 ) ) {
			// no change
			*toF = *fromF;
		}
		else {
			if ( field->bits == 0 ) {
				// float
				if ( MSG_ReadBits( msg, 1 ) == 0 ) {
					// integral float
					trunc = MSG_ReadBits( msg, FLOAT_INT_BITS );
					// bias to allow equal parts positive and negative
					trunc -= FLOAT_INT_BIAS;
					*(float*) toF = trunc;
					if ( print ) {
						Com_Printf( "%s:%i ", field->name, trunc );
					}
				}
				else {
					// full floating point value
					*toF = MSG_ReadBits( msg, 32 );
					if ( print ) {
						Com_Printf( "%s:%f ", field->name, *(float*) toF );
					}
				}
			}
			else {
				// integer
				*toF = MSG_ReadBits( msg, field->bits );
				if ( print ) {
					Com_Printf( "%s:%i ", field->name, *toF );
				}
			}
			if ( forcedFields && *fromF == *toF ) {
				*forcedFieldF = 1;
			}
		}
	}
	for ( i = lc, field = &PSFields[lc]; i < numFields; i++, field++ ) {
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		if ( forcedFields ) {
			forcedFieldF = (int*) ( (byte*) forcedFields + field->offset );
			*forcedFieldF = 0;
		}
		// no change
		*toF = *fromF;
	}

	// read the arrays
	if ( MSG_ReadBits( msg, 1 ) ) {
		// parse stats
		if ( MSG_ReadBits( msg, 1 ) ) {
			LOG( "PS_STATS" );
			bits = MSG_ReadBits( msg, MAX_STATS );
			for ( i = 0; i < MAX_STATS; i++ ) {
				if ( bits & ( 1 << i ) )
				{
					if ( i == STAT_WEAPONS )
					{ //ugly.. but we're gonna need it anyway -rww
						to->stats[i] = MSG_ReadBits( msg, MAX_WEAPONS );
					}
					else
					{
						to->stats[i] = MSG_ReadShort( msg );
					}
					if ( forcedFields ) {
						forcedFields->stats[i] = from->stats[i] == to->stats[i] ? 1 : 0;
					}
				}
			}
		}

		// parse persistant stats
		if ( MSG_ReadBits( msg, 1 ) ) {
			LOG( "PS_PERSISTANT" );
			bits = MSG_ReadBits( msg, MAX_PERSISTANT );
			for ( i = 0; i < MAX_PERSISTANT; i++ ) {
				if ( bits & ( 1 << i ) ) {
					to->persistant[i] = MSG_ReadShort( msg );
					if ( forcedFields ) {
						forcedFields->persistant[i] = from->persistant[i] == to->persistant[i] ? 1 : 0;
					}
				}
			}
		}

		// parse ammo
		if ( MSG_ReadBits( msg, 1 ) ) {
			LOG( "PS_AMMO" );
			bits = MSG_ReadBits( msg, MAX_AMMO_TRANSMIT );
			for ( i = 0; i < MAX_AMMO_TRANSMIT; i++ ) {
				if ( bits & ( 1 << i ) ) {
					to->ammo[i] = MSG_ReadShort( msg );
					if ( forcedFields ) {
						forcedFields->ammo[i] = from->ammo[i] == to->ammo[i] ? 1 : 0;
					}
				}
			}
		}

		// parse powerups
		if ( MSG_ReadBits( msg, 1 ) ) {
			LOG( "PS_POWERUPS" );
			bits = MSG_ReadBits( msg, MAX_POWERUPS );
			for ( i = 0; i < MAX_POWERUPS; i++ ) {
				if ( bits & ( 1 << i ) ) {
					to->powerups[i] = MSG_ReadLong( msg );
					if ( forcedFields ) {
						forcedFields->powerups[i] = from->powerups[i] == to->powerups[i] ? 1 : 0;
					}
				}
			}
		}
	}

	if ( print ) {
		if ( msg->bit == 0 ) {
			endBit = msg->readcount * 8 - GENTITYNUM_BITS;
		}
		else {
			endBit = ( msg->readcount - 1 ) * 8 + msg->bit - GENTITYNUM_BITS;
		}
		Com_Printf( " (%i bits)\n", endBit - startBit );
	}

#ifdef _ONEBIT_COMBO
	if ( numBitsInMask &&
		MSG_ReadBits( msg, 1 ) )
	{ //mask changed...
		int newBitMask = MSG_ReadBits( msg, numBitsInMask );
		int nOneBit = 0;

		//we have to go through all the fields again now to match the values
		for ( i = 0, field = PSFields; i < lc; i++, field++ )
		{
			if ( field->bits == 1 )
			{ //a 1 bit value, get the sent value from the mask
				toF = (int*) ( (byte*) to + field->offset );
				*toF = ( newBitMask >> nOneBit ) & 1;
				nOneBit++;
			}
		}
	}
#endif
}
