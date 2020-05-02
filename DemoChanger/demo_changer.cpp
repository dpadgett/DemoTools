// demo_merger.cpp : Defines the entry point for the console application.
//

#include "deps.h"
#include "client/client.h"
#include "demo_utils.h"
#include "demo_common.h"
#include "utils.h"
#include "jansson.h"

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

typedef struct {
	char filename[MAX_OSPATH];
	fileHandle_t fp;
	demoContext_t *ctx;
	int firstServerCommand;
	int framesSaved;
	char *lastTeamInfo;
	int currentMap;
	qboolean eos;
	// idx of merged server command seq where the last server cmd from this demo was
	int serverCommandSyncIdx;
} demoEntry_t;

void FreeMsg( msg_t *msg ) {
	free( msg->data );
	free( msg );
}

// returns demoFinished
msg_t *ReadNextMessageRaw( demoEntry_t *demo ) {
	msg_t *msg = (msg_t *) calloc( 1, sizeof( msg_t ) );
	byte *msgData = (byte *) calloc( MAX_MSGLEN, 1 );
	MSG_Init( msg, msgData, MAX_MSGLEN );
	if ( !CL_ReadDemoMessage( demo->fp, msg ) ) {
		return nullptr;
	}
	return msg;
}

msg_t *ReadNextMessage( demoEntry_t *demo ) {
	msg_t *msg;
	int entityEventSequence = 0;
	if ( ctx->cl.snap.valid ) {
		entityEventSequence = ctx->cl.snap.ps.entityEventSequence;
	}
	while ( ( msg = ReadNextMessageRaw( demo ) ) != nullptr ) {
		int lastSnapFlags = ctx->cl.snap.snapFlags;
		qboolean lastSnapValid = ctx->cl.snap.valid;
		try {
			CL_ParseServerMessage( msg );
		} catch ( int ) {
			// thrown code means it wasn't a fatal error, so we can still dump what we had
			FreeMsg( msg );
			return nullptr;
		}

		if ( !ctx->cl.newSnapshots ) {
			FreeMsg( msg );
			continue;
		}
		ctx->cl.snap.ps.entityEventSequence = entityEventSequence;
		if ( lastSnapValid && ( ( lastSnapFlags ^ ctx->cl.snap.snapFlags ) & SNAPFLAG_SERVERCOUNT ) != 0 ) {
			demo->currentMap++;
		}
		if ( ctx->clc.serverCommandSequence - ctx->clc.lastExecutedServerCommand > MAX_RELIABLE_COMMANDS ) {
			ctx->clc.lastExecutedServerCommand = ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10; // fudge factor
		}
		demo->firstServerCommand = ctx->clc.lastExecutedServerCommand;
		int firstNonEmptyCommand = 0;
		// process any new server commands
		for ( ; ctx->clc.lastExecutedServerCommand <= ctx->clc.serverCommandSequence; ctx->clc.lastExecutedServerCommand++ ) {
			char *command = ctx->clc.serverCommands[ ctx->clc.lastExecutedServerCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( cmd[0] && firstNonEmptyCommand == 0 ) {
				firstNonEmptyCommand = ctx->clc.lastExecutedServerCommand;
			}
			if ( !strcmp( cmd, "cs" ) ) {
				CL_ConfigstringModified();
			} else if ( !strcmp( cmd, "tinfo" ) ) {
				Q_strncpyz( demo->lastTeamInfo, command, MAX_STRING_CHARS );
			}
		}
		if ( demo->firstServerCommand == ctx->clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 10 && firstNonEmptyCommand > 0 ) {
			demo->firstServerCommand = firstNonEmptyCommand;
		}
		return msg;
	}
	return nullptr;
}

// returns true if demo context could be parsed
qboolean ParseDemoContext( demoEntry_t *demo ) {
	// clear any existing state
	demoContext_t *oldCtx;
	oldCtx = ctx;
	ctx = demo->ctx;
	// load initial state from demo
	msg_t *msg;
	if ( ( msg = ReadNextMessage( demo ) ) != nullptr ) {
		ctx = oldCtx;
		FreeMsg( msg );
		return qtrue;
	}
	ctx = oldCtx;
	return qfalse;
}

int ParseTime( char *timeStr ) {
	int time = -1;
	if ( strlen( timeStr ) != 12 ) {
		printf( "Invalid time format %s.  Must be hh:mm:ss.sss\n", timeStr );
		return -1;
	}
	timeStr[2] = timeStr[5]  = timeStr[8] = '\0';
	time = atoi( timeStr );
	time = ( time * 60 ) + atoi( &timeStr[3] );
	time = ( time * 60 ) + atoi( &timeStr[6] );
	time = ( time * 1000 ) + atoi( &timeStr[9] );
	return time;
}


typedef struct snapshotEntityNumbers_s {
	int				numSnapshotEntities;
	entityState_t *	snapshotEntities[MAX_SNAPSHOT_ENTITIES];
} snapshotEntityNumbers_t;

/*
=======================
SV_QsortEntityNumbers
=======================
*/
static int QDECL SV_QsortEntityNumbers( const void *a, const void *b ) {
	int ea, eb;

	ea = (*(entityState_t **)a)->number;
	eb = (*(entityState_t **)b)->number;

	if ( ea == eb ) {
		Com_Error( ERR_DROP, "SV_QsortEntityStates: duplicated entity" );
	}

	if ( ea < eb ) {
		return -1;
	}

	return 1;
}


/*
===============
SV_AddEntToSnapshot
===============
*/
static void SV_AddEntToSnapshot( entityState_t *ent, snapshotEntityNumbers_t *eNums, qboolean overwrite = qfalse ) {
	// if we have already added this entity to this snapshot, don't add again
	/*if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
		return;
	}
	svEnt->snapshotCounter = sv.snapshotCounter;*/
	for ( int idx = 0; idx < eNums->numSnapshotEntities; idx++ ) {
		if ( eNums->snapshotEntities[idx]->number == ent->number ) {
			if ( overwrite ) {
				eNums->snapshotEntities[idx] = ent;
			}
			return;
		}
	}

	// if we are full, silently discard entities
	if ( eNums->numSnapshotEntities == MAX_SNAPSHOT_ENTITIES ) {
		return;
	}

	eNums->snapshotEntities[ eNums->numSnapshotEntities ] = ent;
	eNums->numSnapshotEntities++;
}


typedef struct {
	int				score;			// updated by score servercmds
	int				location;		// location index for team mode
	int				health;			// you only get this info about your teammates
	int				armor;
	int				curWeapon;
	int				powerups;		// so can display quad/flag status
	int				attacker;		// client id of last attacker
	entityState_t	es;				// last entity state observed
	int				updateTime;		// last serverTime it was updated
	qboolean		updatedFromTinfo;  // health update was from tinfo
	int				timeResidual;
} basicClientInfo_t;

static basicClientInfo_t ci[MAX_CLIENTS];

static QINLINE void CG_EntityStateToPlayerState( entityState_t *s, playerState_t *ps, int frameTime, playerState_t *prev_ps )
{
	//currently unused vars commented out for speed.. only uncomment if you need them.
	ps->clientNum = s->number;
	VectorCopy( s->pos.trBase, ps->origin );
	VectorCopy( s->pos.trDelta, ps->velocity );
	ps->saberLockFrame = s->forceFrame;
	ps->legsAnim = s->legsAnim;
	ps->torsoAnim = s->torsoAnim;
	ps->legsFlip = s->legsFlip;
	ps->torsoFlip = s->torsoFlip;
	ps->clientNum = s->clientNum;
	ps->saberMove = s->saberMove;

	
	VectorCopy( s->apos.trBase, ps->viewangles );

	ps->fd.forceMindtrickTargetIndex = s->trickedentindex;
	ps->fd.forceMindtrickTargetIndex2 = s->trickedentindex2;
	ps->fd.forceMindtrickTargetIndex3 = s->trickedentindex3;
	ps->fd.forceMindtrickTargetIndex4 = s->trickedentindex4;

	ps->electrifyTime = s->emplacedOwner;

	ps->speed = s->speed;

	ps->genericEnemyIndex = s->genericenemyindex;

	ps->activeForcePass = s->activeForcePass;

	ps->movementDir = s->angles2[YAW];

	ps->eFlags = s->eFlags;

	ps->saberInFlight = s->saberInFlight;
	ps->saberEntityNum = s->saberEntityNum;

	ps->fd.forcePowersActive = s->forcePowersActive;

	if (s->bolt1)
	{
		ps->duelInProgress = qtrue;
	}
	else
	{
		ps->duelInProgress = qfalse;
	}

	/*if (s->bolt2)
	{
		ps->dualBlade = qtrue;
	}
	else
	{
		ps->dualBlade = qfalse;
	}*/

	ps->emplacedIndex = s->otherEntityNum2;

	ps->saberHolstered = s->saberHolstered; //reuse bool in entitystate for players differently

	ps->genericEnemyIndex = -1; //no real option for this

	//The client has no knowledge of health levels (except for the client entity)
	if (s->eFlags & EF_DEAD)
	{
		ps->stats[STAT_HEALTH] = 0;
		if (prev_ps->pm_type == PM_DEAD) {
			ps->stats[STAT_DEAD_YAW] = prev_ps->stats[STAT_DEAD_YAW];
		} else {
			int attacker = ci[s->number].attacker;
			if ( attacker < 0 || attacker >= MAX_CLIENTS || attacker == s->number ) {
				// look at self
				ps->stats[STAT_DEAD_YAW] = s->angles[YAW];
			} else {
				entityState_t *attacker_es = &ci[attacker].es;
				vec3_t dir;
				VectorSubtract (attacker_es->pos.trBase, s->pos.trBase, dir);
				ps->stats[STAT_DEAD_YAW] = vectoyaw ( dir );
			}
		}
		ps->pm_type = PM_DEAD;
	}
	else
	{
		if ( ci[s->number].updateTime + 3000 >= frameTime ) {
			ps->stats[STAT_HEALTH] = ci[s->number].health;
			ps->stats[STAT_ARMOR] = ci[s->number].armor;
		} else if ( prev_ps->stats[STAT_HEALTH] > 0 ) {
			ps->stats[STAT_HEALTH] = prev_ps->stats[STAT_HEALTH];
			ps->stats[STAT_ARMOR] = prev_ps->stats[STAT_ARMOR];
		} else {
			ps->stats[STAT_HEALTH] = 100;
		}
	}

	/*if ( ps->externalEvent ) {
		s->event = ps->externalEvent;
		s->eventParm = ps->externalEventParm;
	} else if ( ps->entityEventSequence < ps->eventSequence ) {
		int		seq;

		if ( ps->entityEventSequence < ps->eventSequence - MAX_PS_EVENTS) {
			ps->entityEventSequence = ps->eventSequence - MAX_PS_EVENTS;
		}
		seq = ps->entityEventSequence & (MAX_PS_EVENTS-1);
		s->event = ps->events[ seq ] | ( ( ps->entityEventSequence & 3 ) << 8 );
		s->eventParm = ps->eventParms[ seq ];
		ps->entityEventSequence++;
	}*/
	if ( s->event ) {
		ps->externalEvent = s->event; // & ~EV_EVENT_BITS;
		ps->externalEventParm = s->eventParm;
	}

	ps->weapon = s->weapon;
	ps->groundEntityNum = s->groundEntityNum;

	for ( int i = 0 ; i < MAX_POWERUPS ; i++ ) {
		if (s->powerups & (1 << i))
		{
			if (i == 4 || i == 5) {
				ps->powerups[i] = INT_MAX;
			} else {
				ps->powerups[i] = frameTime + 1000;
			}
		}
		else
		{
			ps->powerups[i] = 0;
		}
	}

	ps->loopSound = s->loopSound;
	ps->generic1 = s->generic1;
	if ( ps->pm_type == PM_DEAD ) {
		ps->viewheight = DEAD_VIEWHEIGHT;
	} else if ( s->legsAnim == BOTH_CROUCH1WALK || s->legsAnim == BOTH_CROUCH1IDLE ) {
		// stole from CG_CalcMuzzlePoint, is it right?
		ps->viewheight = CROUCH_VIEWHEIGHT;
	} else {
		ps->viewheight = DEFAULT_VIEWHEIGHT;
	}
	ps->jetpackFuel = ps->cloakFuel = 100;
	if (s->eFlags & EF_RAG) {
		// seems this is only set if falling to death.
		if (prev_ps->fallingToDeath > 0)
			ps->fallingToDeath = prev_ps->fallingToDeath;
		else
			ps->fallingToDeath = frameTime;
	}
	if ( s->pos.trTime > 0 )
		ps->commandTime = s->pos.trTime;
	else
		ps->commandTime = frameTime;

	// following bit claims to not be included in the original in the comment
	ps->weaponstate = s->modelindex2;
	ps->weaponChargeTime = s->constantLight;

	VectorCopy(s->origin2, ps->lastHitLoc);

	ps->isJediMaster = s->isJediMaster;

	ps->holocronBits = s->time2;

	ps->fd.saberAnimLevel = s->fireflag;

	ps->heldByClient = s->heldByClient;
	ps->ragAttach = s->ragAttach;

	ps->iModelScale = s->iModelScale;

	ps->brokenLimbs = s->brokenLimbs;

	ps->hasLookTarget = s->hasLookTarget;
	ps->lookTarget = s->lookTarget;

	ps->customRGBA[0] = s->customRGBA[0];
	ps->customRGBA[1] = s->customRGBA[1];
	ps->customRGBA[2] = s->customRGBA[2];
	ps->customRGBA[3] = s->customRGBA[3];

	ps->m_iVehicleNum = s->m_iVehicleNum;

	ps->persistant[PERS_TEAM] = getPlayerTeam( s->number );
}

/*
=================
CG_ParseTeamInfo

=================
*/
#define TEAMINFO_OFFSET (6)
static void CG_ParseTeamInfo( int frameTime ) {
	int i, client;

	int numSortedTeamPlayers = atoi( Cmd_Argv( 1 ) );
	if ( numSortedTeamPlayers < 0 || numSortedTeamPlayers > TEAM_MAXOVERLAY ) {
		Com_Error( ERR_DROP, "CG_ParseTeamInfo: numSortedTeamPlayers out of range (%d)", numSortedTeamPlayers );
		return;
	}

	for ( i=0; i<numSortedTeamPlayers; i++ ) {
		client = atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 2 ) );
		if ( client < 0 || client >= MAX_CLIENTS ) {
			Com_Error( ERR_DROP, "CG_ParseTeamInfo: bad client number: %d", client );
			return;
		}

		ci[client].location		= atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 3 ) );
		ci[client].health		= atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 4 ) );
		ci[client].armor		= atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 5 ) );
		ci[client].curWeapon	= atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 6 ) );
		ci[client].powerups		= atoi( Cmd_Argv( i*TEAMINFO_OFFSET + 7 ) );
		ci[client].updateTime	= frameTime;
		ci[client].updatedFromTinfo = qtrue;
	}
}


