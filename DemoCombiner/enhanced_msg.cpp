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
uint32_t g_nextEntityVersion = 1;

// Fast static Huffman lookup tables (max codeword length is 11 bits)
#define HUFF_DEC_BITS 11
#define HUFF_DEC_SIZE (1 << HUFF_DEC_BITS)
#define HUFF_DEC_MASK (HUFF_DEC_SIZE - 1)

struct HuffDecEntry {
	uint8_t sym;
	uint8_t len;
};

static const uint16_t huffEncCode[256] = {
	0x0002u, 0x001bu, 0x0048u, 0x006cu, 0x00a1u, 0x0011u, 0x0010u, 0x003fu, 0x0015u, 0x0034u, 0x0069u, 0x000bu, 0x0013u, 0x002du, 0x0039u, 0x00acu,
	0x0025u, 0x0058u, 0x01f0u, 0x01f8u, 0x01ddu, 0x03f3u, 0x022bu, 0x0323u, 0x00f4u, 0x018du, 0x00abu, 0x0363u, 0x01ebu, 0x0043u, 0x004fu, 0x00d4u,
	0x0037u, 0x00d3u, 0x0044u, 0x02cdu, 0x03c5u, 0x03f9u, 0x030du, 0x03cdu, 0x0094u, 0x01acu, 0x0033u, 0x0014u, 0x0271u, 0x02f0u, 0x01f4u, 0x0078u,
	0x0027u, 0x00c3u, 0x00efu, 0x0197u, 0x0053u, 0x00b1u, 0x000du, 0x0161u, 0x0007u, 0x00f1u, 0x0199u, 0x0191u, 0x0123u, 0x00bcu, 0x0144u, 0x01f3u,
	0x00cfu, 0x0050u, 0x007cu, 0x0004u, 0x0021u, 0x0051u, 0x0080u, 0x0070u, 0x013du, 0x0063u, 0x02d7u, 0x0371u, 0x019du, 0x02abu, 0x01c7u, 0x0333u,
	0x012cu, 0x009du, 0x016bu, 0x036bu, 0x01d3u, 0x0171u, 0x01e3u, 0x0233u, 0x00d7u, 0x02cbu, 0x0170u, 0x00a8u, 0x00c7u, 0x0105u, 0x00ebu, 0x00d8u,
	0x00f3u, 0x003cu, 0x01abu, 0x018fu, 0x0097u, 0x0030u, 0x0041u, 0x014fu, 0x001cu, 0x0028u, 0x00bdu, 0x00c4u, 0x0098u, 0x008fu, 0x000cu, 0x00b3u,
	0x0085u, 0x008cu, 0x0047u, 0x0079u, 0x0059u, 0x0040u, 0x0017u, 0x0019u, 0x004bu, 0x00e1u, 0x00a3u, 0x0073u, 0x006fu, 0x0068u, 0x0008u, 0x0065u,
	0x001fu, 0x0029u, 0x004cu, 0x007du, 0x000fu, 0x0083u, 0x0001u, 0x0087u, 0x0067u, 0x00e7u, 0x0057u, 0x0074u, 0x01cbu, 0x01c4u, 0x0081u, 0x004du,
	0x0131u, 0x0163u, 0x0180u, 0x03d7u, 0x002bu, 0x0145u, 0x006bu, 0x003du, 0x032bu, 0x00f9u, 0x00e3u, 0x0245u, 0x012bu, 0x0031u, 0x03ebu, 0x01b9u,
	0x0114u, 0x01f9u, 0x0133u, 0x002cu, 0x02ddu, 0x01c1u, 0x031du, 0x01d1u, 0x0138u, 0x0061u, 0x02e3u, 0x0345u, 0x026bu, 0x00cdu, 0x00cbu, 0x014du,
	0x0038u, 0x03c1u, 0x023du, 0x03bcu, 0x00c5u, 0x03acu, 0x03e3u, 0x0299u, 0x03d3u, 0x0214u, 0x0203u, 0x01bcu, 0x029du, 0x0381u, 0x0263u, 0x008du,
	0x0054u, 0x0103u, 0x005du, 0x0020u, 0x0009u, 0x03c7u, 0x0307u, 0x00b8u, 0x01f1u, 0x022cu, 0x0045u, 0x0003u, 0x011du, 0x01c5u, 0x034du, 0x001du,
	0x0000u, 0x03b9u, 0x00ddu, 0x0181u, 0x010du, 0x00b9u, 0x01cdu, 0x0394u, 0x01bdu, 0x0194u, 0x038du, 0x0158u, 0x03bdu, 0x00c1u, 0x03ddu, 0x00f8u,
	0x00d1u, 0x0091u, 0x0099u, 0x02f8u, 0x0023u, 0x0071u, 0x02d3u, 0x0391u, 0x0049u, 0x0231u, 0x0107u, 0x0261u, 0x0223u, 0x0018u, 0x0205u, 0x02c1u,
	0x01d7u, 0x00f0u, 0x02c5u, 0x0300u, 0x03d1u, 0x03a8u, 0x021du, 0x0500u, 0x0005u, 0x0358u, 0x02f9u, 0x01a8u, 0x02b9u, 0x028du, 0x002fu, 0x0024u,
};
static const uint8_t huffEncLen[256] = {
	 2,  5,  7,  7,  8,  8,  7,  6,  5,  7,  7,  7,  7,  6,  8,  9,
	 7,  9,  9,  9, 10, 10, 10, 10,  9, 10, 10, 10, 10,  8,  9,  8,
	 6, 10,  9, 10, 10, 10, 10, 10,  9, 10, 10, 10, 10, 10,  9,  8,
	 7,  8,  8,  9,  8,  8,  9,  9,  9,  9,  9, 10, 10,  9,  9, 10,
	 8,  7,  7,  7,  8,  8,  9,  9,  9, 10, 10, 10,  9, 10, 10, 10,
	 9, 10, 10, 10, 10, 10, 10, 10, 10, 10,  9,  9,  9,  9,  9,  8,
	 9,  8,  9,  9,  9,  7,  8,  9,  6,  8,  9,  9,  8,  9,  8,  8,
	 8,  8,  8,  8,  7,  7,  8,  8,  8,  8,  8,  8,  8,  7,  7,  7,
	 6,  7,  7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,
	 9, 10,  9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	 9, 10, 10, 10, 10, 10, 10, 10,  9, 10, 10, 10, 10, 10, 10, 10,
	 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	 8,  9,  8,  6,  7, 10, 10,  8,  9, 10, 10, 10, 10, 10, 10, 10,
	 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	 9,  9, 10, 10, 10, 10, 10, 10,  7, 10, 10, 10, 10,  8, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 11, 10, 10, 10, 10, 10, 10,  7,  6,
};
static HuffDecEntry huffDecTable[HUFF_DEC_SIZE];
static bool fastHuffInitialized = false;

