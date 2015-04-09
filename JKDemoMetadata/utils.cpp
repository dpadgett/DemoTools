#include <cwchar>   // wchar_t wide characters
#include <Windows.h>
#include "client/client.h"
#include "demo_common.h"

// Convert a wide Unicode string to an UTF8 string
static const char *utf8_encode( const wchar_t *wstr, int wstrSize )
{
	int size_needed = WideCharToMultiByte( CP_UTF8, 0, wstr, wstrSize, NULL, 0, NULL, NULL );
	char *strTo = (char *) malloc( size_needed + 1 );
	WideCharToMultiByte( CP_UTF8, 0, &wstr[0], wstrSize, strTo, size_needed, NULL, NULL );
	strTo[size_needed] = '\0';
	return strTo;
}

const wchar_t *utf8BytesToString( const char *utf8 ) {
	int size_needed = MultiByteToWideChar( CP_UTF8, 0, utf8, -1, 0, 0 );
	wchar_t *strTo = (wchar_t *) malloc( size_needed * sizeof( wchar_t ) );
	MultiByteToWideChar( CP_UTF8, 0, utf8, -1, strTo, size_needed );
	return strTo;
}

const char *cp1252toUTF8( const char *cp1252 )
{
	int size_needed = MultiByteToWideChar( CP_ACP, 0, cp1252, -1, 0, 0 );
	wchar_t *strTo = (wchar_t *) malloc( size_needed * sizeof( wchar_t ) );
	MultiByteToWideChar( CP_ACP, 0, cp1252, -1, strTo, size_needed );
	const char *result = utf8_encode( strTo, size_needed );
	free( strTo );
	return result;
}

const char *getPlayerName( int playerIdx ) {
	if ( playerIdx > MAX_CLIENTS ) {
		return "UNKNOWN";
	}
	const char *result = Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_PLAYERS + playerIdx ], "n" );
	if ( result ) {
		return result;
	}
	return "UNKNOWN";
}

const char *getPlayerNameUTF8( int playerIdx ) {
	return cp1252toUTF8( getPlayerName( playerIdx ) );
}

int playerSkill( int playerIdx ) {
	if ( playerIdx > MAX_CLIENTS ) {
		return -1;
	}
	char *skillStr = Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_PLAYERS + playerIdx ], "skill" );
	if ( skillStr && skillStr[0] ) {
		return atoi( skillStr );
	} else {
		return -1;
	}
}

const char *CG_TeamName(team_t team)
{
	if (team==TEAM_RED)
		return "RED";
	else if (team==TEAM_BLUE)
		return "BLUE";
	else if (team==TEAM_SPECTATOR)
		return "SPECTATOR";
	return "FREE";
}

team_t OtherTeam(team_t team) {
	if (team==TEAM_RED)
		return TEAM_BLUE;
	else if (team==TEAM_BLUE)
		return TEAM_RED;
	return team;
}

const char *getPlayerTeamName( int playerIdx ) {
	if ( playerIdx > MAX_CLIENTS ) {
		return "UNKNOWN";
	}
	return CG_TeamName( (team_t) atoi( Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_PLAYERS + playerIdx ], "t" ) ) );
}

qboolean playerActive( int playerIdx ) {
	// player's configstring is set to emptystring if they are not connected
	return *(ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_PLAYERS + playerIdx ]) != 0 ? qtrue : qfalse;
}

team_t getPlayerTeam( int playerIdx ) {
	if ( playerIdx > MAX_CLIENTS ) {
		return TEAM_FREE;
	}
	return (team_t) atoi( Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_PLAYERS + playerIdx ], "t" ) );
}

gametype_t getGameType() {
	return (gametype_t) atoi( Info_ValueForKey( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SERVERINFO ], "g_gametype" ) );
}

clSnapshot_t *previousSnap() {
	int curSnap = ctx->cl.snap.messageNum & PACKET_MASK;
	for ( int snapIdx = ( curSnap - 1 ) & PACKET_MASK; snapIdx != curSnap; snapIdx = ( snapIdx - 1 ) & PACKET_MASK ) {
		if ( ctx->cl.snapshots[ snapIdx ].valid ) {
			return &ctx->cl.snapshots[ snapIdx ];
		}
	}
	return NULL;
}

long getCurrentTime() {
	return ctx->cl.snap.serverTime - atoi( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_LEVEL_START_TIME ] );
}