int CG_InClientBitflags(entityState_t *ent, int client)
{
	int checkIn;
	int sub = 0;

	if (client > 47)
	{
		checkIn = ent->trickedentindex4;
		sub = 48;
	}
	else if (client > 31)
	{
		checkIn = ent->trickedentindex3;
		sub = 32;
	}
	else if (client > 15)
	{
		checkIn = ent->trickedentindex2;
		sub = 16;
	}
	else
	{
		checkIn = ent->trickedentindex;
	}

	if (checkIn & (1 << (client-sub)))
	{
		return 1;
	}

	return 0;
}

qboolean BG_InKnockDownOnly( int anim )
{
	switch ( anim )
	{
	case BOTH_KNOCKDOWN1:
	case BOTH_KNOCKDOWN2:
	case BOTH_KNOCKDOWN3:
	case BOTH_KNOCKDOWN4:
	case BOTH_KNOCKDOWN5:
		return qtrue;
	}
	return qfalse;
}

static long eventReceived[MAX_GENTITIES];
static demoContext_t mergedCtx;
// bitmask of which clients had this ent in cur/prev snaps.
static int32_t ent_owners[2][MAX_GENTITIES];
static int ent_owner_idx = 0;
int RunMerge(int clientNum, char **demos, int numDemos, char *outFilename)
{
	cl_shownet->integer = 0;
	//printf( "JKDemoMetadata v" VERSION " loaded\n");

	demoEntry_t *entryList = (demoEntry_t *) calloc( numDemos, sizeof( demoEntry_t ) );
	{ // debugger fucks up idx without this
		for ( int idx = 0; idx < numDemos; idx++ ) {
			Q_strncpyz( entryList[idx].filename, demos[idx], sizeof( entryList[idx].filename ) );
			FS_FOpenFileRead( entryList[idx].filename, &entryList[idx].fp, qfalse );
			if ( !entryList[idx].fp ) {
				printf( "Failed to open file %s\n", entryList[idx].filename );
				return -1;
			}
			entryList[idx].ctx = (demoContext_t *) calloc( 1, sizeof( demoContext_t ) );
			entryList[idx].lastTeamInfo = (char *) calloc( MAX_STRING_CHARS, 1 );
			entryList[idx].currentMap = 0;
			entryList[idx].eos = qfalse;
			entryList[idx].serverCommandSyncIdx = 0;
		}
	}

	FILE *outFile;
	if ( !Q_strncmp( outFilename, "-", 2 ) ) {
		outFile = stdout;
#ifdef WIN32
		setmode(fileno(stdout), O_BINARY);
#else
		//freopen( NULL, "wb", stdout );
    // apparently this isn't necessary on linux :?
#endif
	} else {
		outFile = fopen( outFilename, "wb" );
	}
	if ( !outFile ) {
		printf( "Couldn't open output file\n" );
		return -1;
	}

	qboolean demoFinished = qfalse;
	int idx;
	for ( idx = 0; idx < numDemos; idx++ ) {
		if ( !ParseDemoContext( &entryList[idx] ) ) {
			printf( "Failed to parse demo context for demo %s, index %d\n", entryList[idx].filename, idx );
			return -1;
		}
	}
	// start with a base ctx
	// should be the earliest one to pick up earliest configstrings
	// note this could actually be wrong cuz sometimes serverTime resets on map restart
	{
		int minTime = entryList[0].ctx->cl.snap.serverTime;
		int minIdx = 0;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime < minTime ) {
				minTime = entryList[idx].ctx->cl.snap.serverTime;
				minIdx = idx;
			}
		}
		mergedCtx = *entryList[minIdx].ctx;
	}
	mergedCtx.clc.lastExecutedServerCommand = mergedCtx.clc.serverCommandSequence = entryList[0].firstServerCommand;
	mergedCtx.clc.serverCommandSequence--;
	ctx = &mergedCtx;
	int serverCommandOffset = 0;
	int framesSaved = 0;
	memset( ent_owners, 0, sizeof( ent_owners ) );
	memset( ci, 0, sizeof( ci ) );
	memset( eventReceived, 0, sizeof( eventReceived ) );
	int lastTeamInfoTime = -1;
	int firstMissingFrame = -1;
	json_t *missingFrames = json_array();
	while ( true ) {
		// find frame time
		int frameTime = (int) (((unsigned int) -1) >> 1);//MAXINT;
		int frameIdx = -1;
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime < frameTime && !entryList[idx].eos ) {
				frameTime = entryList[idx].ctx->cl.snap.serverTime;
				frameIdx = idx;
			}
		}
		if ( frameIdx == -1 ) {
			ctx = &mergedCtx;
			if (firstMissingFrame != -1) {
				int curFrame = getCurrentTime();
				int missingLength = curFrame - firstMissingFrame;
				if (missingLength >= 500) {
					json_t *missingFrameRange = json_array();
					json_array_append( missingFrameRange, json_integer( firstMissingFrame ) );
					json_array_append( missingFrameRange, json_integer( curFrame ) );
					json_array_append( missingFrames, missingFrameRange );
				}
				firstMissingFrame = -1;
			}
			break;
			/*printf( "ERROR: couldn't find any frame\n" );
			return -1;*/
		}
		if (lastTeamInfoTime == -1) {
			lastTeamInfoTime = frameTime;
		}
		// find all demos that got this frame
		int numMatches = 0;
		int matches[MAX_CLIENTS];
		int numNonMatches = 0;
		int nonMatches[MAX_CLIENTS];
		for ( idx = 0; idx < numDemos; idx++ ) {
			if ( entryList[idx].ctx->cl.snap.serverTime == frameTime && !entryList[idx].eos ) {
				matches[numMatches++] = idx;
			} else if ( entryList[idx].ctx->cl.snap.serverTime > frameTime && !entryList[idx].eos ) {
				// dropped frame, reuse last frame for .5s to smooth blips
				if ( entryList[idx].ctx->cl.snap.serverTime <= frameTime + 500 ) {
					nonMatches[numNonMatches++] = idx;
				}
			}
		}
		// numMatches must always be >0
		ctx = entryList[matches[0]].ctx;
		// note: below advancing commented out because we still need to run through the server commands
		// to pick up updated configstrings
		/*if ( !playerActive( clientNum ) ) {
			// commented out the other block since players usually reconnect, doesn't hurt to keep going and see
			//if ( framesSaved == 0 ) {
				// player didn't connect yet
				//continue;
				//printf("player not in configstrings at frame %d\n", frameTime);
				goto advanceLoop;
			//} else {
				// wrote out some frames so player disconnected
			//	break;
			//}
		}
		if ( framesSaved == 0 && getPlayerTeam( clientNum ) == TEAM_SPECTATOR ) {
			// player didn't enter game yet
			//printf("player not in game at frame %d\n", frameTime);
			goto advanceLoop;
		}*/

		ctx = &mergedCtx;
		ctx->clc.serverMessageSequence++;
		//ctx->clc.reliableAcknowledge; // this doesn't need to be bumped

		// figure out commands
		int firstServerCommand = ctx->clc.lastExecutedServerCommand;
		for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
			int curCommand = firstServerCommand;
			idx = matches[matchIdx];
			for ( int commandNum = entryList[idx].firstServerCommand; commandNum <= entryList[idx].ctx->clc.serverCommandSequence; commandNum++ ) {
				char *command = entryList[idx].ctx->clc.serverCommands[ commandNum & ( MAX_RELIABLE_COMMANDS - 1 ) ];
				Cmd_TokenizeString( command );
				char *cmd = Cmd_Argv( 0 );
				if ( cmd[0] == '\0' )
					continue;
				if ( !strcmp( cmd, "tinfo" ) ) {
					// parse tinfo into ci
					CG_ParseTeamInfo( frameTime );
				}
				if ( !strcmp( cmd, "tinfo" ) || !strcmp( cmd, "tchat" ) ) {
					ctx = entryList[idx].ctx;
					team_t team = getPlayerTeam(ctx->cl.snap.ps.clientNum);
					team_t my_team = getPlayerTeam( clientNum );
					ctx = &mergedCtx;
					if ( team != my_team ) {
						// drop it since it overwrites good ones then which is bad
						continue;
					}
					lastTeamInfoTime = frameTime;
				}
				// search for place to insert it
				bool newCommand = qtrue;
				int start = entryList[idx].serverCommandSyncIdx;
				if ( start == 0 ) {
					// normally start at sync idx even if it's in a previous frame, to account for frame drops
					start = firstServerCommand;
				}
				for ( int snapCmdNum = start; snapCmdNum <= ctx->clc.serverCommandSequence; snapCmdNum++ ) {
					if ( !Q_stricmpn( command, ctx->clc.serverCommands[ snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 ) ], MAX_STRING_CHARS ) ) {
						newCommand = qfalse;
						curCommand = snapCmdNum + 1;
						entryList[idx].serverCommandSyncIdx = curCommand;
						break;
					}
				}
				if ( newCommand ) {
					// insert it at index curCommand
					ctx->clc.serverCommandSequence++;
					ctx->clc.lastExecutedServerCommand++;
					for ( int snapCmdNum = ctx->clc.serverCommandSequence; snapCmdNum > curCommand; snapCmdNum-- ) {
						char *dest = ctx->clc.serverCommands[ snapCmdNum & ( MAX_RELIABLE_COMMANDS - 1 ) ];
						char *src = ctx->clc.serverCommands[ ( snapCmdNum - 1 ) & ( MAX_RELIABLE_COMMANDS - 1 ) ];
						memmove( dest, src, MAX_STRING_CHARS );
					}
					Q_strncpyz( ctx->clc.serverCommands[ curCommand & ( MAX_RELIABLE_COMMANDS - 1 ) ], command, MAX_STRING_CHARS );
					curCommand++;
					entryList[idx].serverCommandSyncIdx = curCommand;
					//printf( "idx:%d cmd:%d Command: %s\n", idx, commandNum, command );
				}
			}
		}
		for ( int commandNum = firstServerCommand; commandNum <= ctx->clc.serverCommandSequence; commandNum++ ) {
			char *command = ctx->clc.serverCommands[commandNum & ( MAX_RELIABLE_COMMANDS - 1 )];
			Cmd_TokenizeString( command );
			char *cmd = Cmd_Argv( 0 );
			if ( !strcmp( cmd, "cs" ) ) {
				// needed so util funcs can work against mergedCtx
				CL_ConfigstringModified();
				/*if ( atoi(Cmd_Argv( 1 )) == 1 ) {
					Com_Printf( "Server id: %s\n", Info_ValueForKey( Cmd_ArgsFrom( 2 ), "sv_serverid" ) );
				}*/
			}
		}
		if (lastTeamInfoTime + 3000 < frameTime) {
			lastTeamInfoTime = frameTime;
			// insert it at index curCommand
			ctx->clc.serverCommandSequence++;
			ctx->clc.lastExecutedServerCommand++;
			char command[MAX_STRING_CHARS];
			team_t myTeam = getPlayerTeam( clientNum );
			int numPlayers = 0;
			basicClientInfo_t *players[MAX_CLIENTS];
			for (int idx = 0; idx < MAX_CLIENTS; idx++) {
				if ( !playerActive( idx ) ) {
					continue;
				}
				team_t team = getPlayerTeam( idx );
				if (team != myTeam) {
					continue;
				}
				players[numPlayers++] = &ci[idx];
			}
			Com_sprintf( command, sizeof( command ), "tinfo %d", numPlayers );
			char clientStr[MAX_STRING_CHARS];
			for (int idx = 0; idx < numPlayers; idx++) {
				basicClientInfo_t *client = players[idx];
				Com_sprintf( clientStr, sizeof( clientStr ), " %d %d %d %d %d %d", client->es.number, 0, client->health, client->armor, client->es.weapon, client->es.powerups );
				Q_strcat( command, sizeof( command ), clientStr );
			}
			Q_strncpyz( ctx->clc.serverCommands[ ctx->clc.serverCommandSequence & ( MAX_RELIABLE_COMMANDS - 1 ) ], command, MAX_STRING_CHARS );
		}

		// make a new snap
		clSnapshot_t snap;
		memset( &snap, 0, sizeof( snap ) );
		snap.parseEntitiesNum = ctx->cl.parseEntitiesNum;
		snap.snapFlags = entryList[matches[0]].ctx->cl.snap.snapFlags & SNAPFLAG_SERVERCOUNT;

		// generate snap.  follows similar logic in sv_snapshot.cc:SV_BuildClientSnapshot
		{
			clSnapshot_t				*frame = &snap;
			snapshotEntityNumbers_t		entityNumbers;
			// buffer to store ents generated from the original demos' ps.
			// save because they're copied out before this gets out of scope.
			entityState_t				dentity[32];
			memset(dentity, 0, sizeof(dentity));

			int32_t *owners = ent_owners[ent_owner_idx];
			ent_owner_idx ^= 1;
			int32_t *prev_owners = ent_owners[ent_owner_idx];

			// clear everything in this snapshot
			entityNumbers.numSnapshotEntities = 0;
			Com_Memset( frame->areamask, 0, sizeof( frame->areamask ) );
			memset( owners, 0, sizeof( *owners ) * MAX_GENTITIES );

			frame->numEntities = 0;

			int matchesMask = 0;
			for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
				idx = matches[matchIdx];
				demoContext_t *dctx = entryList[idx].ctx;
				clSnapshot_t *dsnap = &dctx->cl.snap;

				// add all the entities
				for ( int entIdx = dsnap->parseEntitiesNum; entIdx < dsnap->parseEntitiesNum + dsnap->numEntities; entIdx++ ) {
					entityState_t *ent = &dctx->cl.parseEntities[entIdx & (MAX_PARSE_ENTITIES - 1)];
					SV_AddEntToSnapshot( ent, &entityNumbers );
					owners[ent->number] |= 1 << idx;

					// process any new events
					entityState_t es = *ent;  // copy since we will change some stuff
					// check for event-only entities
					if ( es.eType > ET_EVENTS ) {
						// if this is a player event set the entity number of the client entity number
						if ( es.eFlags & EF_PLAYER_EVENT ) {
							es.number = es.otherEntityNum;
						}

						es.event = es.eType - ET_EVENTS;
					}
					if ( ( es.event & ~EV_EVENT_BITS ) != 0 ) {
						int ev = es.event & ~EV_EVENT_BITS;
						if ( es.eType > ET_EVENTS ) {
							if ( eventReceived[ent->number] >= frameTime - EVENT_VALID_MSEC ) {
								// debounce it
								continue;
							}
							eventReceived[ent->number] = frameTime;
						} else {
							if ( eventReceived[ent->number] == es.event ) {
								// debounce it
								continue;
							}
							/*int lastEvCC = eventReceived[ent->number] >> 8;
							int curEvCC = es.event >> 8;
							if ( ((lastEvCC + 1) & 3) != curEvCC ) {
								printf( "Missed event: lastEvCC %d, curEvCC %d. ent %d client %d event %d et %d\n", lastEvCC, curEvCC, ent->number, es.clientNum, es.event, es.eType );
							}*/
							eventReceived[ent->number] = es.event;
						}
						switch( ev ) {
							case EV_OBITUARY: {
								int			mod;
								int			target, attacker;

								target = ent->otherEntityNum;
								attacker = ent->otherEntityNum2;
								mod = ent->eventParm;

								if ( target < 0 || target >= MAX_CLIENTS ) {
									Com_Error( ERR_DROP, "CG_Obituary: target out of range" );
								}
								ci[target].attacker = attacker;
								if (ci[target].updateTime != frameTime || !ci[target].updatedFromTinfo) {
									ci[target].health = 0;
									ci[target].armor = 0;
									ci[target].updateTime = frameTime;
									ci[target].updatedFromTinfo = qfalse;
								}
								break;
							}
							case EV_USE_ITEM0:
							case EV_USE_ITEM1:
							case EV_USE_ITEM2:
							case EV_USE_ITEM3:
							case EV_USE_ITEM4:
							case EV_USE_ITEM5:
							case EV_USE_ITEM6:
							case EV_USE_ITEM7:
							case EV_USE_ITEM8:
							case EV_USE_ITEM9:
							case EV_USE_ITEM10:
							case EV_USE_ITEM11:
							case EV_USE_ITEM12:
							case EV_USE_ITEM13:
							case EV_USE_ITEM14: {
								if (es.number < MAX_CLIENTS) {
									gitem_t *item;
									holdable_t itemNum = (holdable_t) (ev - EV_USE_ITEM0);
									item = BG_FindItemForHoldable( itemNum );
									if (itemNum == HI_MEDPAC || itemNum == HI_MEDPAC_BIG) {
										if (ci[es.number].updateTime != frameTime || !ci[es.number].updatedFromTinfo) {
											ci[es.number].health += item->quantity;
											ci[es.number].health = Q_min(100, ci[es.number].health);
											ci[es.number].updateTime = frameTime;
											ci[es.number].updatedFromTinfo = qfalse;
										}
									}
								}
								break;
							}
							case EV_ITEM_PICKUP: {
								int		index;

								entityState_t *tent = NULL;
								for ( index = dsnap->parseEntitiesNum; index < dsnap->parseEntitiesNum + dsnap->numEntities; index++ ) {
									tent = &dctx->cl.parseEntities[index & (MAX_PARSE_ENTITIES - 1)];
									if ( tent->number == es.eventParm ) {
										break;
									}
									tent = NULL;
								}
								if ( tent == NULL ) {
									for ( index = 0; index < MAX_GENTITIES; index++ ) {
										tent = &dctx->cl.entityBaselines[index];
										if ( tent->number == es.eventParm ) {
											break;
										}
										tent = NULL;
									}
								}
								if ( tent != NULL ) {
									index = tent->modelindex;
									extern gitem_t bg_itemlist[];
									gitem_t *item = &bg_itemlist[index];
									//printf( "pickup %d %s %d %d(%d)\n", es.number, bg_itemlist[index].classname, es.eType, es.event, es.event >> 8 );
									//int owner = owners[tent->number];
									//int prev_owner = prev_owners[tent->number];
									//printf( "owner %d %d\n", owner, prev_owner );
									if (es.number < MAX_CLIENTS) {
										if (ci[es.number].updateTime != frameTime || !ci[es.number].updatedFromTinfo) {
											if (item->giType == IT_HEALTH) {
												ci[es.number].health += item->quantity;
												ci[es.number].health = Q_min(100, ci[es.number].health);
												ci[es.number].updateTime = frameTime;
												ci[es.number].updatedFromTinfo = qfalse;
											} else if (item->giType == IT_ARMOR) {
												ci[es.number].armor += item->quantity;
												if (item->quantity == 25) {
													// small armor clamps to 100 max
													ci[es.number].armor = Q_min(100, ci[es.number].health);
												} else {
													// large armor has max 200
													ci[es.number].armor = Q_min(200, ci[es.number].health);
												}
												ci[es.number].updateTime = frameTime;
												ci[es.number].updatedFromTinfo = qfalse;
											}
										}
									}
								}
								break;
							}
							case EV_PAIN: {
								// pain events carry the health of the player with them.
								//printf("Pain:%d %d\n", es.clientNum, es.eventParm);
								if (ci[es.clientNum].updateTime != frameTime || !ci[es.clientNum].updatedFromTinfo) {
									ci[es.clientNum].health = es.eventParm;
									if (es.eventParm < 70) {
										ci[es.clientNum].armor = 0;
									}
									ci[es.clientNum].updateTime = frameTime;
									ci[es.clientNum].updatedFromTinfo = qfalse;
								}
								break;
							}
							case EV_TEAM_POWER: {
								int clnum = 0;

								while (clnum < MAX_CLIENTS)
								{
									if (CG_InClientBitflags(&es, clnum))
									{
										if (ci[clnum].updateTime != frameTime || !ci[clnum].updatedFromTinfo) {
											if (es.eventParm == 1) { //eventParm 1 is heal
												ci[clnum].health += 50;
												ci[clnum].health = Q_min(100, ci[clnum].health);
												ci[clnum].updateTime = frameTime;
												ci[clnum].updatedFromTinfo = qfalse;
											} else { //eventParm 2 is force regen
											}
										}
									}
									clnum++;
								}
								break;
							}
							case EV_SHIELD_HIT: {
								int clnum = es.otherEntityNum;
								if (clnum >= 0 && clnum < MAX_CLIENTS) {
									if (ci[clnum].updateTime != frameTime || !ci[clnum].updatedFromTinfo) {
										ci[clnum].armor -= es.time2;
										ci[clnum].armor = Q_max(0, ci[clnum].armor);
										ci[clnum].updateTime = frameTime;
										ci[clnum].updatedFromTinfo = qfalse;
									}
								}
								break;
							}
							case EV_FALL:
							case EV_ROLL: {
								int delta = es.eventParm;
								qboolean knockDownage = qfalse;
								int clnum = es.clientNum;
								int damage;
								if (clnum < 0 || clnum >= MAX_CLIENTS) {
									break;
								}
								basicClientInfo_t *client = &ci[clnum];

								if (client && (client->es.eFlags & EF_RAG))
								{
									break;
								}

								if ( client->es.eType != ET_PLAYER )
								{
									break;		// not in the player model
								}

								if (BG_InKnockDownOnly(client->es.legsAnim))
								{
									if (delta <= 14)
									{
										break;
									}
									knockDownage = qtrue;
								}
								else
								{
									if (delta <= 44)
									{
										break;
									}
								}

								if (knockDownage)
								{
									damage = delta*1; //you suffer for falling unprepared. A lot. Makes throws and things useful, and more realistic I suppose.
								}
								else
								{
									if (getGameType() == GT_SIEGE &&
										delta > 60)
									{ //longer falls hurt more
										damage = delta*1; //good enough for now, I guess
									}
									else
									{
										damage = delta*0.16; //good enough for now, I guess
									}
								}

								if (ci[clnum].updateTime != frameTime || !ci[clnum].updatedFromTinfo) {
										ci[clnum].health -= damage;
										ci[clnum].health = Q_max(0, ci[clnum].health);
										ci[clnum].updateTime = frameTime;
										ci[clnum].updatedFromTinfo = qfalse;
									}
								break;
							}
							default:
								break;
						}
					}
				}
				BG_PlayerStateToEntityStateExtraPolate( &dsnap->ps, &dentity[idx], dsnap->ps.commandTime, qfalse );
				// this pains me to do but it bugs the ui, which tries to display a health meter for these players
				dentity[idx].health = 0;
				//TODO: this must be fixed.  events are duplicated this way.  instead this should first check if there were
				// any temp events already created for this player.  if so use those.  otherwise create a new temp event.
				SV_AddEntToSnapshot( &dentity[idx], &entityNumbers, qtrue );
				owners[dentity[idx].number] |= 1 << idx;

				if ( dsnap->ps.clientNum == clientNum ) {
					// one of the demos merging is actually the players demo, so ps can just be copied!
					snap.ps = dsnap->ps;
				}

				matchesMask |= 1 << idx;
			}

			// any ents that are missing from this snap but we had in the last snap, assume they are supposed to be gone
			for ( int entIdx = 0; entIdx < MAX_GENTITIES; entIdx++ ) {
				if ( (prev_owners[entIdx] & matchesMask) != 0 && owners[entIdx] == 0 ) {
					owners[entIdx] = (prev_owners[entIdx] & matchesMask);
				}
			}

			for ( int nonMatchIdx = 0; nonMatchIdx < numNonMatches; nonMatchIdx++ ) {
				idx = nonMatches[nonMatchIdx];
				demoContext_t *dctx = entryList[idx].ctx;
				clSnapshot_t *dsnap = &dctx->cl.snap;

				// for non matches, they missed the frame (or more) and advanced past it.  look back for the previous snap instead.
				for ( int snapIdx = dsnap->messageNum - 1; snapIdx >= 0 && (snapIdx & PACKET_MASK) != (dsnap->messageNum & PACKET_MASK); snapIdx-- ) {
					if ( dctx->cl.snapshots[snapIdx & PACKET_MASK].valid ) {
						dsnap = &dctx->cl.snapshots[snapIdx & PACKET_MASK];
						break;
					}
				}
				if ( dsnap == &dctx->cl.snap ) {
					// couldn't find previous snap. oh well
					continue;
				}

				// add all the entities if we need to
				for ( int entIdx = dsnap->parseEntitiesNum; entIdx < dsnap->parseEntitiesNum + dsnap->numEntities; entIdx++ ) {
					entityState_t *ent = &dctx->cl.parseEntities[entIdx % MAX_PARSE_ENTITIES];
					// skip if this ent is already in current snap
					if ( owners[ent->number] != 0 ) continue;
					SV_AddEntToSnapshot( ent, &entityNumbers );
					owners[ent->number] |= 1 << idx;
				}
				BG_PlayerStateToEntityStateExtraPolate( &dsnap->ps, &dentity[idx], dsnap->ps.commandTime, qfalse );
				// this pains me to do but it bugs the ui, which tries to display a health meter for these players
				dentity[idx].health = 0;
				if ( owners[dentity[idx].number] == 0 ) {
					SV_AddEntToSnapshot( &dentity[idx], &entityNumbers );
					owners[dentity[idx].number] |= 1 << idx;
				}

				if ( dsnap->ps.clientNum == clientNum && snap.ps.commandTime == 0 ) {
					// one of the demos merging is actually the players demo, so ps can just be copied!
					snap.ps = dsnap->ps;
				}
			}

			// if there were portals visible, there may be out of order entities
			// in the list which will need to be resorted for the delta compression
			// to work correctly.  This also catches the error condition
			// of an entity being included twice.
			qsort( entityNumbers.snapshotEntities, entityNumbers.numSnapshotEntities,
				sizeof( entityNumbers.snapshotEntities[0] ), SV_QsortEntityNumbers );

			// now that all viewpoint's areabits have been OR'd together, invert
			// all of them to make it a mask vector, which is what the renderer wants
			for ( int i = 0 ; i < MAX_MAP_AREA_BYTES/4 ; i++ ) {
				//((int *)frame->areamask)[i] = ((int *)frame->areamask)[i] ^ -1;
				((int *)frame->areamask)[i] = 0;
			}

			entityState_t *player_es = NULL;
			for ( int i = 0; i < entityNumbers.numSnapshotEntities; i++ ) {
				entityState_t *ent = entityNumbers.snapshotEntities[i];
				if ( ent->number < MAX_CLIENTS ) {
					ci[ent->number].es = *ent;
				}
				if ( ent->number == clientNum ) {
					player_es = ent;
				}
			}
			for ( int i = 0; i < MAX_CLIENTS; i++ ) {
				if ( !playerActive( i ) ) {
					continue;
				}
				if ( !(ci[i].es.eFlags & EF_DEAD) ) {
					if ( ci[i].health == 0 && (ci[i].updateTime != frameTime || !ci[i].updatedFromTinfo) ) {
						ci[i].health = 125;
						ci[i].armor = 25;
						ci[i].timeResidual = 0;
					}
				}
				ci[i].timeResidual += frameTime - ctx->cl.snap.serverTime;
				while ( ci[i].timeResidual >= 1000 )
				{
					ci[i].timeResidual -= 1000;

					qboolean updated = qfalse;

					// count down health when over max
					if ( ci[i].health > 100 && (ci[i].updateTime != frameTime || !ci[i].updatedFromTinfo) ) { // TODO: fix?
						ci[i].health--;
						updated = qtrue;
					}

					// count down armor when over max
					if ( ci[i].armor > 100 && (ci[i].updateTime != frameTime || !ci[i].updatedFromTinfo) ) {
						ci[i].armor--;
						updated = qtrue;
					}

					if ( updated ) {
						ci[i].updateTime = frameTime;
						ci[i].updatedFromTinfo = qfalse;
					}
				}
			}
			if ( snap.ps.commandTime != 0 ) {
				// already copied ps from one of the demos merging
			} else if ( player_es == NULL ) {
				if (framesSaved == 0) {
					//printf("couldn't find player at frame %d\n", frameTime);
					goto advanceLoop;
				} else if (!playerActive( clientNum ) ) {
					break;
				}
				/*printf( "Couldn't find ent for player %d, skipping frame %d\n", clientNum, frameTime );
				//BIG TODO: dont duplicate this
				// for all demos that have this frame, advance to the next frame.
				for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
					idx = matches[matchIdx];
					if ( !entryList[idx].eos ) {
						ctx = entryList[idx].ctx;
						msg_t *msg = ReadNextMessage( &entryList[idx] );
						if ( msg == nullptr ) {
							entryList[idx].eos = qtrue;
							continue;
						}
						FreeMsg( msg );
					}
				}
				// write next frame as non delta
				framesSaved = 0;
				continue;*/
				// just reuse the last one, player will appear lagged out
				snap.ps = ctx->cl.snap.ps;
				if (firstMissingFrame == -1) {
					firstMissingFrame = frameTime - atoi( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_LEVEL_START_TIME ] );
				}
			} else {
				// convert es->ps
				CG_EntityStateToPlayerState(player_es, &snap.ps, frameTime, &ctx->cl.snap.ps);
				playerState_t *entry_ps = &entryList[matches[0]].ctx->cl.snap.ps;
				if ( entry_ps->pm_type == PM_INTERMISSION ) {
					// force intermission then
					snap.ps.pm_type = PM_INTERMISSION;
					VectorCopy( entry_ps->origin, snap.ps.origin );
					VectorCopy( entry_ps->velocity, snap.ps.velocity );
					VectorCopy( entry_ps->viewangles, snap.ps.viewangles );
				}
				if (firstMissingFrame != -1) {
					int curFrame = frameTime - atoi( ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ CS_LEVEL_START_TIME ] );
					int missingLength = curFrame - firstMissingFrame;
					if (missingLength >= 500) {
						json_t *missingFrameRange = json_array();
						json_array_append( missingFrameRange, json_integer( firstMissingFrame ) );
						json_array_append( missingFrameRange, json_integer( curFrame ) );
						json_array_append( missingFrames, missingFrameRange );
					}
					firstMissingFrame = -1;
				}
			}

			// copy the entity states out
			frame->numEntities = 0;
			for ( int i = 0 ; i < entityNumbers.numSnapshotEntities ; i++ ) {
				entityState_t *ent = entityNumbers.snapshotEntities[i];
				if ( ent->number == clientNum ) {
					// don't add ourself
					continue;
				}
				entityState_t *state = &ctx->cl.parseEntities[ctx->cl.parseEntitiesNum % MAX_PARSE_ENTITIES];
				ctx->cl.parseEntitiesNum++;
				*state = *ent;
				// this should never hit, map should always be restarted first in SV_Frame
				if ( ctx->cl.parseEntitiesNum >= 0x7FFFFFFE ) {
					Com_Error(ERR_FATAL, "ctx->cl.parseEntitiesNum wrapped");
				}
				frame->numEntities++;
			}
		}

		snap.messageNum = ctx->clc.serverMessageSequence;
		snap.serverCommandNum = ctx->clc.serverCommandSequence;
		snap.valid = qtrue;
		snap.serverTime = frameTime;
		snap.ps.pm_flags |= PMF_FOLLOW;

		// copy new snap into structs
		ctx->cl.snap = ctx->cl.snapshots[snap.messageNum & PACKET_MASK] = snap;

		if ( framesSaved > 0 ) {
			writeDeltaSnapshot( firstServerCommand, outFile, qfalse, serverCommandOffset );
			framesSaved++;
		} else {
			writeDemoHeader( outFile );
			writeDeltaSnapshot( firstServerCommand, outFile, qtrue, serverCommandOffset );
			// copy rest
			framesSaved = 1;
		}

		//printf( "Wrote frame at time %d [%d:%02d.%04d]\n", ctx->cl.snap.serverTime, getCurrentTime() / 1000 / 60, (getCurrentTime() / 1000) % 60, getCurrentTime() % 1000 );
		/*printf( "Wrote frame at time %d clients ", ctx->cl.snap.serverTime );
		for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
			printf( "%d ", matches[matchIdx] );
		}
		printf( "\n" );*/

