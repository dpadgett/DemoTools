#pragma once
#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

extern uint32_t g_nextEntityVersion;

void MSG_WriteDeltaEntityWithFloatsForcedOrFloatForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to, struct entityState_s* floatForced,
	qboolean force, qboolean isFloatForced );
void MSG_WriteDeltaEntityWithFloatsForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to, struct entityState_s* floatForced,
	qboolean force );
void MSG_WriteDeltaEntityOrFloatForced( msg_t* msg, struct entityState_s* from, struct entityState_s* to,
	qboolean force, qboolean isFloatForced );

void MSG_ReadDeltaEntityWithFloats( msg_t* msg, entityState_t* from, entityState_t* to, entityState_t* floatForced,
	int number, qboolean isFloatForced );
void MSG_WriteDeltaPlayerstateWithFieldsForcedOrWriteForcedFields( msg_t* msg, struct playerState_s* from, struct playerState_s* to, struct playerState_s* forcedFields, qboolean isVehiclePS, qboolean isForcedFields );
void MSG_WriteDeltaPlayerstateWithFieldsForced( msg_t* msg, struct playerState_s* from, struct playerState_s* to, struct playerState_s* forcedFields, qboolean isVehiclePS );
void MSG_WriteDeltaPlayerstateForcedFields( msg_t* msg, struct playerState_s* from, struct playerState_s* to, qboolean isVehiclePS );

void MSG_ReadDeltaPlayerstateWithForcedFields( msg_t* msg, playerState_t* from, playerState_t* to, playerState_t* forcedFields, qboolean isVehiclePS, qboolean isForcedFields = qfalse );
static inline void MSG_ReadDeltaPlayerstateForcedFields( msg_t* msg, playerState_t* from, playerState_t* to, qboolean isVehiclePS = qfalse ) {
	MSG_ReadDeltaPlayerstateWithForcedFields( msg, from, to, NULL, isVehiclePS, qtrue );
}

static inline void MSG_FastWriteDeltaPlayerstate( msg_t* msg, struct playerState_s* from, struct playerState_s* to, qboolean isVehiclePS = qfalse ) {
	MSG_WriteDeltaPlayerstateWithFieldsForced( msg, from, to, NULL, isVehiclePS );
}
static inline void MSG_FastReadDeltaPlayerstate( msg_t* msg, playerState_t* from, playerState_t* to, qboolean isVehiclePS = qfalse ) {
	MSG_ReadDeltaPlayerstateWithForcedFields( msg, from, to, NULL, isVehiclePS );
}

static inline void MSG_FastInit( msg_t* buf, byte* data, int length ) {
	Com_Memset( buf, 0, sizeof( *buf ) );
	buf->data = data;
	buf->maxsize = length;
}

void MSG_FastWriteBitsInternal( msg_t* msg, int value, int bits );
int MSG_FastReadBitsInternal( msg_t* msg, int bits );

static inline void MSG_FastWriteBits( msg_t* msg, int value, int bits ) {
	if ( __builtin_constant_p( bits ) && bits == 1 && !msg->oob ) {
		extern int oldsize;
		oldsize += 1;
		if ( msg->maxsize - msg->cursize < 4 ) {
			msg->overflowed = qtrue;
			return;
		}
		int bloc = msg->bit;
		int bitPos = bloc & 7;
		if ( bitPos == 0 ) {
			msg->data[bloc >> 3] = (byte)( value & 1 );
		} else {
			msg->data[bloc >> 3] |= (byte)( ( value & 1 ) << bitPos );
		}
		bloc++;
		msg->bit = bloc;
		msg->cursize = ( bloc >> 3 ) + 1;
		return;
	}
	MSG_FastWriteBitsInternal( msg, value, bits );
}

static inline int MSG_FastReadBits( msg_t* msg, int bits ) {
	if ( __builtin_constant_p( bits ) && bits == 1 && !msg->oob ) {
		int bloc = msg->bit;
		int value = ( msg->data[bloc >> 3] >> ( bloc & 7 ) ) & 1;
		bloc++;
		msg->bit = bloc;
		msg->readcount = ( bloc >> 3 ) + 1;
		return value;
	}
	return MSG_FastReadBitsInternal( msg, bits );
}
void MSG_FastWriteChar( msg_t* sb, int c );
void MSG_FastWriteByte( msg_t* sb, int c );
void MSG_FastWriteShort( msg_t* sb, int c );
void MSG_FastWriteLong( msg_t* sb, int c );
void MSG_FastWriteData( msg_t* buf, const void* data, int length );
void MSG_FastWriteString( msg_t* sb, const char* s );
void MSG_FastWriteBigString( msg_t* sb, const char* s );
int MSG_FastReadChar( msg_t* msg );
int MSG_FastReadByte( msg_t* msg );
int MSG_FastReadShort( msg_t* msg );
int MSG_FastReadLong( msg_t* msg );
void MSG_FastReadData( msg_t* msg, void* data, int len );

char *MSG_ReadRawString( msg_t *msg );
char *MSG_ReadRawBigString( msg_t *msg );

#define MSG_Init MSG_FastInit
#define MSG_WriteBits MSG_FastWriteBits
#define MSG_ReadBits MSG_FastReadBits
#define MSG_WriteChar MSG_FastWriteChar
#define MSG_WriteByte MSG_FastWriteByte
#define MSG_WriteShort MSG_FastWriteShort
#define MSG_WriteLong MSG_FastWriteLong
#define MSG_WriteData MSG_FastWriteData
#define MSG_WriteString MSG_FastWriteString
#define MSG_WriteBigString MSG_FastWriteBigString
#define MSG_ReadChar MSG_FastReadChar
#define MSG_ReadByte MSG_FastReadByte
#define MSG_ReadShort MSG_FastReadShort
#define MSG_ReadLong MSG_FastReadLong
#define MSG_ReadData MSG_FastReadData
#define MSG_ReadString MSG_ReadRawString
#define MSG_ReadBigString MSG_ReadRawBigString
#define MSG_WriteDeltaEntity(msg, from, to, force) MSG_WriteDeltaEntityWithFloatsForced(msg, from, to, NULL, force)
#define MSG_ReadDeltaEntity(msg, from, to, num) MSG_ReadDeltaEntityWithFloats(msg, from, to, NULL, num, qfalse)
#define MSG_WriteDeltaPlayerstate MSG_FastWriteDeltaPlayerstate
#define MSG_ReadDeltaPlayerstate MSG_FastReadDeltaPlayerstate
