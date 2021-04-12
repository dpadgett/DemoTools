#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "client/client.h"
#include "jansson.h"
#include "main.h"
#include "demo_common.h"
#include "utils.h"

#define VERSION "0.1b"
extern void system( char * ); 

// if any code changes are made, bump this number to track which demos are updated / not updated
// small version history:
// version 4: added "raw" times which are not affected by game pauses.
// version 5: added newmod client id
// version 6: added bookmarks
// version 7: added extraction of server ctfstats
const int kSchemaVersion = 7;

typedef struct info_s {
	long startTime;
	json_t *arr;
	json_t *current;
} info_t;

typedef struct teamInfo_s {
	team_t team;
	info_t info;
} teamInfo_t;

typedef struct newmodInfo_s {
	char newmodId[MAX_STRING_CHARS];
	info_t info;
} newmodInfo_t;

typedef struct nameInfo_s {
	char name[MAX_STRING_CHARS];
	int64_t uniqueId;
	int isBot;
	info_t info;
} nameInfo_t;

typedef struct clientInfoTrack_s {
	info_t *(*getInfo)(void *allInfo);
	qboolean (*changed)(void *allInfo, int clientIdx);
	void (*makeValueNode)(void *allInfo);
	void *allInfos;
	size_t infoSize;
	char *keyPrefix;
	json_t *rootNode;
} clientInfoTrack_t;

static void updateClientInfo(clientInfoTrack_t *cit) {
	for ( int clientIdx = 0; clientIdx < MAX_CLIENTS; clientIdx++ ) {
		void *allInfo = (void *)((char *)cit->allInfos + (clientIdx * cit->infoSize));
		if ( cit->changed( allInfo, clientIdx ) ) {
			// value changed.  close the last node if present
			info_t *info = cit->getInfo( allInfo );
			if ( info->current != NULL ) {
				json_object_set_new( info->current, va( "%s_end_time", cit->keyPrefix ), json_integer( getCurrentTime() ) );
				json_object_set_new( info->current, va( "%s_end_time_raw", cit->keyPrefix ), json_integer( ctx->cl.snap.serverTime ) );
				json_decref( info->current );
			}
			if ( playerActive( clientIdx ) ) {
				// changed should perform the update
				info->startTime = ctx->cl.snap.serverTime;
				info->current = json_object();
				cit->makeValueNode( allInfo );
				json_object_set_new( info->current, va( "%s_start_time", cit->keyPrefix ), json_integer( getCurrentTime() ) );
				json_object_set_new( info->current, va( "%s_start_time_raw", cit->keyPrefix ), json_integer( ctx->cl.snap.serverTime ) );
				if ( info->arr == NULL ) {
					info->arr = json_array();
					json_object_set( cit->rootNode, va( "%d", clientIdx ), info->arr );
				}
				json_array_append( info->arr, info->current );
			} else {
				info->current = NULL;
			}
		}
	}
}

static void finishClientInfo( clientInfoTrack_t *cit, long timestamp, long rawtimestamp ) {
	for ( int clientIdx = 0; clientIdx < MAX_CLIENTS; clientIdx++ ) {
		void *allInfo = (void *)((char *)cit->allInfos + (clientIdx * cit->infoSize));
		info_t *info = cit->getInfo( allInfo );
		if ( info->current != NULL ) {
			// finish last current team node
			json_object_set_new( info->current, va( "%s_end_time", cit->keyPrefix ), json_integer( timestamp ) );
			json_object_set_new( info->current, va( "%s_end_time_raw", cit->keyPrefix ), json_integer( rawtimestamp ) );
			json_decref( info->current );
			info->current = NULL;
			json_decref( info->arr );
			info->arr = NULL;
		}
	}
	if ( cit->rootNode != NULL ) {
		json_decref( cit->rootNode );
		cit->rootNode = NULL;
	}
}

static info_t *getInfoFromTeamInfo( void *teamInfo ) {
	return &( (teamInfo_t *) teamInfo )->info;
}

qboolean updateTeam( void *allInfo, int clientIdx ) {
	teamInfo_t *ti = (teamInfo_t *)allInfo;
	team_t newTeam = getPlayerTeam( clientIdx );
	if ( ti->info.current == NULL || ( ti->info.current != NULL && ( newTeam != ti->team || !playerActive( clientIdx ) ) ) ) {
		ti->team = newTeam;
		return qtrue;
	}
	return qfalse;
}

void makeTeamValueNode( void *allInfo ) {
	teamInfo_t *ti = (teamInfo_t *)allInfo;
	json_object_set_new( ti->info.current, "team", json_string( CG_TeamName( ti->team ) ) );
}

static info_t *getInfoFromNewmodInfo( void *newmodInfo ) {
	return &( (newmodInfo_t *) newmodInfo )->info;
}

qboolean updateNewmod( void *allInfo, int clientIdx ) {
	newmodInfo_t *ni = (newmodInfo_t *)allInfo;
	const char *newNewmodId = getNewmodId( clientIdx );
	if ( ( newNewmodId != NULL && Q_strncmp( newNewmodId, ni->newmodId, sizeof( ni->newmodId ) ) ) ||
         ( newNewmodId == NULL && *ni->newmodId ) ||
          !playerActive( clientIdx ) ) {
    if ( newNewmodId != NULL ) {
      Q_strncpyz( ni->newmodId, newNewmodId, sizeof( ni->newmodId ) );
    } else {
      *ni->newmodId = 0;
    }
		return qtrue;
	}
	return qfalse;
}

void makeNewmodValueNode( void *allInfo ) {
	newmodInfo_t *ni = (newmodInfo_t *)allInfo;
  if ( *ni->newmodId ) {
    json_object_set_new( ni->info.current, "newmod_id", json_string( ni->newmodId ) );
  }
}

static info_t *getInfoFromNameInfo( void *nameInfo ) {
	return &( (nameInfo_t *) nameInfo )->info;
}

qboolean updateName( void *allInfo, int clientIdx ) {
	nameInfo_t *ni = (nameInfo_t *)allInfo;
	const char *newName = getPlayerNameUTF8( clientIdx );
	int newIsBot = playerSkill( clientIdx ) == -1 ? 0 : 1;
	int64_t newUniqueId = getUniqueId( clientIdx );
	if ( ni->info.current == NULL ||
      ( ni->info.current != NULL && (
        Q_strncmp( newName, ni->name, sizeof( ni->name ) ) ||
        ni->isBot != newIsBot ||
        newUniqueId != ni->uniqueId ||
        !playerActive( clientIdx ) ) ) ) {
		Q_strncpyz( ni->name, newName, sizeof( ni->name ) );
		ni->uniqueId = newUniqueId;
		ni->isBot = newIsBot;
		return qtrue;
	}
	return qfalse;
}

#if (defined _MSC_VER)
#define PRIu64 "I64u"
#endif

void makeNameValueNode( void *allInfo ) {
	nameInfo_t *ni = (nameInfo_t *)allInfo;
	json_object_set_new( ni->info.current, "name", json_string( ni->name ) );
	if ( ni->uniqueId != 0 ) {
		int guid_hash = ni->uniqueId & 0xFFFFFFFF;
		int ip_hash = ni->uniqueId >> 32;
		json_object_set_new( ni->info.current, "guid_hash", json_integer( guid_hash ) );
		json_object_set_new( ni->info.current, "ip_hash", json_integer( ip_hash ) );
	}
	json_object_set_new( ni->info.current, "is_bot", json_integer( ni->isBot ) );
}