advanceLoop:
		// for all demos that have this frame, advance to the next frame.
		for ( int matchIdx = 0; matchIdx < numMatches; matchIdx++ ) {
			idx = matches[matchIdx];
			if ( !entryList[idx].eos ) {
				ctx = entryList[idx].ctx;
				msg_t *msg = ReadNextMessage( &entryList[idx] );
				if ( msg == nullptr ) {
					entryList[idx].eos = qtrue;
					continue;
				}
				FreeMsg( msg );
			}
		}
	}

	{
		// finish up
		int len = -1;
		fwrite (&len, 4, 1, outFile);
		fwrite (&len, 4, 1, outFile);
	}

	for ( idx = 0; idx < numDemos; idx++ ) {
		FS_FCloseFile( entryList[idx].fp );
		free( entryList[idx].ctx );
		free( entryList[idx].lastTeamInfo );
	}
	free( entryList );
	fclose( outFile );

	FILE *metaFile;
	if ( Q_strncmp( outFilename, "-", 2 ) ) {
		metaFile = stdout;
	} else {
		//metaFile = fopen( va( "%s.dm_meta", filename ), "wb" );
		metaFile = NULL;
	}
	if ( metaFile ) {
		json_dumpf( missingFrames, metaFile, JSON_INDENT(2) | JSON_PRESERVE_ORDER );
		fclose( metaFile );
	}

	//system("PAUSE");

	return 0;
}

#define MAX_CUTS 1000

int main(int argc, char** argv)
{
	if ( argc < 4 ) {
		printf( "No file specified.\n"
				"Usage: \"%s\" clientnum demo1.dm_26 demo2.dm_26 ... demoN.dm_26 outfile.dm_26\n"
				"Note: all demoI.dm_26 should be from the same game\n", argv[0] );
		//system( "pause" );
		return -1;
	}

	int clientNum = atoi( argv[1] );
	int numDemos = argc - 3;
	char **demos = &argv[2];
	char *outFile = argv[argc - 1];
	RunMerge( clientNum, demos, numDemos, outFile );
}