static void InitFastHuffman() {
	if ( fastHuffInitialized ) {
		return;
	}
	for ( int ch = 0; ch < 256; ch++ ) {
		uint32_t code = huffEncCode[ch];
		uint8_t len = huffEncLen[ch];
		uint32_t numHi = 1u << ( HUFF_DEC_BITS - len );
		for ( uint32_t hi = 0; hi < numHi; hi++ ) {
			huffDecTable[code | ( hi << len )] = { (uint8_t)ch, len };
		}
	}
	fastHuffInitialized = true;
}

static inline void AppendRawBits( byte* fout, int& bloc, uint32_t code, int nbits ) {
	int bitPos = bloc & 7;
	byte* p = fout + ( bloc >> 3 );
	uint32_t acc = ( (uint32_t)p[0] & ( ( 1u << bitPos ) - 1u ) ) | ( code << bitPos );
	p[0] = (byte)acc;
	int totalBits = bitPos + nbits;
	if ( totalBits > 8 ) {
		p[1] = (byte)( acc >> 8 );
		if ( totalBits > 16 ) {
			p[2] = (byte)( acc >> 16 );
		}
	}
	bloc += nbits;
}

#include "enhanced_msg.h"

void MSG_FastWriteBitsInternal( msg_t* msg, int value, int bits ) {
	oldsize += bits;

	if ( msg->maxsize - msg->cursize < 4 ) {
		msg->overflowed = qtrue;
		return;
	}

	if ( bits == 1 && !msg->oob ) {
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

	if ( bits == 0 || bits < -31 || bits > 32 ) {
		Com_Error( ERR_DROP, "MSG_WriteBits: bad bits %i", bits );
	}

	if ( bits != 32 ) {
		if ( bits > 0 ) {
			if ( value > ( ( 1 << bits ) - 1 ) || value < 0 ) {
				overflows++;
			}
		} else {
			int r = 1 << ( -bits - 1 );
			if ( value > r - 1 || value < -r ) {
				overflows++;
			}
		}
	}
	if ( bits < 0 ) {
		bits = -bits;
	}
	if ( msg->oob ) {
		if ( bits == 8 ) {
			msg->data[msg->cursize] = value;
			msg->cursize += 1;
			msg->bit += 8;
		} else if ( bits == 16 ) {
			short temp = value;
			CopyLittleShort( &msg->data[msg->cursize], &temp );
			msg->cursize += 2;
			msg->bit += 16;
		} else if ( bits == 32 ) {
			CopyLittleLong( &msg->data[msg->cursize], &value );
			msg->cursize += 4;
			msg->bit += 32;
		} else {
			Com_Error( ERR_DROP, "can't write %d bits\n", bits );
		}
	} else {
		if ( !fastHuffInitialized ) {
			InitFastHuffman();
		}
		uint32_t uval = (uint32_t)value & ( 0xffffffffu >> ( 32 - bits ) );
		int bloc = msg->bit;
		byte* fout = msg->data;
		if ( bits & 7 ) {
			int nbits = bits & 7;
			AppendRawBits( fout, bloc, uval & ( ( 1u << nbits ) - 1u ), nbits );
			uval >>= nbits;
			bits -= nbits;
		}
		if ( bits ) {
			for ( int i = 0; i < bits; i += 8 ) {
				uint8_t ch = (uint8_t)( uval & 0xffu );
				AppendRawBits( fout, bloc, huffEncCode[ch], huffEncLen[ch] );
				uval >>= 8;
			}
		}
		msg->bit = bloc;
		msg->cursize = ( bloc >> 3 ) + 1;
	}
}

int MSG_FastReadBitsInternal( msg_t* msg, int bits ) {
	if ( bits == 1 && !msg->oob ) {
		int bloc = msg->bit;
		int value = ( msg->data[bloc >> 3] >> ( bloc & 7 ) ) & 1;
		bloc++;
		msg->bit = bloc;
		msg->readcount = ( bloc >> 3 ) + 1;
		return value;
	}

	int value = 0;
	qboolean sgn;
	if ( bits < 0 ) {
		bits = -bits;
		sgn = qtrue;
	} else {
		sgn = qfalse;
	}

	if ( msg->oob ) {
		if ( bits == 8 ) {
			value = msg->data[msg->readcount];
			msg->readcount += 1;
			msg->bit += 8;
		} else if ( bits == 16 ) {
			short temp;
			CopyLittleShort( &temp, &msg->data[msg->readcount] );
			value = temp;
			msg->readcount += 2;
			msg->bit += 16;
		} else if ( bits == 32 ) {
			CopyLittleLong( &value, &msg->data[msg->readcount] );
			msg->readcount += 4;
			msg->bit += 32;
		} else {
			Com_Error( ERR_DROP, "can't read %d bits\n", bits );
		}
	} else {
		if ( !fastHuffInitialized ) {
			InitFastHuffman();
		}
		int bloc = msg->bit;
		const byte* fin = msg->data;
		int nbits = 0;
		if ( bits & 7 ) {
			nbits = bits & 7;
			int byteIdx = bloc >> 3;
			int bitPos = bloc & 7;
			uint32_t raw = fin[byteIdx];
			if ( bitPos + nbits > 8 ) {
				raw |= (uint32_t)fin[byteIdx + 1] << 8;
			}
			value = (int)( ( raw >> bitPos ) & ( ( 1u << nbits ) - 1u ) );
			bloc += nbits;
			bits -= nbits;
		}
		if ( bits ) {
			int maxsize = msg->maxsize;
			for ( int i = 0; i < bits; i += 8 ) {
				int byteIdx = bloc >> 3;
				int bitPos = bloc & 7;
				uint32_t raw;
				if ( byteIdx + 2 < maxsize ) {
					raw = (uint32_t)fin[byteIdx] | ( (uint32_t)fin[byteIdx + 1] << 8 ) | ( (uint32_t)fin[byteIdx + 2] << 16 );
				} else {
					raw = ( byteIdx < maxsize ? (uint32_t)fin[byteIdx] : 0u )
						| ( byteIdx + 1 < maxsize ? ( (uint32_t)fin[byteIdx + 1] << 8 ) : 0u )
						| ( byteIdx + 2 < maxsize ? ( (uint32_t)fin[byteIdx + 2] << 16 ) : 0u );
				}
				HuffDecEntry e = huffDecTable[( raw >> bitPos ) & HUFF_DEC_MASK];
				value |= ( (int)e.sym << ( i + nbits ) );
				bloc += e.len;
			}
		}
		msg->bit = bloc;
		msg->readcount = ( bloc >> 3 ) + 1;
	}
	if ( sgn && bits > 0 && bits < 32 ) {
		if ( value & ( 1 << ( bits - 1 ) ) ) {
			value |= -1 ^ ( ( 1 << bits ) - 1 );
		}
	}

	return value;
}

void MSG_FastWriteChar( msg_t* sb, int c ) {
	MSG_FastWriteBits( sb, c, 8 );
}

void MSG_FastWriteByte( msg_t* sb, int c ) {
	MSG_FastWriteBits( sb, c, 8 );
}

void MSG_FastWriteShort( msg_t* sb, int c ) {
	MSG_FastWriteBits( sb, c, 16 );
}

void MSG_FastWriteLong( msg_t* sb, int c ) {
	MSG_FastWriteBits( sb, c, 32 );
}

void MSG_FastWriteData( msg_t* buf, const void* data, int length ) {
	if ( !buf->oob ) {
		if ( !fastHuffInitialized ) {
			InitFastHuffman();
		}
		const byte* p = (const byte*)data;
		byte* fout = buf->data;
		int bloc = buf->bit;
		for ( int i = 0; i < length; i++ ) {
			if ( buf->maxsize - ( ( bloc >> 3 ) + 1 ) < 4 ) {
				buf->cursize = ( bloc >> 3 ) + 1;
				buf->bit = bloc;
				buf->overflowed = qtrue;
				return;
			}
			oldsize += 8;
			uint8_t ch = p[i];
			AppendRawBits( fout, bloc, huffEncCode[ch], huffEncLen[ch] );
		}
		buf->bit = bloc;
		buf->cursize = ( bloc >> 3 ) + 1;
		return;
	}
	const byte* p = (const byte*)data;
	for ( int i = 0; i < length; i++ ) {
		MSG_FastWriteBitsInternal( buf, p[i], 8 );
	}
}

void MSG_FastWriteString( msg_t* sb, const char* s ) {
	if ( !s ) {
		MSG_FastWriteData( sb, "", 1 );
	} else {
		int l = (int)strlen( s );
		if ( l >= MAX_STRING_CHARS ) {
			Com_Printf( "MSG_WriteString: MAX_STRING_CHARS" );
			MSG_FastWriteData( sb, "", 1 );
			return;
		}
		MSG_FastWriteData( sb, s, l + 1 );
	}
}

void MSG_FastWriteBigString( msg_t* sb, const char* s ) {
	if ( !s ) {
		MSG_FastWriteData( sb, "", 1 );
	} else {
		int l = (int)strlen( s );
		if ( l >= BIG_INFO_STRING ) {
			Com_Printf( "MSG_WriteString: BIG_INFO_STRING" );
			MSG_FastWriteData( sb, "", 1 );
			return;
		}
		MSG_FastWriteData( sb, s, l + 1 );
	}
}

int MSG_FastReadChar( msg_t* msg ) {
	int c = (signed char)MSG_FastReadBits( msg, 8 );
	if ( msg->readcount > msg->cursize ) {
		c = -1;
	}
	return c;
}

int MSG_FastReadByte( msg_t* msg ) {
	int c = (unsigned char)MSG_FastReadBits( msg, 8 );
	if ( msg->readcount > msg->cursize ) {
		c = -1;
	}
	return c;
}

int MSG_FastReadShort( msg_t* msg ) {
	int c = (short)MSG_FastReadBits( msg, 16 );
	if ( msg->readcount > msg->cursize ) {
		c = -1;
	}
	return c;
}

int MSG_FastReadLong( msg_t* msg ) {
	int c = MSG_FastReadBits( msg, 32 );
	if ( msg->readcount > msg->cursize ) {
		c = -1;
	}
	return c;
}

void MSG_FastReadData( msg_t* msg, void* data, int len ) {
	byte* p = (byte*)data;
	for ( int i = 0; i < len; i++ ) {
		p[i] = (byte)MSG_FastReadByte( msg );
	}
}

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

	if ( !isFloatForced && ( to->number < 0 || to->number >= MAX_GENTITIES ) ) {
		Com_Error( ERR_FATAL, "MSG_WriteDeltaEntity: Bad entity number: %i", to->number );
	}

	const bool hasFloatForced = ( floatForced != NULL && floatForced->number != 0 );
	lc = 0;
	if ( isFloatForced ) {
		if ( from->number != 0 || to->number != 0 ) {
			for ( i = numFields - 1; i >= 0; i-- ) {
				size_t offset = entityStateFields[i].offset;
				if ( *(const int*)( (const byte*)from + offset ) != *(const int*)( (const byte*)to + offset ) ) {
					lc = i + 1;
					break;
				}
			}
		}
	} else if ( !hasFloatForced ) {
		if ( memcmp( (const byte*)from + 4, (const byte*)to + 4, sizeof( *from ) - 4 ) != 0 ) {
			for ( i = numFields - 1; i >= 0; i-- ) {
				size_t offset = entityStateFields[i].offset;
				if ( *(const int*)( (const byte*)from + offset ) != *(const int*)( (const byte*)to + offset ) ) {
					lc = i + 1;
					break;
				}
			}
		}
	} else {
		for ( i = numFields - 1; i >= 0; i-- ) {
			size_t offset = entityStateFields[i].offset;
			if ( *(const int*)( (const byte*)from + offset ) != *(const int*)( (const byte*)to + offset ) ||
				 *(const int*)( (const byte*)floatForced + offset ) == 1 ) {
				lc = i + 1;
				break;
			}
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

		qboolean forceFloat = qfalse;
		if ( hasFloatForced ) {
			floatForcedF = (int*) ( (byte*) floatForced + field->offset );
			forceFloat = *floatForcedF == 1 ? qtrue : qfalse;
		}

		if ( *fromF == *toF && !forceFloat ) {
			MSG_WriteBits( msg, 0, 1 );	// no change
			continue;
		}

		int fieldBits = isFloatForced ? 32 : field->bits;

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
				MSG_WriteBits( msg, *toF, fieldBits );
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
	int			trunc;

	if ( number < 0 || number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "Bad delta entity number: %i", number );
	}

	// check for a remove
	if ( MSG_ReadBits( msg, 1 ) == 1 ) {
		Com_Memset( to, 0, sizeof( *to ) );
		to->number = MAX_GENTITIES - 1;
		return;
	}

	// check for no delta
	if ( MSG_ReadBits( msg, 1 ) == 0 ) {
		if ( isFloatForced ) {
			if ( from->number || to->number ) {
				*to = *from;
			}
		} else {
			*to = *from;
			to->number = number;
			if ( floatForced && floatForced->number ) {
				Com_Memset( floatForced, 0, sizeof( *floatForced ) );
			}
		}
		return;
	}

	numFields = (int) ARRAY_LEN( entityStateFields );
	lc = MSG_ReadByte( msg );

	if ( lc > numFields || lc < 0 )
		Com_Error( ERR_DROP, "invalid entityState field count (got: %i, expecting: %i)", lc, numFields );

	entityState_t oldCopy;
	if ( from == to ) {
		oldCopy = *from;
		from = &oldCopy;
	} else {
		*to = *from;
	}
	if ( !isFloatForced ) {
		to->number = number;
	}
	if ( floatForced && floatForced->number ) {
		Com_Memset( floatForced, 0, sizeof( *floatForced ) );
	}
	int hasForced = 0;

	for ( i = 0, field = entityStateFields; i < lc; i++, field++ ) {
		if ( !MSG_ReadBits( msg, 1 ) ) {
			// no change (already copied by *to = *from)
			continue;
		}
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		int fieldBits = isFloatForced ? 32 : field->bits;
		if ( fieldBits == 0 ) {
			// float
			if ( MSG_ReadBits( msg, 1 ) == 0 ) {
				*(float*) toF = 0.0f;
				if ( *toF == *fromF && floatForced ) {
					*(int*) ( (byte*) floatForced + field->offset ) = 1;
					hasForced = 1;
				}
			}
			else {
				if ( MSG_ReadBits( msg, 1 ) == 0 ) {
					// integral float
					trunc = MSG_ReadBits( msg, FLOAT_INT_BITS );
					trunc -= FLOAT_INT_BIAS;
					*(float*) toF = trunc;
				}
				else {
					// full floating point value
					*toF = MSG_ReadBits( msg, 32 );
				}
			}
		}
		else {
			if ( MSG_ReadBits( msg, 1 ) == 0 ) {
				*toF = 0;
				if ( *toF == *fromF && floatForced ) {
					*(int*) ( (byte*) floatForced + field->offset ) = 1;
					hasForced = 1;
				}
			}
			else {
				// integer
				*toF = MSG_ReadBits( msg, fieldBits );
				if ( *toF == *fromF && floatForced ) {
					*(int*) ( (byte*) floatForced + field->offset ) = 1;
					hasForced = 1;
				}
				if ( *toF == 0 ) {
					*toF = ( 1 << fieldBits );
				}
			}
		}
	}

	if ( floatForced ) {
		floatForced->number = hasForced;
	} else if ( isFloatForced ) {
		int mask = 0;
		const int* toInts = (const int*)to;
		for ( i = 1; i <= numFields; i++ ) {
			mask |= toInts[i];
		}
		to->number = mask;
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

	static const playerState_t zeroDummy = {};
	if ( !from ) {
		from = (playerState_t*)&zeroDummy;
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

	const bool hasForced = ( forcedFields != NULL && forcedFields->ping != 0 );
	if ( !hasForced && ( ( isForcedFields && !from->ping && !to->ping ) || memcmp( from, to, sizeof( *from ) ) == 0 ) ) {
		MSG_WriteByte( msg, 0 );
		MSG_WriteBits( msg, 0, 1 );
		return;
	}

	lc = 0;
	if ( !hasForced ) {
		for ( i = numFields - 1; i >= 0; i-- ) {
			size_t offset = PSFields[i].offset;
			if ( *(const int*)( (const byte*)from + offset ) != *(const int*)( (const byte*)to + offset ) ) {
				lc = i + 1;
				break;
			}
		}
	} else {
		for ( i = numFields - 1; i >= 0; i-- ) {
			size_t offset = PSFields[i].offset;
			if ( *(const int*)( (const byte*)from + offset ) != *(const int*)( (const byte*)to + offset ) ||
				 *(const int*)( (const byte*)forcedFields + offset ) != 0 ) {
				lc = i + 1;
				break;
			}
		}
	}

	MSG_WriteByte( msg, lc );	// # of changes

#ifndef FINAL_BUILD
	gLastBitIndex = lc;
#endif

	oldsize += numFields - lc;

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
		if ( hasForced ) {
			forcedFieldF = (int*) ( (byte*) forcedFields + field->offset );
			isForced = *forcedFieldF == 0 ? qfalse : qtrue;
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
		if ( to->stats[i] != from->stats[i] || ( hasForced && forcedFields->stats[i] ) ) {
			statsbits |= 1 << i;
		}
	}
	persistantbits = 0;
	for ( i = 0; i < MAX_PERSISTANT; i++ ) {
		if ( to->persistant[i] != from->persistant[i] || ( hasForced && forcedFields->persistant[i] ) ) {
			persistantbits |= 1 << i;
		}
	}
	ammobits = 0;
	for ( i = 0; i < MAX_AMMO_TRANSMIT; i++ ) {
		if ( to->ammo[i] != from->ammo[i] || ( hasForced && forcedFields->ammo[i] ) ) {
			ammobits |= 1 << i;
		}
	}
	powerupbits = 0;
	for ( i = 0; i < MAX_POWERUPS; i++ ) {
		if ( to->powerups[i] != from->powerups[i] || ( hasForced && forcedFields->powerups[i] ) ) {
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
void MSG_ReadDeltaPlayerstateWithForcedFields( msg_t * msg, playerState_t * from, playerState_t * to, playerState_t * forcedFields, qboolean isVehiclePS, qboolean isForcedFields ) {
	int			i, lc;
	int			bits;
	netField_t* field;
	netField_t* PSFields = playerStateFields;
	int			numFields;
	int* fromF, * toF;
	int			trunc;
#ifdef _ONEBIT_COMBO
	int			numBitsInMask = 0;
#endif
	static const playerState_t zeroDummy = {};

	if ( !from ) {
		from = (playerState_t*)&zeroDummy;
	}
	if ( isForcedFields ) {
		if ( from->ping ) {
			*to = *from;
		} else if ( to->ping ) {
			Com_Memset( to, 0, sizeof( *to ) );
		}
	} else {
		*to = *from;
		if ( forcedFields && forcedFields->ping ) {
			Com_Memset( forcedFields, 0, sizeof( *forcedFields ) );
		}
	}
	int hasForced = 0;

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
#ifdef _ONEBIT_COMBO
		if ( numBitsInMask < 32 &&
			field->bits == 1 )
		{
			numBitsInMask++;
			continue;
		}
#endif

		if ( !MSG_ReadBits( msg, 1 ) ) {
			// no change (already copied by *to = *from)
			continue;
		}
		fromF = (int*) ( (byte*) from + field->offset );
		toF = (int*) ( (byte*) to + field->offset );
		if ( field->bits == 0 ) {
			// float
			if ( MSG_ReadBits( msg, 1 ) == 0 ) {
				// integral float
				trunc = MSG_ReadBits( msg, FLOAT_INT_BITS );
				// bias to allow equal parts positive and negative
				trunc -= FLOAT_INT_BIAS;
				*(float*) toF = trunc;
			}
			else {
				// full floating point value
				*toF = MSG_ReadBits( msg, 32 );
			}
		}
		else {
			// integer
			*toF = MSG_ReadBits( msg, field->bits );
		}
		if ( forcedFields && *fromF == *toF ) {
			*(int*) ( (byte*) forcedFields + field->offset ) = 1;
			hasForced = 1;
		}
	}

	// read the arrays
	int arraysChanged = MSG_ReadBits( msg, 1 );
	if ( arraysChanged ) {
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
					if ( forcedFields && from->stats[i] == to->stats[i] ) {
						forcedFields->stats[i] = 1;
						hasForced = 1;
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
					if ( forcedFields && from->persistant[i] == to->persistant[i] ) {
						forcedFields->persistant[i] = 1;
						hasForced = 1;
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
					if ( forcedFields && from->ammo[i] == to->ammo[i] ) {
						forcedFields->ammo[i] = 1;
						hasForced = 1;
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
					if ( forcedFields && from->powerups[i] == to->powerups[i] ) {
						forcedFields->powerups[i] = 1;
						hasForced = 1;
					}
				}
			}
		}
	}
	if ( forcedFields ) {
		forcedFields->ping = hasForced;
	} else if ( isForcedFields ) {
		if ( lc > 0 || arraysChanged || from->ping ) {
			to->ping = 0;
			int mask = 0;
			const int* p = (const int*)to;
			for ( size_t k = 0; k < sizeof( *to ) / sizeof( int ); k++ ) {
				mask |= p[k];
			}
			to->ping = ( mask != 0 ) ? 1 : 0;
		}
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

char *MSG_ReadRawString( msg_t *msg ) {
	static char	string[MAX_STRING_CHARS];
	unsigned int l = 0;

	if ( !msg->oob ) {
		if ( !fastHuffInitialized ) {
			InitFastHuffman();
		}
		int bloc = msg->bit;
		const byte* fin = msg->data;
		int maxsize = msg->maxsize;
		int cursize = msg->cursize;
		do {
			int byteIdx = bloc >> 3;
			int bitPos = bloc & 7;
			uint32_t raw = ( byteIdx + 2 < maxsize )
				? ( (uint32_t)fin[byteIdx] | ( (uint32_t)fin[byteIdx + 1] << 8 ) | ( (uint32_t)fin[byteIdx + 2] << 16 ) )
				: ( ( byteIdx < maxsize ? (uint32_t)fin[byteIdx] : 0u )
					| ( byteIdx + 1 < maxsize ? ( (uint32_t)fin[byteIdx + 1] << 8 ) : 0u )
					| ( byteIdx + 2 < maxsize ? ( (uint32_t)fin[byteIdx + 2] << 16 ) : 0u ) );
			HuffDecEntry e = huffDecTable[( raw >> bitPos ) & HUFF_DEC_MASK];
			bloc += e.len;
			if ( ( bloc >> 3 ) + 1 > cursize || e.sym == 0 ) {
				break;
			}
			string[l++] = (char)e.sym;
		} while ( l <= sizeof( string ) - 1 );
		msg->bit = bloc;
		msg->readcount = ( bloc >> 3 ) + 1;
		string[l <= sizeof( string ) - 1 ? l : sizeof( string ) - 1] = 0;
		return string;
	}

	do {
		int c = MSG_ReadByte( msg );
		if ( c == -1 || c == 0 ) {
			break;
		}
		string[l++] = c;
	} while ( l <= sizeof( string ) - 1 );
	string[l <= sizeof( string ) - 1 ? l : sizeof( string ) - 1] = 0;
	return string;
}

char *MSG_ReadRawBigString( msg_t *msg ) {
	static char	string[BIG_INFO_STRING];
	int		c;
	unsigned int l;

	l = 0;
	do {
		c = MSG_ReadByte(msg);
		if ( c == -1 || c == 0 ) {
			break;
		}
		string[l] = c;
		l++;
	} while (l < sizeof(string)-1);

	string[l] = 0;

	return string;
}