// maintains the entityState_t of the first time we saw a missile
typedef struct firstMissile_s {
	entityState_t es;
	long snapTime;
	long numFrames;
	long numDirectionChanges;
} firstMissile_t;
firstMissile_t firstMissiles[MAX_GENTITIES];
entityState_t lastMissiles[MAX_GENTITIES];
long eventReceived[MAX_GENTITIES];
json_t *lastFrag[MAX_CLIENTS];
typedef struct clientCorpse_s {
	int number;
	long snapTime;
} clientCorpse_t;
clientCorpse_t lastClientCorpse[MAX_CLIENTS];
typedef struct corpseTrack_s {
	int number;
	entityState_t lastEntityState;
	entityState_t firstEntityState;
	int numSnapsMissing;
	json_t *frag;
} corpseTrack_t;
corpseTrack_t corpseTrack[MAX_CLIENTS + 8]; // BODY_QUEUE_SIZE
int corpseTrackIdx = 0;

void finishCorpse( corpseTrack_t *corpse ) {
	int totalDistance = Distance( corpse->firstEntityState.pos.trBase,
			corpse->lastEntityState.pos.trBase );
	int zDistance = abs( corpse->lastEntityState.pos.trBase[2] -
			corpse->firstEntityState.pos.trBase[2] );
	json_object_set_new( corpse->frag, "target_corpse_travel_distance", json_integer( totalDistance ) );
	json_object_set_new( corpse->frag, "target_corpse_travel_z_distance", json_integer( zDistance ) );
}

typedef struct playerHistory_s {
	entityState_t es;
	long frameTime;
	qboolean valid;
} playerHistory_t;
// minimum frame time is 1msec, so to save 1 second of history we need 1000
playerHistory_t playerHistory[MAX_CLIENTS][1000];
int nextPlayerHistoryIdx = 0;

void updateClients( entityState_t **clients, long frameTime ) {
	for ( int idx = 0; idx < MAX_CLIENTS; idx++ ) {
		if ( clients[idx] != NULL ) {
			playerHistory[idx][nextPlayerHistoryIdx].es = *clients[idx];
			playerHistory[idx][nextPlayerHistoryIdx].frameTime = frameTime;
			playerHistory[idx][nextPlayerHistoryIdx].valid = qtrue;
		} else {
			playerHistory[idx][nextPlayerHistoryIdx].valid = qfalse;
		}
	}
	nextPlayerHistoryIdx = ( nextPlayerHistoryIdx + 1 ) % 1000;
}

// computes the distance the given player moved in the last second
float getPlayerMovementLastSecond( int playerIdx, long frameTime ) {
	float totalDistance = 0;
	vec3_t *nextPos = NULL;
	for ( int currentIdx = ( nextPlayerHistoryIdx + 1000 - 1 ) % 1000;
			currentIdx != nextPlayerHistoryIdx &&
				playerHistory[playerIdx][currentIdx].valid == qtrue &&
				( frameTime - playerHistory[playerIdx][currentIdx].frameTime ) < 1000;
			currentIdx = ( currentIdx + 1000 - 1 ) % 1000 ) {
		if ( nextPos != NULL ) {
			totalDistance += Distance( *nextPos, playerHistory[playerIdx][currentIdx].es.pos.trBase );
		}
		nextPos = &playerHistory[playerIdx][currentIdx].es.pos.trBase;
	}
	return totalDistance;
}

entityState_t makerEnts[MAX_GENTITIES];

