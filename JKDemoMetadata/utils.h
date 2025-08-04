#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <inttypes.h>

const wchar_t *utf8BytesToString( const char *utf8 );
const char *cp437toUTF8( const char *cp437 );
const char *cp1252toUTF8( const char *cp1252 );

const char *getPlayerName( int playerIdx );
const char *getPlayerNameUTF8( int playerIdx );

int playerSkill( int playerIdx );

const char *CG_TeamName(team_t team);
team_t OtherTeam(team_t team);
const char *getPlayerTeamName( int playerIdx );
qboolean playerActive( int playerIdx );
team_t getPlayerTeam( int playerIdx );
gametype_t getGameType();
uint64_t getUniqueId( int playerIdx );
const char *getNewmodId( int playerIdx );

clSnapshot_t *previousSnap();

long getCurrentTime();

#endif