int main( int argc, char **argv ) {
	memset( playerHistory, 0, sizeof( playerHistory ) );
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");
	if ( argc < 2 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" filename.dm_26 [live]\n"
				"In live mode, the file is tailed until intermission.\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	char *filename = argv[1];
	fileHandle_t fp;
	FS_FOpenFileRead( filename, &fp, qfalse );
	if ( !fp ) {
		printf( "File %s not found.\n", filename );
		//system( "pause" );
		return -1;
	}

	live_mode = (qboolean) (argc > 2 && !Q_stricmp("live", argv[2]));
	qboolean progress_mode = (qboolean) (argc > 2 && !Q_stricmp("progress", argv[2]));

	// set to qtrue to emit executable .cfg files to recreate the makermod entity state from the demo
	qboolean parseMakerEnts = qfalse;

	memset( lastFrag, 0, sizeof( lastFrag ) );

	json_t *root = json_object();
	json_object_set_new( root, "version", json_integer( kSchemaVersion ) );

	json_t *client = json_object();
	json_object_set( root, "client", client );
	qboolean clientIdInitialized = qfalse;
	json_object_set_new( client, "id", json_integer( -1 ) );

	newmodInfo_t clientNewmodInfo[MAX_CLIENTS];
	memset( clientNewmodInfo, 0, sizeof( clientNewmodInfo ) );

	clientInfoTrack_t clientNewmodTrack;
	clientNewmodTrack.allInfos = &clientNewmodInfo;
	clientNewmodTrack.infoSize = sizeof( *clientNewmodInfo );
	clientNewmodTrack.keyPrefix = "newmod";
	clientNewmodTrack.rootNode = NULL;
	clientNewmodTrack.changed = updateNewmod;
	clientNewmodTrack.getInfo = getInfoFromNewmodInfo;
	clientNewmodTrack.makeValueNode = makeNewmodValueNode;

	nameInfo_t clientNames[MAX_CLIENTS];
	memset( clientNames, 0, sizeof( clientNames ) );

	clientInfoTrack_t clientNamesTrack;
	clientNamesTrack.allInfos = &clientNames;
	clientNamesTrack.infoSize = sizeof( *clientNames );
	clientNamesTrack.keyPrefix = "name";
	clientNamesTrack.rootNode = NULL;
	clientNamesTrack.changed = updateName;
	clientNamesTrack.getInfo = getInfoFromNameInfo;
	clientNamesTrack.makeValueNode = makeNameValueNode;

	teamInfo_t clientTeams[MAX_CLIENTS];
	memset( clientTeams, 0, sizeof( clientTeams ) );

	clientInfoTrack_t clientTeamsTrack;
	clientTeamsTrack.allInfos = &clientTeams;
	clientTeamsTrack.infoSize = sizeof( *clientTeams );
	clientTeamsTrack.keyPrefix = "team";
	clientTeamsTrack.rootNode = NULL;
	clientTeamsTrack.changed = updateTeam;
	clientTeamsTrack.getInfo = getInfoFromTeamInfo;
	clientTeamsTrack.makeValueNode = makeTeamValueNode;

	json_t *maps = json_array();
	json_object_set( root, "maps", maps );

	// each map gets a sub-object
	json_t *map = NULL;
	qboolean mapStartTimeInitialized = qfalse;
	json_t *frags = NULL;
	json_t *ownfrags = NULL;
	json_t *bookmarks = NULL;
	json_t *ctfevents = NULL;
	json_t *scoreRoot = NULL;

	clSnapshot_t previousSnapshot;
	memset( &previousSnapshot, 0, sizeof( previousSnapshot ) );
	long previousTime = 0;
	long previousServerId = -1;

	memset( firstMissiles, 0, sizeof( firstMissiles ) );

	memset( makerEnts, 0, sizeof( makerEnts ) );

	qboolean inIntermission = qfalse;
	qboolean finalScores = qfalse;

	qboolean demoFinished = qfalse;
	int lastReportTime = 0;
	while ( !demoFinished && !( live_mode && finalScores ) ) {
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

		if ( !ctx->cl.newSnapshots ) {
			continue;
		}
    if ( progress_mode && ctx->cl.snap.serverTime > lastReportTime + 10000 ) {
      Com_Printf( "read snapshot at time %d\n", ctx->cl.snap.serverTime );
      lastReportTime = ctx->cl.snap.serverTime;
    }
		/*if (cl.snap.serverTime == 1626964134) {
			printf("at bad time");
		}*/
		// jump ahead to resolve a new serverId as we need to know if it changed before doing anything
		const char *systemInfo = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
		int start = Q_max( ctx->clc.lastExecutedServerCommand, ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 1 );
		ctx->clc.lastExecutedServerCommand = start;
		for (int cmdIdx = start; cmdIdx <= ctx->clc.serverCommandSequence; cmdIdx++ ) {
			char *command = ctx->clc.serverCommands[ cmdIdx & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "cs" ) ) {
				CL_ConfigstringModified();
				systemInfo = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
			}
		}
		ctx->cl.serverId = atoi( Info_ValueForKey( systemInfo, "sv_serverid" ) );
		qboolean trashCurrentMap = qfalse;
		if ( previousSnapshot.valid && ctx->cl.snap.serverTime < previousSnapshot.serverTime ) {
			// time warped backwards, this could happen right after a gamestate - a few snaps can be sent with old
			// timebase until client acks the gamestate.  in this case it's somewhat expected that the client drops
			// these snaps.  in our case we already processed them, but we just throw out what we did so far and start
			// over.
			trashCurrentMap = qtrue;
			previousServerId = -1;
		}
		// unfortunately using serverId is buggy for map_restart since game waits some frames before sending the new cs
		if ( (ctx->cl.snap.snapFlags ^ previousSnapshot.snapFlags) & SNAPFLAG_SERVERCOUNT || map == NULL || trashCurrentMap ) {
			finishClientInfo( &clientTeamsTrack, previousTime, previousSnapshot.serverTime );
			finishClientInfo( &clientNamesTrack, previousTime, previousSnapshot.serverTime );
			finishClientInfo( &clientNewmodTrack, previousTime, previousSnapshot.serverTime );

			if ( map != NULL ) {
				json_object_set_new( map, "map_end_time", json_integer( previousTime ) );
				json_decref( map );
				if ( trashCurrentMap ) {
					json_array_remove( maps, json_array_size( maps ) - 1 );
					json_delete( map );
				}
				map = NULL;
			}

			// if we didn't receive yet the new gamestate, skip this snap
			if (previousSnapshot.valid && ctx->cl.serverId == previousServerId) {
				// need to still process commands for map_restart to reset the serverid
				// process any new server commands
				for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence && ctx->cl.serverId == previousServerId; ctx->clc.lastExecutedServerCommand++ ) {
					char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
					Cmd_TokenizeString( command );
					char *cmd = Cmd_Argv( 0 );
					if ( !strcmp( cmd, "cs" ) ) {
						CL_ConfigstringModified();
						systemInfo = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
						ctx->cl.serverId = atoi( Info_ValueForKey( systemInfo, "sv_serverid" ) );
					}
				}
				if (ctx->cl.serverId == previousServerId) {
					continue;
				}
			}
			previousServerId = ctx->cl.serverId;
			map = json_object();
			const char *info = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_SERVERINFO ];
			json_object_set_new( map, "mapname", json_string( cp1252toUTF8( Info_ValueForKey( info, "mapname" ) ) ) );
			mapStartTimeInitialized = qfalse;
			json_object_set_new( map, "map_start_time", json_integer( -1 ) );
			json_object_set_new( map, "map_end_time", json_integer( -1 ) );
			json_object_set_new( map, "serverId", json_integer( ctx->cl.serverId ) );
			json_object_set_new( map, "checksumFeed", json_integer( ctx->clc.checksumFeed ) );
			json_object_set_new( root, "sv_hostname", json_string( cp1252toUTF8( Info_ValueForKey( info, "sv_hostname" ) ) ) );
			json_object_set_new( root, "g_motd", json_string( cp1252toUTF8( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_MOTD ] ) ) );
			json_array_append( maps, map );

			clientNamesTrack.rootNode = json_object();
			json_object_set( map, "names", clientNamesTrack.rootNode );
			clientTeamsTrack.rootNode = json_object();
			json_object_set( map, "teams", clientTeamsTrack.rootNode );
			clientNewmodTrack.rootNode = json_object();
			json_object_set( map, "newmod", clientNewmodTrack.rootNode );
			if ( frags != NULL ) {
				json_decref( frags );
			}
			frags = json_array();
			json_object_set( map, "frags", frags );
			if ( ownfrags != NULL ) {
				json_decref( ownfrags );
			}
			ownfrags = json_array();
			json_object_set( map, "ownfrags", ownfrags );
			if ( bookmarks != NULL ) {
				json_decref( bookmarks );
			}
			bookmarks = json_array();
			json_object_set( map, "bookmarks", bookmarks );
			if ( ctfevents != NULL ) {
				json_decref( ctfevents );
			}
			ctfevents = json_array();
			json_object_set( map, "ctfevents", ctfevents );
			if ( scoreRoot != NULL ) {
				json_decref( scoreRoot );
			}
			scoreRoot = json_object();
			json_object_set( map, "scores", scoreRoot );
			json_object_set_new( scoreRoot, "is_final", json_integer( 0 ) );
			inIntermission = qfalse;
			finalScores = qfalse;
		}

		
		if ( ctx->cl.snap.ps.pm_type == PM_INTERMISSION && scoreRoot != NULL ) {
			inIntermission = qtrue;
		}

		// process any new server commands
		for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
			char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "cs" ) ) {
				if ( strcmp( Cmd_Argv( 1 ), "21" ) || ctx->cl.snap.serverTime - atoi( Cmd_Argv( 2 ) ) <= getCurrentTime() ) {
					// allow time only to be set backwards, not forwards
					CL_ConfigstringModified();
				} else {
					Com_Printf("Skipping command %s since it fast forwards time %d to %d\n", command, getCurrentTime(), ctx->cl.snap.serverTime - atoi( Cmd_Argv( 2 ) ));
				}
			} else if ( !strcmp( cmd, "scores" ) ) {
				if ( scoreRoot != NULL && finalScores == qfalse ) {
					finalScores = inIntermission;
					if ( finalScores ) {
						json_object_set_new( scoreRoot, "is_final", json_integer( 1 ) );
					}
					// end of game scoreboard
					int i, powerups, readScores, scoreOffset;

					int numScores = atoi( Cmd_Argv( 1 ) );

					readScores = numScores;

					if (readScores > MAX_CLIENT_SCORE_SEND)
						readScores = MAX_CLIENT_SCORE_SEND;

					if ( numScores > MAX_CLIENTS )
						numScores = MAX_CLIENTS;

					numScores = readScores;

					json_object_set_new( scoreRoot, "red_score", json_integer( atoi( Cmd_Argv( 2 ) ) ) );
					json_object_set_new( scoreRoot, "blue_score", json_integer( atoi( Cmd_Argv( 3 ) ) ) );

					json_t *playerScores[] = { json_array(), json_array(), json_array(), json_array() };
					json_object_set( scoreRoot, "freeplayers", playerScores[0] );
					json_object_set( scoreRoot, "redplayers", playerScores[1] );
					json_object_set( scoreRoot, "blueplayers", playerScores[2] );
					json_object_set( scoreRoot, "specplayers", playerScores[3] );

					scoreOffset = SCORE_OFFSET;
					if ((Cmd_Argc() - 4) % numScores == 0 && (Cmd_Argc() - 4) / numScores != scoreOffset)
					{
						// some nonstandard addition to scoreboard, assume trailing stuff can be ignored
						scoreOffset = (Cmd_Argc() - 4) / numScores;
					}

					score_t scores[ MAX_CLIENTS ];
					for ( i=0; i<readScores; i++ ) {
						scores[i].client				= atoi( Cmd_Argv( i*scoreOffset +  4 ) );
						scores[i].score					= atoi( Cmd_Argv( i*scoreOffset +  5 ) );
						scores[i].ping					= atoi( Cmd_Argv( i*scoreOffset +  6 ) );
						scores[i].time					= atoi( Cmd_Argv( i*scoreOffset +  7 ) );
						scores[i].scoreFlags			= atoi( Cmd_Argv( i*scoreOffset +  8 ) );
						powerups						= atoi( Cmd_Argv( i*scoreOffset +  9 ) );
						scores[i].accuracy				= atoi( Cmd_Argv( i*scoreOffset + 10 ) );
						scores[i].impressiveCount		= atoi( Cmd_Argv( i*scoreOffset + 11 ) );
						scores[i].excellentCount		= atoi( Cmd_Argv( i*scoreOffset + 12 ) );
						scores[i].gauntletCount			= atoi( Cmd_Argv( i*scoreOffset + 13 ) );
						scores[i].defendCount			= atoi( Cmd_Argv( i*scoreOffset + 14 ) );
						scores[i].assistCount			= atoi( Cmd_Argv( i*scoreOffset + 15 ) );
						scores[i].perfect				= atoi( Cmd_Argv( i*scoreOffset + 16 ) );
						scores[i].captures				= atoi( Cmd_Argv( i*scoreOffset + 17 ) );

						if ( scores[i].client < 0 || scores[i].client >= MAX_CLIENTS )
							scores[i].client = 0;

						json_t *score = json_object();

						json_object_set_new( score, "client", json_integer( scores[i].client ) );
						json_object_set_new( score, "client_name", json_string( getPlayerNameUTF8( scores[i].client ) ) );
						json_object_set_new( score, "team", json_string( getPlayerTeamName( scores[i].client ) ) );
						json_object_set_new( score, "score", json_integer( scores[i].score ) );
						json_object_set_new( score, "ping", json_integer( scores[i].ping ) );
						json_object_set_new( score, "is_bot", json_integer( playerSkill( scores[i].client ) == -1 ? 0 : 1 ) );
						json_object_set_new( score, "time", json_integer( scores[i].time ) );
						json_object_set_new( score, "powerups", json_integer( powerups ) );
						json_object_set_new( score, "captures", json_integer( scores[i].captures ) );

						int playerTeam = getPlayerTeam( scores[i].client );
						if ( playerTeam < 0 || playerTeam >= TEAM_NUM_TEAMS ) {
							playerTeam = TEAM_SPECTATOR;
						}
						json_array_append_new( playerScores[ playerTeam ], score );
					}

					for ( int idx = 0; idx < sizeof( playerScores ) / sizeof( *playerScores ); idx++ ) {
						json_decref( playerScores[ idx ] );
					}
				}
			} else if ( !strcmp( cmd, "ircg" ) ) {
				if ( Cmd_Argc() < 3 ) {
					continue;
				}
				int clientNum = atoi( Cmd_Argv( 1 ) );
				int bodyNum = atoi( Cmd_Argv( 2 ) );
				for ( int corpseIdx = 0; corpseIdx < corpseTrackIdx; corpseIdx++ ) {
					if ( corpseTrack[corpseIdx].number == clientNum ) {
						corpseTrack[corpseIdx].number = bodyNum;
					} else if ( corpseTrack[corpseIdx].number == bodyNum ) {
						// body was repo'd, remove this track
						corpseTrack[corpseIdx].number = -1;
					}
				}
			}/* else if ( !strcmp( cmd, "cp" ) ) {
				printf( "received centerprint\n" );
			}*/
			else if ( !strcmp( cmd, "print" ) && Cmd_Argc() == 2 ) {
				const char *text = Cmd_Argv( 1 );
				const char *match = "unknown cmd ";
				int offset = sizeof( "unknown cmd " ) - 1;
				if ( !Q_stricmpn( text, "unknown cmd ", offset ) && text[strlen(text)-1] == '\n' ) {
					char bookmark[MAX_STRING_CHARS];
					Q_strncpyz( bookmark, &text[offset], MAX_STRING_CHARS );
					bookmark[strlen( bookmark ) - 1] = '\0';
					// Com_Printf( "Found bookmark: %s at time %d\n", bookmark, getCurrentTime() );
					json_t *bm = json_object();
					json_object_set_new( bm, "time", json_integer( getCurrentTime() ) );
					json_object_set_new( bm, "time_raw", json_integer( ctx->cl.snap.serverTime ) );
					long millis = getCurrentTime();
					long seconds = millis / 1000;
					json_object_set_new( bm, "human_time", json_string(
						va( "%02d:%02d:%02d.%03d", seconds / 60 / 60, ( seconds / 60 ) % 60, seconds % 60, millis % 1000 ) ) );
					json_object_set_new( bm, "mark", json_string( bookmark ) );
					json_array_append( bookmarks, bm );
				}
			}
			if ( !strcmp( cmd, "print" ) && Cmd_Argc() == 2 ) {
				const char* text = Cmd_Argv( 1 );
				int offset = sizeof( "^1RED: ^7" ) - 1;
				if ( ( !Q_stricmpn( text, "^1RED: ^7", offset ) || !Q_stricmpn( text, "^4BLUE: ^7", offset+1 )  ) && text[strlen( text ) - 1] == '\n' ) {
					// ctf stats started printing, it should be sent in an uninterrupted sequence of prints
					json_t* jctfstats = json_object();
					json_t* playerStats[MAX_CLIENTS] = {};
					char ctfstats[MAX_STRING_CHARS * 10] = "";
					Q_strcat( ctfstats, sizeof( ctfstats ), text );
					int numLinebreaks = 0;
					char header[MAX_STRING_CHARS] = "";
					char divider[MAX_STRING_CHARS] = "";
					int nameColLen = 0;
					for ( ctx->clc.lastExecutedServerCommand++; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
						char* command = ctx->clc.serverCommands[ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 )];
						Cmd_TokenizeString( command );
						char* cmd = Cmd_Argv( 0 );
						if ( strcmp( cmd, "print" ) ) {
							// end of prints, back up one to process this cmd and continue
							ctx->clc.lastExecutedServerCommand--;
							break;
						}
						text = Cmd_Argv( 1 );
						Q_strcat( ctfstats, sizeof( ctfstats ), text );
						if ( !strcmp( text, "\n" ) ) {
							numLinebreaks++;
							header[0] = '\0';
							if ( numLinebreaks > 1 ) {
								// there should be 2 empty lines and then printing is done
								break;
							}
							continue;
						}
						// top section
						if ( header[0] == '\0' ) {
							// parse header
							Q_strncpyz( header, text, sizeof( header ) );
							header[strlen( text ) / 2] = '\0';
							Q_strncpyz( divider, &text[strlen( text ) / 2], sizeof( divider ) );
							char *nameColEnd = strchr( divider, ' ' );
							*nameColEnd = 0;
							nameColLen = Q_PrintStrlen( divider );
							*nameColEnd = ' ';
						}
						else if ( !strcmp( text + 2, divider + 2 ) ) {
							// divider row
						}
						else {
							// player row, try to find which player it is based on their name
							char name[MAX_STRING_CHARS];
							Q_strncpyz( name, text, sizeof( name ) );
							name[MAX_NAME_LENGTH] = 0;
							int nameEnd = 0;
							while ( *name && ( Q_PrintStrlen( name ) > nameColLen || name[strlen( name ) - 1] == ' ' ) ) {
								name[strlen( name ) - 1] = 0;
								if ( nameEnd == 0 &&  Q_PrintStrlen( name ) <= nameColLen ) {
									nameEnd = strlen( name ) + 1;
								}
							}
							int clientIdx = 0;
							for ( ; clientIdx < MAX_CLIENTS; clientIdx++ ) {
								if ( playerActive( clientIdx ) ) {
									const char* playerName = getPlayerName( clientIdx );
									if ( !strncmp( playerName, name, strlen( name ) ) &&
										(strlen(playerName) <= strlen(name) || !strncmp( playerName + strlen(name), "                                            ", strlen(playerName) - strlen(name) ) ) ) {
										// likely match
										break;
									}
								}
							}
							if ( clientIdx == MAX_CLIENTS ) {
								//Com_Printf( "Couldn't identify: %s\n", name );
								continue;
							}
							//Com_Printf( "Found name: %s client: %d\n", name, clientIdx );
							int statsIdx = nameColLen + 1;
							char stats[MAX_STRING_CHARS];
							Q_strncpyz( stats, text, sizeof( stats ) );
							Q_StripColor( stats );
							json_t* jstats = playerStats[clientIdx];
							if ( jstats == NULL ) {
								jstats = json_object();
								playerStats[clientIdx] = jstats;
								json_object_set_new( jctfstats, va( "%d", clientIdx ), jstats );
							}
							while ( statsIdx < strlen( text ) ) {
								const char* colEnd = Q_strchrs( &divider[statsIdx + 2], " \n" );
								if ( colEnd == NULL ) {
									break;
								}
								int colLen = colEnd - &divider[statsIdx + 2];
								if ( colLen == 0 ) { break; }
								char key[MAX_STRING_CHARS];
								Q_strncpyz( key, &header[statsIdx + 2], colLen + 1 );
								Q_strlwr( key );
								char value[MAX_STRING_CHARS];
								int skipLen = 0;
								for ( ; stats[statsIdx + skipLen] == ' '; skipLen++ ) {}
								Q_strncpyz( value, &stats[statsIdx + skipLen], colLen + 1 - skipLen );
								for ( ; value[strlen( value ) - 1] == ' '; value[strlen( value ) - 1] = '\0' ) {}
								//Com_Printf( "%s: %s\n", key, value );
								statsIdx += colLen + 1;

								json_object_set_new( jstats, key, json_string( value ) );
							}
							//Com_Printf( "Stats: %s\n", &text[nameEnd] );
						}
					}
					//Com_Printf( "Found ctfstats: %s at time %d\n", ctfstats, getCurrentTime() );
					// json_object_set_new( map, "ctfstats", jctfstats );
					// i decided embedding the stats into scoreboard makes more sense
					json_t* scoreLists[] = { json_object_get( scoreRoot, "freeplayers" ), json_object_get( scoreRoot, "redplayers" ), json_object_get( scoreRoot, "blueplayers" ), json_object_get( scoreRoot, "specplayers" ) };
					for ( int scoreListIdx = 0; scoreListIdx < ARRAY_LEN( scoreLists ); scoreListIdx++ ) {
						json_t* scoreList = scoreLists[scoreListIdx];
						if ( scoreList == NULL ) { continue;  }
						for ( int scoreIdx = 0; scoreIdx < json_array_size( scoreList ); scoreIdx++ ) {
							json_t* score = json_array_get( scoreList, scoreIdx );
							json_t* scoreClient = json_object_get( score, "client" );
							if ( scoreClient != NULL && json_integer_value( scoreClient ) < MAX_CLIENTS && playerStats[json_integer_value( scoreClient )] != NULL ) {
								json_object_set( score, "ctfstats", playerStats[json_integer_value( scoreClient )] );
							}
						}
					}
				}
			}
			//Com_Printf( "Received server command %d: %s\n", ctx->clc.lastExecutedServerCommand, command );
		}
		updateClientInfo( &clientNewmodTrack );
		updateClientInfo( &clientNamesTrack );
		updateClientInfo( &clientTeamsTrack );
		if ( !mapStartTimeInitialized ) {
			json_object_set_new( map, "map_start_time", json_integer( getCurrentTime() ) );
			json_object_set_new( map, "map_start_time_raw", json_integer( ctx->cl.snap.serverTime ) );
			mapStartTimeInitialized = qtrue;
		}
		if ( !clientIdInitialized ) {
			json_object_set_new( client, "id", json_integer( ctx->clc.clientNum ) );
			clientIdInitialized = qtrue;
		}
		// process any events
		entityState_t *clientEnts[MAX_CLIENTS];
		entityState_t me;
		memset( clientEnts, 0, sizeof( clientEnts ) );
		for ( int entityIdx = 0; entityIdx < ctx->cl.snap.numEntities; entityIdx++ ) {
			entityState_t *es = &ctx->cl.parseEntities[( ctx->cl.snap.parseEntitiesNum + entityIdx ) & (MAX_PARSE_ENTITIES-1)];
			if ( es->number >= 0 && es->number < MAX_CLIENTS ) {
				clientEnts[es->number] = es;
			}
		}
		BG_PlayerStateToEntityState( &ctx->cl.snap.ps, &me, qfalse );
		clientEnts[me.number] = &me;
		updateClients( clientEnts, ctx->cl.snap.serverTime );
		for ( int corpseIdx = 0; corpseIdx < corpseTrackIdx; corpseIdx++ ) {
			corpseTrack[corpseIdx].numSnapsMissing++;
		}
		for ( int entityIdx = 0; entityIdx < ctx->cl.snap.numEntities; entityIdx++ ) {
			entityState_t *es = &ctx->cl.parseEntities[( ctx->cl.snap.parseEntitiesNum + entityIdx ) & (MAX_PARSE_ENTITIES-1)];
			if ( es->eType > ET_EVENTS ) {
				es->event = es->eType - ET_EVENTS;
				if ( eventReceived[es->number] >= ctx->cl.snap.serverTime - EVENT_VALID_MSEC ) {
					// debounce it
					continue;
				}
				eventReceived[es->number] = ctx->cl.snap.serverTime;
				//Com_Printf( "Event %d fired\n", es->event );
				switch( es->event ) {
					case EV_OBITUARY: {
						int target = es->otherEntityNum;
						int attacker = es->otherEntityNum2;
						int mod = es->eventParm;
						long millis = getCurrentTime();
						long seconds = millis / 1000;
						//Com_Printf( "At time %02d:%02d:%02d.%03d, client %s killed client %s by %s\n",
						//	seconds / 60 / 60, (seconds / 60) % 60, seconds % 60, cl.snap.serverTime % 1000,
						//	getPlayerName( attacker ), getPlayerName( target ), modNames[mod] );
						json_t *frag = json_object();
						//Com_Printf( "frag: %x\n", (int) frag );
						lastFrag[ attacker ] = frag;
						json_object_set_new( frag, "time", json_integer( getCurrentTime() ) );
						json_object_set_new( frag, "time_raw", json_integer( ctx->cl.snap.serverTime ) );
						json_object_set_new( frag, "human_time", json_string(
							va( "%02d:%02d:%02d.%03d", seconds / 60 / 60, (seconds / 60) % 60, seconds % 60, millis % 1000 ) ) );
						json_object_set_new( frag, "attacker", json_integer( attacker ) );
						json_object_set_new( frag, "attacker_name", json_string( getPlayerNameUTF8( attacker ) ) );
						json_object_set_new( frag, "attacker_team", json_string( getPlayerTeamName( attacker ) ) );
						json_object_set_new( frag, "attacker_is_bot", json_integer( playerSkill( attacker ) == -1 ? 0 : 1 ) );
						json_object_set_new( frag, "target", json_integer( target ) );
						json_object_set_new( frag, "target_name", json_string( getPlayerNameUTF8( target ) ) );
						json_object_set_new( frag, "target_team", json_string( getPlayerTeamName( target ) ) );
						json_object_set_new( frag, "target_is_bot", json_integer( playerSkill( target ) == -1 ? 0 : 1 ) );
						json_object_set_new( frag, "target_had_flag", json_integer( 0 ) );
						json_object_set_new( frag, "mod", json_integer( mod ) );
						json_object_set_new( frag, "mod_name", json_string( modNames[mod] ) );
						json_array_append( frags, frag );
						if ( ctx->clc.clientNum == attacker && ctx->clc.clientNum != target && mod != MOD_SUICIDE ) {
							// skip selfkills and suicide grants
							json_array_append( ownfrags, frag );
						}
						// search for attacker distance to target
						if ( attacker < MAX_CLIENTS && target < MAX_CLIENTS &&
								clientEnts[attacker] && clientEnts[target] ) {
							// we have both players in this snap, so we can figure out the distance
							json_object_set_new( frag, "attacker_target_distance",
								json_integer( (int) Distance( clientEnts[attacker]->pos.trBase, clientEnts[target]->pos.trBase ) ) );
						}
						if ( attacker < MAX_CLIENTS && clientEnts[attacker] ) {
							vec3_t clippedDelta;
							VectorCopy( clientEnts[attacker]->pos.trDelta, clippedDelta );
							int zSpeed = (int) abs( clippedDelta[2] );
							clippedDelta[2] = 0;
							float speed = VectorLength( clippedDelta );
							json_object_set_new( frag, "attacker_xy_speed",
									json_integer( (int) speed ) );
							json_object_set_new( frag, "attacker_z_speed",
									json_integer( zSpeed ) );
							float lastSecondDistance = getPlayerMovementLastSecond( attacker, ctx->cl.snap.serverTime );
							json_object_set_new( frag, "attacker_distance_last_second",
									json_integer( (int) lastSecondDistance ) );
						}
						if ( target < MAX_CLIENTS && clientEnts[target] ) {
							vec3_t clippedDelta;
							VectorCopy( clientEnts[target]->pos.trDelta, clippedDelta );
							int zSpeed = (int) abs( clippedDelta[2] );
							clippedDelta[2] = 0;
							float speed = VectorLength( clippedDelta );
							json_object_set_new( frag, "target_xy_speed",
									json_integer( (int) speed ) );
							json_object_set_new( frag, "target_z_speed",
									json_integer( zSpeed ) );
							float lastSecondDistance = getPlayerMovementLastSecond( target, ctx->cl.snap.serverTime );
							json_object_set_new( frag, "target_distance_last_second",
									json_integer( (int) lastSecondDistance ) );
							if ( corpseTrackIdx >= sizeof( corpseTrack ) / sizeof( *corpseTrack ) ) {
								Com_Error( ERR_FATAL, "Corpse track overflow" );
							}
							if ( clientEnts[target]->eFlags & EF_DEAD ) {
								corpseTrack[corpseTrackIdx].frag = frag;
								corpseTrack[corpseTrackIdx].lastEntityState = *clientEnts[target];
								corpseTrack[corpseTrackIdx].firstEntityState = *clientEnts[target];
								corpseTrack[corpseTrackIdx].number = target;
								corpseTrack[corpseTrackIdx].numSnapsMissing = 0;
								corpseTrackIdx++;
							}
						}
						switch ( mod ) {
						case MOD_ROCKET:
						case MOD_BRYAR_PISTOL:
						case MOD_BRYAR_PISTOL_ALT:
						case MOD_BOWCASTER:
						case MOD_CONC:
						case MOD_CONC_ALT:
						case MOD_DEMP2:
						case MOD_FLECHETTE:
						case MOD_REPEATER:
						case MOD_REPEATER_ALT:
						case MOD_THERMAL:
						{
							weapon_t weapon = WP_NONE;
							switch ( mod ) {
								case MOD_ROCKET:
									weapon = WP_ROCKET_LAUNCHER;
									break;
								case MOD_BRYAR_PISTOL:
								case MOD_BRYAR_PISTOL_ALT:
									weapon = WP_BRYAR_PISTOL;
									break;
								case MOD_BOWCASTER:
									weapon = WP_BOWCASTER;
									break;
								case MOD_CONC:
								case MOD_CONC_ALT:
									weapon = WP_CONCUSSION;
									break;
								case MOD_DEMP2:
									weapon = WP_DEMP2;
									break;
								case MOD_FLECHETTE:
									weapon = WP_FLECHETTE;
									break;
								case MOD_REPEATER:
								case MOD_REPEATER_ALT:
									weapon = WP_REPEATER;
									break;
								case MOD_THERMAL:
									weapon = WP_THERMAL;
									break;
							}
							// try and find the rocket
							entityState_t *missile = NULL;
							for ( int rocketIdx = 0; rocketIdx < ctx->cl.snap.numEntities; rocketIdx++ ) {
								entityState_t *es = &ctx->cl.parseEntities[( ctx->cl.snap.parseEntitiesNum + rocketIdx ) & (MAX_PARSE_ENTITIES-1)];
								if ( es->eType == ET_GENERAL &&
										( es->event & ~EV_EVENT_BITS ) == EV_MISSILE_HIT &&
										es->otherEntityNum == target &&
										es->weapon == weapon) {
									// this missile just hit the target
									missile = es;
									break;
								}
							}
							if ( missile != NULL ) {
								//Com_Printf( "Found the missile: %d\n", missile->number );
								firstMissile_t *firstMissile = &firstMissiles[missile->number];
								if ( firstMissile->es.number == missile->number ) {
									//Com_Printf( "Found the initial missile: %d\n", firstMissile->es.number );
									json_object_set_new( frag, "missile_lifetime",
										json_integer( ctx->cl.snap.serverTime - firstMissile->snapTime ) );
									if ( mod == MOD_ROCKET &&
											firstMissile->numDirectionChanges > 2 &&
											firstMissile->numDirectionChanges >= firstMissile->numFrames / 2 ) {
										// rocket bug - the ent is recorded as primary but it's really homing
										json_object_set_new( frag, "mod", json_integer( MOD_ROCKET_HOMING ) );
										json_object_set_new( frag, "mod_name", json_string( modNames[MOD_ROCKET_HOMING] ) );
									}
									entityState_t *lastMissile = &lastMissiles[missile->number];
									// use the missile from 1 previous frame to get pitch, for gravity affected trajectories
									if ( lastMissile->number == missile->number &&
										lastMissile->pos.trType != TR_STATIONARY ) {
										// can record the angle of elevation (pitch)
										vec3_t dir;
										vectoangles( firstMissile->es.pos.trDelta, dir );
										if ( dir[PITCH] < -180 ) {
											dir[PITCH] += 360;
										}
										// theoretically, pitch should now be between -90 (stright down) and +90 (straight up)
										json_object_set_new( frag, "missile_pitch",
											json_integer( -dir[PITCH] ) );
									}
								}
							}
							break;
						}
						default:
							break;
						}
						break;
					}
					case EV_CTFMESSAGE: {
						int clIndex = es->trickedentindex;
						team_t teamIndex = (team_t) es->trickedentindex2;
						int ctfMessage = es->eventParm;
						json_t *ctfevent = json_object();
						json_object_set_new( ctfevent, "time", json_integer( getCurrentTime() ) );
						json_object_set_new( ctfevent, "time_raw", json_integer( ctx->cl.snap.serverTime ) );
						long millis = getCurrentTime();
						long seconds = millis / 1000;
						json_object_set_new( ctfevent, "human_time", json_string(
							va( "%02d:%02d:%02d.%03d", seconds / 60 / 60, (seconds / 60) % 60, seconds % 60, millis % 1000 ) ) );
						switch (ctfMessage)
						{
						case CTFMESSAGE_FRAGGED_FLAG_CARRIER:
							//Com_Printf("Player %s fragged the %s flag carrier\n", getPlayerName( clIndex ), CG_TeamName( OtherTeam( teamIndex ) ) );
							if ( clIndex < MAX_CLIENTS && lastFrag[ clIndex ] != NULL ) {
								//TODO: this may be incorrect if attacker had multiple frags in one frame
								json_object_set_new( lastFrag[ clIndex ], "target_had_flag", json_integer( 1 ) );
							}
							json_object_set_new( ctfevent, "eventtype", json_string( "FRAGGED_FLAG_CARRIER" ) );
							json_object_set_new( ctfevent, "attacker", json_integer( clIndex ) );
							json_object_set_new( ctfevent, "attacker_name", json_string( getPlayerNameUTF8( clIndex ) ) );
							json_object_set_new( ctfevent, "team", json_string( CG_TeamName( OtherTeam( teamIndex ) ) ) );
							break;
						case CTFMESSAGE_FLAG_RETURNED:
							//Com_Printf("%s flag was returned\n", CG_TeamName( teamIndex ) );
							json_object_set_new( ctfevent, "eventtype", json_string( "FLAG_RETURNED" ) );
							json_object_set_new( ctfevent, "team", json_string( CG_TeamName( teamIndex ) ) );
							break;
						case CTFMESSAGE_PLAYER_RETURNED_FLAG:
							//Com_Printf("Player %s returned the %s flag\n", getPlayerName( clIndex ), CG_TeamName( teamIndex ) );
							json_object_set_new( ctfevent, "eventtype", json_string( "PLAYER_RETURNED_FLAG" ) );
							json_object_set_new( ctfevent, "attacker", json_integer( clIndex ) );
							json_object_set_new( ctfevent, "attacker_name", json_string( getPlayerNameUTF8( clIndex ) ) );
							json_object_set_new( ctfevent, "team", json_string( CG_TeamName( teamIndex ) ) );
							break;
						case CTFMESSAGE_PLAYER_CAPTURED_FLAG:
							//Com_Printf("Player %s captured the %s flag\n", getPlayerName( clIndex ), CG_TeamName( teamIndex ) );
							json_object_set_new( ctfevent, "eventtype", json_string( "PLAYER_CAPTURED_FLAG" ) );
							json_object_set_new( ctfevent, "attacker", json_integer( clIndex ) );
							json_object_set_new( ctfevent, "attacker_name", json_string( getPlayerNameUTF8( clIndex ) ) );
							json_object_set_new( ctfevent, "team", json_string( CG_TeamName( teamIndex ) ) );
							break;
						case CTFMESSAGE_PLAYER_GOT_FLAG:
							//Com_Printf("Player %s got the %s flag\n", getPlayerName( clIndex ), CG_TeamName( teamIndex ) );
							json_object_set_new( ctfevent, "eventtype", json_string( "PLAYER_GOT_FLAG" ) );
							json_object_set_new( ctfevent, "attacker", json_integer( clIndex ) );
							json_object_set_new( ctfevent, "attacker_name", json_string( getPlayerNameUTF8( clIndex ) ) );
							json_object_set_new( ctfevent, "team", json_string( CG_TeamName( teamIndex ) ) );
							break;
						default:
							break;
						}
						json_array_append_new( ctfevents, ctfevent );
						break;
					}
					default:
						break;
				}
			} else if ( ( es->eType == ET_BODY || es->eType == ET_PLAYER ) && ( es->eFlags & EF_DEAD ) ) {
				for ( int corpseIdx = 0; corpseIdx < corpseTrackIdx; corpseIdx++ ) {
					if ( corpseTrack[corpseIdx].number == es->number ) {
						// update it
						corpseTrack[corpseIdx].numSnapsMissing = 0;
						corpseTrack[corpseIdx].lastEntityState = *es;
					}
				}
			}
		}
		for ( int corpseIdx = 0; corpseIdx < corpseTrackIdx; corpseIdx++ ) {
			if ( ( corpseTrack[corpseIdx].numSnapsMissing > 1 || corpseTrack[corpseIdx].number == -1 ) ||
					corpseTrack[corpseIdx].lastEntityState.pos.trType == TR_STATIONARY ) {
				finishCorpse( &corpseTrack[corpseIdx] );
				corpseTrack[corpseIdx] = corpseTrack[corpseTrackIdx - 1];
				corpseTrackIdx--;
				corpseIdx--; // should reprocess it now
				continue;
			}
		}
		memset( lastMissiles, 0, sizeof( lastMissiles ) );
		for ( int rocketIdx = 0; rocketIdx < ctx->cl.snap.numEntities; rocketIdx++ ) {
			entityState_t *es = &ctx->cl.parseEntities[( ctx->cl.snap.parseEntitiesNum + rocketIdx ) & (MAX_PARSE_ENTITIES-1)];
			if ( parseMakerEnts ) {
				if ( ( es->eType == ET_MOVER && es->modelindex && es->modelindex2 ) || es->eType == ET_FX ) {
					// see if it already exists in our list
					qboolean exists = qfalse;
					for (int searchIdx = 1; searchIdx < MAX_GENTITIES; searchIdx++) {
						if (makerEnts[searchIdx].number == 0) {
							// ents are allocated contiguiously
							break;
						}
						if (VectorCompare(makerEnts[searchIdx].origin, es->origin) &&
								makerEnts[searchIdx].eType == es->eType &&
								makerEnts[searchIdx].modelindex == es->modelindex &&
								makerEnts[searchIdx].userInt1 != ctx->cl.snap.serverTime) {
							exists = qtrue;
							if (es->eType == ET_FX) {
								es->userInt1 = ctx->cl.snap.serverTime;
								es->userInt2 = es->userInt1 - makerEnts[searchIdx].userInt1;
							}
							makerEnts[searchIdx] = *es;
							break;
						}
					}
					if (!exists) {
						// copy it to an unused location
						for (int unusedIdx = 1; unusedIdx < MAX_GENTITIES; unusedIdx++) {
							if (makerEnts[unusedIdx].number == 0) {
								// slot is unused
								makerEnts[unusedIdx] = *es;
								break;
							}
						}
					}
				}
			}
			if ( es->eType == ET_MISSILE) {
				lastMissiles[es->number] = *es;
			}
		}
		for ( int rocketIdx = 0; rocketIdx < sizeof( lastMissiles ) / sizeof( *lastMissiles ); rocketIdx++ ) {
			if ( firstMissiles[rocketIdx].es.number == 0 && lastMissiles[rocketIdx].number != 0 ) {
				firstMissiles[rocketIdx].es = lastMissiles[rocketIdx];
				firstMissiles[rocketIdx].snapTime = ctx->cl.snap.serverTime;
				firstMissiles[rocketIdx].numFrames = 0;
				firstMissiles[rocketIdx].numDirectionChanges =  0;
			} else if ( firstMissiles[rocketIdx].es.number != 0 && lastMissiles[rocketIdx].number == 0 ) {
				memset( &firstMissiles[rocketIdx], 0, sizeof( *firstMissiles ) );
			} else if ( firstMissiles[rocketIdx].es.number != 0 && lastMissiles[rocketIdx].number != 0 ) {
				// update frame count, direction change if applicable
				firstMissiles[rocketIdx].numFrames++;
				if ( memcmp( firstMissiles[rocketIdx].es.pos.trDelta, lastMissiles[rocketIdx].pos.trDelta, sizeof( vec3_t ) ) ) {
					firstMissiles[rocketIdx].numDirectionChanges++;
				}
			}
		}

		previousSnapshot = ctx->cl.snap;
		previousTime = getCurrentTime();
	}

	finishClientInfo( &clientTeamsTrack, getCurrentTime(), ctx->cl.snap.serverTime );
	finishClientInfo( &clientNamesTrack, getCurrentTime(), ctx->cl.snap.serverTime );
	finishClientInfo( &clientNewmodTrack, getCurrentTime(), ctx->cl.snap.serverTime );

	if ( map != NULL ) {
		json_object_set_new( map, "map_end_time", json_integer( getCurrentTime() ) );
		json_object_set_new( map, "map_end_time_raw", json_integer( ctx->cl.snap.serverTime ) );
		json_decref( map );
		map = NULL;
	}

	for ( int corpseIdx = 0; corpseIdx < corpseTrackIdx; corpseIdx++ ) {
		finishCorpse( &corpseTrack[corpseIdx] );
	}

	json_object_set_new( root, "filesize", json_integer( FS_ReadCount( fp ) ) );

	FS_FCloseFile( fp );

	FILE *metaFile;
	if ( !Q_strncmp( filename, "-", 2 ) ) {
		metaFile = stdout;
	} else {
		metaFile = fopen( va( "%s.dm_meta", filename ), "wb" );
	}
	if ( metaFile ) {
		json_dumpf( root, metaFile, JSON_INDENT(2) | JSON_PRESERVE_ORDER );
		fclose( metaFile );
	}

	if ( parseMakerEnts ) {
		int makerFileIdx = 0;
		int lineCount = 0;
		FILE *makerFile = fopen( va( "%s.maker.%d.cfg", filename, makerFileIdx ), "wb" );
		for ( int makerIdx = 0; makerIdx < MAX_GENTITIES; makerIdx++ ) {
			entityState_t *es = &makerEnts[makerIdx];
			if (es->number == 0) {
				continue;
			}
			char *mplace = "";
			char *mpain = "";
			if (es->eType == ET_MOVER) {
				int offset2 = ctx->cl.gameState.stringOffsets[ CS_MODELS+es->modelindex2 ];
				const char *modelName2 = ctx->cl.gameState.stringData + offset2;
				if ( offset2 != 0 && strlen( modelName2 ) > strlen( "models/map_objects/" ) ) {
					char modelName[MAX_QPATH];
					Q_strncpyz(modelName, modelName2 + strlen( "models/map_objects/" ), sizeof(modelName));
					modelName[strlen(modelName) - 4] = 0;
					mplace = va("mplace %s", modelName);
				}
			} else if (es->eType == ET_FX) {
				int offset = ctx->cl.gameState.stringOffsets[ CS_EFFECTS+es->modelindex ];
				const char *modelName = ctx->cl.gameState.stringData + offset;
				if ( offset != 0 && *modelName ) {
					char modelName2[MAX_QPATH];
					Q_strncpyz(modelName2, modelName, sizeof(modelName2));
					mplace = va("mplacefx %s %d", modelName2, Q_max(50, es->userInt2));
					mpain = "mpain 10 10";
				}
			}
			if (*mplace) {
				fprintf(makerFile, "mmark %d %d %d\n", (int) es->origin[0], (int) es->origin[1], (int) es->origin[2]);
				fprintf(makerFile, "%s\n", mplace);
				fprintf(makerFile, "mrotate %d %d %d\n", (int) es->angles[0], (int) es->angles[1], (int) es->angles[2]);
				if (*mpain) {
					fprintf(makerFile, "%s\n", mpain);
					lineCount++;
				}
				if (es->iModelScale != 100 && es->eType == ET_MOVER) {
					fprintf(makerFile, "mscale %f\n", es->iModelScale/100.0f);
					lineCount++;
				}
				lineCount += 3;
				if (lineCount >= 120) {
					makerFileIdx++;
					lineCount = 0;
					fclose(makerFile);
					makerFile = fopen( va( "%s.maker.%d.cfg", filename, makerFileIdx ), "wb" );
				}

			}
		}
		fclose( makerFile );
	}

	//system( "pause" );
	return 0;
}
