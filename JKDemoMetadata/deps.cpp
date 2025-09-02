#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "server/server.h"
#include "client/client.h"
#include "demo_common.h"

demoContext_t concreteCtx;
demoContext_t *ctx = &concreteCtx;

cvar_t cl_shownet_concrete;
cvar_t *cl_shownet = &cl_shownet_concrete;

cvar_t sv_blockJumpSelect_concrete;
cvar_t *sv_blockJumpSelect = &sv_blockJumpSelect_concrete;

server_t sv;

sharedEntity_t *SV_GentityNum( int num ) {
	Com_Error( ERR_FATAL, "SV_GentityNum not supported" );
	return NULL;
}

#define	MAXPRINTMSG	4096
static char com_errorMessage[MAXPRINTMSG] = {0};
void QDECL Com_Error( int code, const char *fmt, ... ) {
	va_list		argptr;

	va_start( argptr, fmt );
	Q_vsnprintf( com_errorMessage, sizeof( com_errorMessage ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "Com_Error code %d: %s", code, com_errorMessage );
	//system( "pause" );
	if ( code == ERR_FATAL ) {
		exit( code );
	} else {
		throw code;
	}
}

void QDECL Com_Printf( const char *fmt, ... ) {
	va_list argptr;

	va_start( argptr, fmt );
	vfprintf( stderr, fmt, argptr );
	va_end( argptr );
}

void *Z_Malloc(int iSize, memtag_t eTag, qboolean bZeroit, int unusedAlign) {
	if ( bZeroit ) {
		return calloc( 1, iSize );
	} else {
		return malloc( iSize );
	}
}

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif

static FILE *fileHandles[1024];
static char fileNames[1024][MAX_OSPATH];
static int fileHandleCount = 1;
static long bytesRead[1024];
long QDECL FS_FOpenFileRead( const char *filename, fileHandle_t *fileHandle, qboolean uniqueFILE) {
	*fileHandle = fileHandleCount++;
	if ( !Q_strncmp( filename, "-", 2 ) ) {
#ifdef WIN32
		setmode(fileno(stdin), O_BINARY);
#else
		freopen( NULL, "rb", stdin );
#endif
		fileHandles[*fileHandle] = stdin;
		return 0;
	}
	fileHandles[*fileHandle] = fopen( filename, "rb" );
	if ( fileHandles[*fileHandle] == NULL ) {
		*fileHandle = 0;
		return -1;
	}
	bytesRead[*fileHandle] = 0;
	Q_strncpyz( fileNames[*fileHandle], filename, sizeof( *fileNames ) );
	fseek( fileHandles[*fileHandle], 0, SEEK_END );
	long length = ftell( fileHandles[*fileHandle] );
	fseek( fileHandles[*fileHandle], 0, SEEK_SET );
	return length;
}

#ifdef WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

qboolean live_mode = qfalse;
int QDECL FS_Read( void *data, int dataSize, fileHandle_t fileHandle ) {
	int totalRead = 0;
	while ( totalRead < dataSize ) {
		if ( live_mode ) {
			int stepSize = 100;
			for ( int timeWaited = 0; timeWaited < 5000 && feof( fileHandles[fileHandle] ); timeWaited += stepSize ) {
#ifdef WIN32
				Sleep( stepSize );
#else
				usleep( stepSize * 1000 );
#endif
				fclose( fileHandles[fileHandle] );
				fileHandles[fileHandle] = fopen( fileNames[fileHandle], "rb" );
				if ( 0 != fseek( fileHandles[fileHandle], bytesRead[fileHandle], SEEK_SET ) ) {
					return 0;
				}
			}
		}
		if ( feof( fileHandles[fileHandle] ) ) {
			break;
		}
		int read = fread( (void *)((char *)data + totalRead), 1, dataSize - totalRead, fileHandles[fileHandle] );
		totalRead += read;
		bytesRead[fileHandle] += read;
	}
	return totalRead;
}

long QDECL FS_ReadCount( fileHandle_t fileHandle ) {
	return bytesRead[fileHandle];
}

void QDECL FS_FCloseFile( fileHandle_t fileHandle ) {
	fclose( fileHandles[fileHandle] );
	fileHandles[fileHandle] = NULL;
}

/*
=====================
CL_ConfigstringModified
=====================
*/
void CL_ConfigstringModified( void ) {
	char		*old, *s;
	int			i, index;
	char		*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "CL_ConfigstringModified: bad index %i", index );
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = ctx->cl.gameState.stringData + ctx->cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// uber hack to work around base_enhanced forced net settings
	char buf[MAX_INFO_STRING];
	if ( index == CS_SYSTEMINFO ) {
		if ( *Info_ValueForKey( s, "sv_serverid" ) == '\0' ) {
			// just concat them instead of overwriting in this case
			Com_sprintf( buf, sizeof( buf ), "%s%s", old, s );
			s = buf;
		}
	}

	// build the new gameState_t
	oldGs = ctx->cl.gameState;

	Com_Memset( &ctx->cl.gameState, 0, sizeof( ctx->cl.gameState ) );

	// leave the first 0 for uninitialized strings
	ctx->cl.gameState.dataCount = 1;

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = strlen( dup );

		if ( len + 1 + ctx->cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		ctx->cl.gameState.stringOffsets[ i ] = ctx->cl.gameState.dataCount;
		Com_Memcpy( ctx->cl.gameState.stringData + ctx->cl.gameState.dataCount, dup, len + 1 );
		ctx->cl.gameState.dataCount += len + 1;
	}
}

/*
========================
BG_PlayerStateToEntityState

This is done after each set of usercmd_t on the server,
and after local prediction on the client
========================
*/
void BG_PlayerStateToEntityState( playerState_t *ps, entityState_t *s, qboolean snap ) {
	int		i;

	if ( ps->pm_type == PM_INTERMISSION || ps->pm_type == PM_SPECTATOR ) {
		s->eType = ET_INVISIBLE;
	} else if ( ps->stats[STAT_HEALTH] <= GIB_HEALTH ) {
		s->eType = ET_INVISIBLE;
	} else {
		s->eType = ET_PLAYER;
	}

	s->number = ps->clientNum;

	s->pos.trType = TR_INTERPOLATE;
	VectorCopy( ps->origin, s->pos.trBase );
	VectorCopy( ps->origin, s->origin );
	if ( snap ) {
		SnapVector( s->pos.trBase );
	}
	// set the trDelta for flag direction
	VectorCopy( ps->velocity, s->pos.trDelta );

	s->apos.trType = TR_INTERPOLATE;
	VectorCopy( ps->viewangles, s->apos.trBase );
	VectorCopy( ps->viewangles, s->angles );
	if ( snap ) {
		SnapVector( s->apos.trBase );
	}

	s->trickedentindex = ps->fd.forceMindtrickTargetIndex;
	s->trickedentindex2 = ps->fd.forceMindtrickTargetIndex2;
	s->trickedentindex3 = ps->fd.forceMindtrickTargetIndex3;
	s->trickedentindex4 = ps->fd.forceMindtrickTargetIndex4;

	s->forceFrame = ps->saberLockFrame;

	s->emplacedOwner = ps->electrifyTime;

	s->speed = ps->speed;

	s->genericenemyindex = ps->genericEnemyIndex;

	s->activeForcePass = ps->activeForcePass;

	s->angles2[YAW] = ps->movementDir;
	s->legsAnim = ps->legsAnim;
	s->torsoAnim = ps->torsoAnim;

	s->legsFlip = ps->legsFlip;
	s->torsoFlip = ps->torsoFlip;

	s->clientNum = ps->clientNum;		// ET_PLAYER looks here instead of at number
										// so corpses can also reference the proper config
	s->eFlags = ps->eFlags;
	s->eFlags2 = ps->eFlags2;

	s->saberInFlight = ps->saberInFlight;
	s->saberEntityNum = ps->saberEntityNum;
	s->saberMove = ps->saberMove;
	s->forcePowersActive = ps->fd.forcePowersActive;

	if (ps->duelInProgress)
	{
		s->bolt1 = 1;
	}
	else
	{
		s->bolt1 = 0;
	}

	s->otherEntityNum2 = ps->emplacedIndex;

	s->saberHolstered = ps->saberHolstered;

	if (ps->genericEnemyIndex != -1)
	{
		s->eFlags |= EF_SEEKERDRONE;
	}

	if ( ps->stats[STAT_HEALTH] <= 0 ) {
		s->eFlags |= EF_DEAD;
	} else {
		s->eFlags &= ~EF_DEAD;
	}

	if ( ps->externalEvent ) {
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
	}


	s->weapon = ps->weapon;
	s->groundEntityNum = ps->groundEntityNum;

	s->powerups = 0;
	for ( i = 0 ; i < MAX_POWERUPS ; i++ ) {
		if ( ps->powerups[ i ] ) {
			s->powerups |= 1 << i;
		}
	}

	s->loopSound = ps->loopSound;
	s->generic1 = ps->generic1;

	//NOT INCLUDED IN ENTITYSTATETOPLAYERSTATE:
	s->modelindex2 = ps->weaponstate;
	s->constantLight = ps->weaponChargeTime;

	VectorCopy(ps->lastHitLoc, s->origin2);

	s->isJediMaster = ps->isJediMaster;

	s->time2 = ps->holocronBits;

	s->fireflag = ps->fd.saberAnimLevel;

	s->heldByClient = ps->heldByClient;
	s->ragAttach = ps->ragAttach;

	s->iModelScale = ps->iModelScale;

	s->brokenLimbs = ps->brokenLimbs;

	s->hasLookTarget = ps->hasLookTarget;
	s->lookTarget = ps->lookTarget;

	s->customRGBA[0] = ps->customRGBA[0];
	s->customRGBA[1] = ps->customRGBA[1];
	s->customRGBA[2] = ps->customRGBA[2];
	s->customRGBA[3] = ps->customRGBA[3];

	s->m_iVehicleNum = ps->m_iVehicleNum;
}

/*
========================
BG_PlayerStateToEntityStateExtraPolate

This is done after each set of usercmd_t on the server,
and after local prediction on the client
========================
*/
void BG_PlayerStateToEntityStateExtraPolate( playerState_t *ps, entityState_t *s, int time, qboolean snap ) {
	int		i;

	if ( ps->pm_type == PM_INTERMISSION || ps->pm_type == PM_SPECTATOR ) {
		s->eType = ET_INVISIBLE;
	} else if ( ps->stats[STAT_HEALTH] <= GIB_HEALTH ) {
		s->eType = ET_INVISIBLE;
	} else {
		s->eType = ET_PLAYER;
	}

	s->number = ps->clientNum;

	s->pos.trType = TR_LINEAR_STOP;
	VectorCopy( ps->origin, s->pos.trBase );
	if ( snap ) {
		SnapVector( s->pos.trBase );
	}
	// set the trDelta for flag direction and linear prediction
	VectorCopy( ps->velocity, s->pos.trDelta );
	// set the time for linear prediction
	s->pos.trTime = time;
	// set maximum extra polation time
	s->pos.trDuration = 50; // 1000 / sv_fps (default = 20)

	s->apos.trType = TR_INTERPOLATE;
	VectorCopy( ps->viewangles, s->apos.trBase );
	if ( snap ) {
		SnapVector( s->apos.trBase );
	}

	s->trickedentindex = ps->fd.forceMindtrickTargetIndex;
	s->trickedentindex2 = ps->fd.forceMindtrickTargetIndex2;
	s->trickedentindex3 = ps->fd.forceMindtrickTargetIndex3;
	s->trickedentindex4 = ps->fd.forceMindtrickTargetIndex4;

	s->forceFrame = ps->saberLockFrame;

	s->emplacedOwner = ps->electrifyTime;

	s->speed = ps->speed;

	s->genericenemyindex = ps->genericEnemyIndex;

	s->activeForcePass = ps->activeForcePass;

	s->angles2[YAW] = ps->movementDir;
	s->legsAnim = ps->legsAnim;
	s->torsoAnim = ps->torsoAnim;

	s->legsFlip = ps->legsFlip;
	s->torsoFlip = ps->torsoFlip;

	s->clientNum = ps->clientNum;		// ET_PLAYER looks here instead of at number
										// so corpses can also reference the proper config
	s->eFlags = ps->eFlags;
	s->eFlags2 = ps->eFlags2;

	s->saberInFlight = ps->saberInFlight;
	s->saberEntityNum = ps->saberEntityNum;
	s->saberMove = ps->saberMove;
	s->forcePowersActive = ps->fd.forcePowersActive;

	if (ps->duelInProgress)
	{
		s->bolt1 = 1;
	}
	else
	{
		s->bolt1 = 0;
	}

	s->otherEntityNum2 = ps->emplacedIndex;

	s->saberHolstered = ps->saberHolstered;

	if (ps->genericEnemyIndex != -1)
	{
		s->eFlags |= EF_SEEKERDRONE;
	}

	if ( ps->stats[STAT_HEALTH] <= 0 ) {
		s->eFlags |= EF_DEAD;
	} else {
		s->eFlags &= ~EF_DEAD;
	}

	if ( ps->externalEvent ) {
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
	}
	s->weapon = ps->weapon;
	s->groundEntityNum = ps->groundEntityNum;

	s->powerups = 0;
	for ( i = 0 ; i < MAX_POWERUPS ; i++ ) {
		if ( ps->powerups[ i ] ) {
			s->powerups |= 1 << i;
		}
	}

	s->loopSound = ps->loopSound;
	s->generic1 = ps->generic1;

	//NOT INCLUDED IN ENTITYSTATETOPLAYERSTATE:
	s->modelindex2 = ps->weaponstate;
	s->constantLight = ps->weaponChargeTime;

	VectorCopy(ps->lastHitLoc, s->origin2);

	s->isJediMaster = ps->isJediMaster;

	s->time2 = ps->holocronBits;

	s->fireflag = ps->fd.saberAnimLevel;

	s->heldByClient = ps->heldByClient;
	s->ragAttach = ps->ragAttach;

	s->iModelScale = ps->iModelScale;

	s->brokenLimbs = ps->brokenLimbs;

	s->hasLookTarget = ps->hasLookTarget;
	s->lookTarget = ps->lookTarget;

	s->customRGBA[0] = ps->customRGBA[0];
	s->customRGBA[1] = ps->customRGBA[1];
	s->customRGBA[2] = ps->customRGBA[2];
	s->customRGBA[3] = ps->customRGBA[3];

	s->m_iVehicleNum = ps->m_iVehicleNum;
}

/*
=================
CL_ReadDemoMessage
=================
*/
qboolean CL_ReadDemoMessage( fileHandle_t demofile, msg_t *msg ) {
	try {
		int			r;
		msg_t		&buf = *msg;
		byte		*bufData = buf.data;
		int			s;

		if ( !demofile ) {
			return qfalse;
		}

		// get the sequence number
		r = FS_Read( &s, 4, demofile);
		if ( r != 4 ) {
			return qfalse;
		}
		ctx->clc.serverMessageSequence = LittleLong( s );

		// init the message
		//MSG_Init( &buf, bufData, sizeof( bufData ) );
		// msg should be passed in initialized

		// get the length
		r = FS_Read (&buf.cursize, 4, demofile);
		if ( r != 4 ) {
			return qfalse;
		}
		buf.cursize = LittleLong( buf.cursize );
		if ( buf.cursize == -1 ) {
			return qfalse;
		}
		if ( buf.cursize > buf.maxsize ) {
			Com_Error (ERR_DROP, "CL_ReadDemoMessage: demoMsglen > MAX_MSGLEN");
		}
		r = FS_Read( buf.data, buf.cursize, demofile );
		if ( r != buf.cursize ) {
			Com_Printf( "Demo file was truncated.\n");
			buf.readcount = r;
			return qfalse;
		}

		ctx->clc.lastPacketTime = ctx->cls.realtime;
		buf.readcount = 0;
		//CL_ParseServerMessage( &buf );
		return qtrue;
	} catch ( int code ) {
		Com_Printf( "Demo terminated by code %d\n", code );
		return qfalse;
	}
}

float vectoyaw( const vec3_t vec ) {
	float	yaw;

	if (vec[YAW] == 0 && vec[PITCH] == 0) {
		yaw = 0;
	} else {
		if (vec[PITCH]) {
			yaw = ( atan2( vec[YAW], vec[PITCH]) * 180 / M_PI );
		} else if (vec[YAW] > 0) {
			yaw = 90;
		} else {
			yaw = 270;
		}
		if (yaw < 0) {
			yaw += 360;
		}
	}

	return yaw;
}

gitem_t	bg_itemlist[] =
{
	{
		NULL,				// classname
		NULL,				// pickup_sound
		{	NULL,			// world_model[0]
			NULL,			// world_model[1]
			0, 0} ,			// world_model[2],[3]
		NULL,				// view_model
/* icon */		NULL,		// icon
/* pickup */	//NULL,		// pickup_name
		0,					// quantity
		IT_BAD,				// giType (IT_*)
		0,					// giTag
/* precache */ "",			// precaches
/* sounds */ "",			// sounds
		""					// description
	},	// leave index 0 alone

	//
	// Pickups
	//

/*QUAKED item_shield_sm_instant (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Instant shield pickup, restores 25
*/
	{
		"item_shield_sm_instant",
		"sound/player/pickupshield.wav",
        { "models/map_objects/mp/psd_sm.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/small_shield",
/* pickup *///	"Shield Small",
		25,
		IT_ARMOR,
		1, //special for shield - max on pickup is maxhealth*tag, thus small shield goes up to 100 shield
/* precache */ "",
/* sounds */ ""
		""					// description
	},

/*QUAKED item_shield_lrg_instant (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Instant shield pickup, restores 100
*/
	{
		"item_shield_lrg_instant",
		"sound/player/pickupshield.wav",
        { "models/map_objects/mp/psd.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/large_shield",
/* pickup *///	"Shield Large",
		100,
		IT_ARMOR,
		2, //special for shield - max on pickup is maxhealth*tag, thus large shield goes up to 200 shield
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_medpak_instant (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Instant medpack pickup, heals 25
*/
	{
		"item_medpak_instant",
		"sound/player/pickuphealth.wav",
        { "models/map_objects/mp/medpac.md3",
		0, 0, 0 },
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_medkit",
/* pickup *///	"Medpack",
		25,
		IT_HEALTH,
		0,
/* precache */ "",
/* sounds */ "",
		""					// description
	},


	//
	// ITEMS
	//

/*QUAKED item_seeker (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
30 seconds of seeker drone
*/
	{
		"item_seeker",
		"sound/weapons/w_pkup.wav",
		{ "models/items/remote.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_seeker",
/* pickup *///	"Seeker Drone",
		120,
		IT_HOLDABLE,
		HI_SEEKER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_AN_ATTACK_DRONE_SIMILAR"					// description
	},

/*QUAKED item_shield (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Portable shield
*/
	{
		"item_shield",
		"sound/weapons/w_pkup.wav",
		{ "models/map_objects/mp/shield.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_shieldwall",
/* pickup *///	"Forcefield",
		120,
		IT_HOLDABLE,
		HI_SHIELD,
/* precache */ "",
/* sounds */ "sound/weapons/detpack/stick.wav sound/movers/doors/forcefield_on.wav sound/movers/doors/forcefield_off.wav sound/movers/doors/forcefield_lp.wav sound/effects/bumpfield.wav",
		"@MENUS_THIS_STATIONARY_ENERGY"					// description
	},

/*QUAKED item_medpac (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Bacta canister pickup, heals 25 on use
*/
	{
		"item_medpac",	//should be item_bacta
		"sound/weapons/w_pkup.wav",
		{ "models/map_objects/mp/bacta.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_bacta",
/* pickup *///	"Bacta Canister",
		25,
		IT_HOLDABLE,
		HI_MEDPAC,
/* precache */ "",
/* sounds */ "",
		"@SP_INGAME_BACTA_DESC"					// description
	},

/*QUAKED item_medpac_big (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Big bacta canister pickup, heals 50 on use
*/
	{
		"item_medpac_big",	//should be item_bacta
		"sound/weapons/w_pkup.wav",
		{ "models/items/big_bacta.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_big_bacta",
/* pickup *///	"Bacta Canister",
		25,
		IT_HOLDABLE,
		HI_MEDPAC_BIG,
/* precache */ "",
/* sounds */ "",
		"@SP_INGAME_BACTA_DESC"					// description
	},

/*QUAKED item_binoculars (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
These will be standard equipment on the player - DO NOT PLACE
*/
	{
		"item_binoculars",
		"sound/weapons/w_pkup.wav",
		{ "models/items/binoculars.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_zoom",
/* pickup *///	"Binoculars",
		60,
		IT_HOLDABLE,
		HI_BINOCULARS,
/* precache */ "",
/* sounds */ "",
		"@SP_INGAME_LA_GOGGLES_DESC"					// description
	},

/*QUAKED item_sentry_gun (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Sentry gun inventory pickup.
*/
	{
		"item_sentry_gun",
		"sound/weapons/w_pkup.wav",
		{ "models/items/psgun.glm",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_sentrygun",
/* pickup *///	"Sentry Gun",
		120,
		IT_HOLDABLE,
		HI_SENTRY_GUN,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THIS_DEADLY_WEAPON_IS"					// description
	},

/*QUAKED item_jetpack (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Do not place.
*/
	{
		"item_jetpack",
		"sound/weapons/w_pkup.wav",
		{ "models/items/psgun.glm", //FIXME: no model
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_jetpack",
/* pickup *///	"Sentry Gun",
		120,
		IT_HOLDABLE,
		HI_JETPACK,
/* precache */ "effects/boba/jet.efx",
/* sounds */ "sound/chars/boba/JETON.wav sound/chars/boba/JETHOVER.wav sound/effects/fire_lp.wav",
		"@MENUS_JETPACK_DESC"					// description
	},

/*QUAKED item_healthdisp (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Do not place. For siege classes ONLY.
*/
	{
		"item_healthdisp",
		"sound/weapons/w_pkup.wav",
		{ "models/map_objects/mp/bacta.md3", //replace me
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_healthdisp",
/* pickup *///	"Sentry Gun",
		120,
		IT_HOLDABLE,
		HI_HEALTHDISP,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_ammodisp (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Do not place. For siege classes ONLY.
*/
	{
		"item_ammodisp",
		"sound/weapons/w_pkup.wav",
		{ "models/map_objects/mp/bacta.md3", //replace me
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_ammodisp",
/* pickup *///	"Sentry Gun",
		120,
		IT_HOLDABLE,
		HI_AMMODISP,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_eweb_holdable (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
Do not place. For siege classes ONLY.
*/
	{
		"item_eweb_holdable",
		"sound/interface/shieldcon_empty",
		{ "models/map_objects/hoth/eweb_model.glm",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_eweb",
/* pickup *///	"Sentry Gun",
		120,
		IT_HOLDABLE,
		HI_EWEB,
/* precache */ "",
/* sounds */ "",
		"@MENUS_EWEB_DESC"					// description
	},

/*QUAKED item_seeker (.3 .3 1) (-8 -8 -0) (8 8 16) suspended
30 seconds of seeker drone
*/
	{
		"item_cloak",
		"sound/weapons/w_pkup.wav",
		{ "models/items/psgun.glm", //FIXME: no model
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_cloak",
/* pickup *///	"Seeker Drone",
		120,
		IT_HOLDABLE,
		HI_CLOAK,
/* precache */ "",
/* sounds */ "",
		"@MENUS_CLOAK_DESC"					// description
	},

/*QUAKED item_force_enlighten_light (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Adds one rank to all Force powers temporarily. Only light jedi can use.
*/
	{
		"item_force_enlighten_light",
		"sound/player/enlightenment.wav",
		{ "models/map_objects/mp/jedi_enlightenment.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/mpi_jlight",
/* pickup *///	"Light Force Enlightenment",
		25,
		IT_POWERUP,
		PW_FORCE_ENLIGHTENED_LIGHT,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_force_enlighten_dark (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Adds one rank to all Force powers temporarily. Only dark jedi can use.
*/
	{
		"item_force_enlighten_dark",
		"sound/player/enlightenment.wav",
		{ "models/map_objects/mp/dk_enlightenment.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/mpi_dklight",
/* pickup *///	"Dark Force Enlightenment",
		25,
		IT_POWERUP,
		PW_FORCE_ENLIGHTENED_DARK,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_force_boon (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Unlimited Force Pool for a short time.
*/
	{
		"item_force_boon",
		"sound/player/boon.wav",
		{ "models/map_objects/mp/force_boon.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/mpi_fboon",
/* pickup *///	"Force Boon",
		25,
		IT_POWERUP,
		PW_FORCE_BOON,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED item_ysalimari (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
A small lizard carried on the player, which prevents the possessor from using any Force power.  However, he is unaffected by any Force power.
*/
	{
		"item_ysalimari",
		"sound/player/ysalimari.wav",
		{ "models/map_objects/mp/ysalimari.md3",
		0, 0, 0} ,
/* view */		NULL,
/* icon */		"gfx/hud/mpi_ysamari",
/* pickup *///	"Ysalamiri",
		25,
		IT_POWERUP,
		PW_YSALAMIRI,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	//
	// WEAPONS
	//

/*QUAKED weapon_stun_baton (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		"weapon_stun_baton",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/stun_baton/baton_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/stun_baton/baton.md3",
/* icon */		"gfx/hud/w_icon_stunbaton",
/* pickup *///	"Stun Baton",
		100,
		IT_WEAPON,
		WP_STUN_BATON,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED weapon_melee (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		"weapon_melee",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/stun_baton/baton_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/stun_baton/baton.md3",
/* icon */		"gfx/hud/w_icon_melee",
/* pickup *///	"Stun Baton",
		100,
		IT_WEAPON,
		WP_MELEE,
/* precache */ "",
/* sounds */ "",
		"@MENUS_MELEE_DESC"					// description
	},

/*QUAKED weapon_saber (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		"weapon_saber",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/saber/saber_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/saber/saber_w.md3",
/* icon */		"gfx/hud/w_icon_lightsaber",
/* pickup *///	"Lightsaber",
		100,
		IT_WEAPON,
		WP_SABER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_AN_ELEGANT_WEAPON_FOR"				// description
	},

/*QUAKED weapon_bryar_pistol (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		//"weapon_bryar_pistol",
		"weapon_blaster_pistol",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/blaster_pistol/blaster_pistol_w.glm",//"models/weapons2/briar_pistol/briar_pistol_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/blaster_pistol/blaster_pistol.md3",//"models/weapons2/briar_pistol/briar_pistol.md3",
/* icon */		"gfx/hud/w_icon_blaster_pistol",//"gfx/hud/w_icon_rifle",
/* pickup *///	"Bryar Pistol",
		100,
		IT_WEAPON,
		WP_BRYAR_PISTOL,
/* precache */ "",
/* sounds */ "",
		"@MENUS_BLASTER_PISTOL_DESC"					// description
	},

/*QUAKED weapon_concussion_rifle (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_concussion_rifle",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/concussion/c_rifle_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/concussion/c_rifle.md3",
/* icon */		"gfx/hud/w_icon_c_rifle",//"gfx/hud/w_icon_rifle",
/* pickup *///	"Concussion Rifle",
		50,
		IT_WEAPON,
		WP_CONCUSSION,
/* precache */ "",
/* sounds */ "",
		"@MENUS_CONC_RIFLE_DESC"					// description
	},

/*QUAKED weapon_bryar_pistol_old (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		"weapon_bryar_pistol",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/briar_pistol/briar_pistol_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/briar_pistol/briar_pistol.md3",
/* icon */		"gfx/hud/w_icon_briar",//"gfx/hud/w_icon_rifle",
/* pickup *///	"Bryar Pistol",
		100,
		IT_WEAPON,
		WP_BRYAR_OLD,
/* precache */ "",
/* sounds */ "",
		"@SP_INGAME_BLASTER_PISTOL"					// description
	},

/*QUAKED weapon_blaster (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_blaster",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/blaster_r/blaster_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/blaster_r/blaster.md3",
/* icon */		"gfx/hud/w_icon_blaster",
/* pickup *///	"E11 Blaster Rifle",
		100,
		IT_WEAPON,
		WP_BLASTER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THE_PRIMARY_WEAPON_OF"				// description
	},

/*QUAKED weapon_disruptor (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_disruptor",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/disruptor/disruptor_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/disruptor/disruptor.md3",
/* icon */		"gfx/hud/w_icon_disruptor",
/* pickup *///	"Tenloss Disruptor Rifle",
		100,
		IT_WEAPON,
		WP_DISRUPTOR,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THIS_NEFARIOUS_WEAPON"					// description
	},

/*QUAKED weapon_bowcaster (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_bowcaster",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/bowcaster/bowcaster_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/bowcaster/bowcaster.md3",
/* icon */		"gfx/hud/w_icon_bowcaster",
/* pickup *///	"Wookiee Bowcaster",
		100,
		IT_WEAPON,
		WP_BOWCASTER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THIS_ARCHAIC_LOOKING"					// description
	},

/*QUAKED weapon_repeater (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_repeater",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/heavy_repeater/heavy_repeater_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/heavy_repeater/heavy_repeater.md3",
/* icon */		"gfx/hud/w_icon_repeater",
/* pickup *///	"Imperial Heavy Repeater",
		100,
		IT_WEAPON,
		WP_REPEATER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THIS_DESTRUCTIVE_PROJECTILE"					// description
	},

/*QUAKED weapon_demp2 (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
NOTENOTE This weapon is not yet complete.  Don't place it.
*/
	{
		"weapon_demp2",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/demp2/demp2_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/demp2/demp2.md3",
/* icon */		"gfx/hud/w_icon_demp2",
/* pickup *///	"DEMP2",
		100,
		IT_WEAPON,
		WP_DEMP2,
/* precache */ "",
/* sounds */ "",
		"@MENUS_COMMONLY_REFERRED_TO"					// description
	},

/*QUAKED weapon_flechette (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_flechette",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/golan_arms/golan_arms_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/golan_arms/golan_arms.md3",
/* icon */		"gfx/hud/w_icon_flechette",
/* pickup *///	"Golan Arms Flechette",
		100,
		IT_WEAPON,
		WP_FLECHETTE,
/* precache */ "",
/* sounds */ "",
		"@MENUS_WIDELY_USED_BY_THE_CORPORATE"					// description
	},

/*QUAKED weapon_rocket_launcher (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_rocket_launcher",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/merr_sonn/merr_sonn_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/merr_sonn/merr_sonn.md3",
/* icon */		"gfx/hud/w_icon_merrsonn",
/* pickup *///	"Merr-Sonn Missile System",
		3,
		IT_WEAPON,
		WP_ROCKET_LAUNCHER,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THE_PLX_2M_IS_AN_EXTREMELY"					// description
	},

/*QUAKED ammo_thermal (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"ammo_thermal",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/thermal/thermal_pu.md3",
		"models/weapons2/thermal/thermal_w.glm", 0, 0},
/* view */		"models/weapons2/thermal/thermal.md3",
/* icon */		"gfx/hud/w_icon_thermal",
/* pickup *///	"Thermal Detonators",
		4,
		IT_AMMO,
		AMMO_THERMAL,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THE_THERMAL_DETONATOR"					// description
	},

/*QUAKED ammo_tripmine (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"ammo_tripmine",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/laser_trap/laser_trap_pu.md3",
		"models/weapons2/laser_trap/laser_trap_w.glm", 0, 0},
/* view */		"models/weapons2/laser_trap/laser_trap.md3",
/* icon */		"gfx/hud/w_icon_tripmine",
/* pickup *///	"Trip Mines",
		3,
		IT_AMMO,
		AMMO_TRIPMINE,
/* precache */ "",
/* sounds */ "",
		"@MENUS_TRIP_MINES_CONSIST_OF"					// description
	},

/*QUAKED ammo_detpack (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"ammo_detpack",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/detpack/det_pack_pu.md3", "models/weapons2/detpack/det_pack_proj.glm", "models/weapons2/detpack/det_pack_w.glm", 0},
/* view */		"models/weapons2/detpack/det_pack.md3",
/* icon */		"gfx/hud/w_icon_detpack",
/* pickup *///	"Det Packs",
		3,
		IT_AMMO,
		AMMO_DETPACK,
/* precache */ "",
/* sounds */ "",
		"@MENUS_A_DETONATION_PACK_IS"					// description
	},

/*QUAKED weapon_thermal (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_thermal",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/thermal/thermal_w.glm", "models/weapons2/thermal/thermal_pu.md3",
		0, 0 },
/* view */		"models/weapons2/thermal/thermal.md3",
/* icon */		"gfx/hud/w_icon_thermal",
/* pickup *///	"Thermal Detonator",
		4,
		IT_WEAPON,
		WP_THERMAL,
/* precache */ "",
/* sounds */ "",
		"@MENUS_THE_THERMAL_DETONATOR"					// description
	},

/*QUAKED weapon_trip_mine (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_trip_mine",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/laser_trap/laser_trap_w.glm", "models/weapons2/laser_trap/laser_trap_pu.md3",
		0, 0},
/* view */		"models/weapons2/laser_trap/laser_trap.md3",
/* icon */		"gfx/hud/w_icon_tripmine",
/* pickup *///	"Trip Mine",
		3,
		IT_WEAPON,
		WP_TRIP_MINE,
/* precache */ "",
/* sounds */ "",
		"@MENUS_TRIP_MINES_CONSIST_OF"					// description
	},

/*QUAKED weapon_det_pack (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_det_pack",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/detpack/det_pack_proj.glm", "models/weapons2/detpack/det_pack_pu.md3", "models/weapons2/detpack/det_pack_w.glm", 0},
/* view */		"models/weapons2/detpack/det_pack.md3",
/* icon */		"gfx/hud/w_icon_detpack",
/* pickup *///	"Det Pack",
		3,
		IT_WEAPON,
		WP_DET_PACK,
/* precache */ "",
/* sounds */ "",
		"@MENUS_A_DETONATION_PACK_IS"					// description
	},

/*QUAKED weapon_emplaced (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
*/
	{
		"weapon_emplaced",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/blaster_r/blaster_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/blaster_r/blaster.md3",
/* icon */		"gfx/hud/w_icon_blaster",
/* pickup *///	"Emplaced Gun",
		50,
		IT_WEAPON,
		WP_EMPLACED_GUN,
/* precache */ "",
/* sounds */ "",
		""					// description
	},


//NOTE: This is to keep things from messing up because the turret weapon type isn't real
	{
		"weapon_turretwp",
		"sound/weapons/w_pkup.wav",
        { "models/weapons2/blaster_r/blaster_w.glm",
		0, 0, 0},
/* view */		"models/weapons2/blaster_r/blaster.md3",
/* icon */		"gfx/hud/w_icon_blaster",
/* pickup *///	"Turret Gun",
		50,
		IT_WEAPON,
		WP_TURRET,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	//
	// AMMO ITEMS
	//

/*QUAKED ammo_force (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Don't place this
*/
	{
		"ammo_force",
		"sound/player/pickupenergy.wav",
        { "models/items/energy_cell.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/hud/w_icon_blaster",
/* pickup *///	"Force??",
		100,
		IT_AMMO,
		AMMO_FORCE,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED ammo_blaster (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Ammo for the Bryar and Blaster pistols.
*/
	{
		"ammo_blaster",
		"sound/player/pickupenergy.wav",
        { "models/items/energy_cell.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/hud/i_icon_battery",
/* pickup *///	"Blaster Pack",
		100,
		IT_AMMO,
		AMMO_BLASTER,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED ammo_powercell (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Ammo for Tenloss Disruptor, Wookie Bowcaster, and the Destructive Electro Magnetic Pulse (demp2 ) guns
*/
	{
		"ammo_powercell",
		"sound/player/pickupenergy.wav",
        { "models/items/power_cell.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/ammo_power_cell",
/* pickup *///	"Power Cell",
		100,
		IT_AMMO,
		AMMO_POWERCELL,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED ammo_metallic_bolts (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Ammo for Imperial Heavy Repeater and the Golan Arms Flechette
*/
	{
		"ammo_metallic_bolts",
		"sound/player/pickupenergy.wav",
        { "models/items/metallic_bolts.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/ammo_metallic_bolts",
/* pickup *///	"Metallic Bolts",
		100,
		IT_AMMO,
		AMMO_METAL_BOLTS,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED ammo_rockets (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
Ammo for Merr-Sonn portable missile launcher
*/
	{
		"ammo_rockets",
		"sound/player/pickupenergy.wav",
        { "models/items/rockets.md3",
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/ammo_rockets",
/* pickup *///	"Rockets",
		3,
		IT_AMMO,
		AMMO_ROCKETS,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED ammo_all (.3 .3 1) (-16 -16 -16) (16 16 16) suspended
DO NOT PLACE in a map, this is only for siege classes that have ammo
dispensing ability
*/
	{
		"ammo_all",
		"sound/player/pickupenergy.wav",
        { "models/items/battery.md3",  //replace me
		0, 0, 0},
/* view */		NULL,
/* icon */		"gfx/mp/ammo_rockets", //replace me
/* pickup *///	"Rockets",
		0,
		IT_AMMO,
		-1,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	//
	// POWERUP ITEMS
	//
/*QUAKED team_CTF_redflag (1 0 0) (-16 -16 -16) (16 16 16)
Only in CTF games
*/
	{
		"team_CTF_redflag",
		NULL,
        { "models/flags/r_flag.md3",
		"models/flags/r_flag_ysal.md3", 0, 0 },
/* view */		NULL,
/* icon */		"gfx/hud/mpi_rflag",
/* pickup *///	"Red Flag",
		0,
		IT_TEAM,
		PW_REDFLAG,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

/*QUAKED team_CTF_blueflag (0 0 1) (-16 -16 -16) (16 16 16)
Only in CTF games
*/
	{
		"team_CTF_blueflag",
		NULL,
        { "models/flags/b_flag.md3",
		"models/flags/b_flag_ysal.md3", 0, 0 },
/* view */		NULL,
/* icon */		"gfx/hud/mpi_bflag",
/* pickup *///	"Blue Flag",
		0,
		IT_TEAM,
		PW_BLUEFLAG,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	//
	// PERSISTANT POWERUP ITEMS
	//

	/*QUAKED team_CTF_neutralflag (0 0 1) (-16 -16 -16) (16 16 16)
Only in One Flag CTF games
*/
	{
		"team_CTF_neutralflag",
		NULL,
        { "models/flags/n_flag.md3",
		0, 0, 0 },
/* view */		NULL,
/* icon */		"icons/iconf_neutral1",
/* pickup *///	"Neutral Flag",
		0,
		IT_TEAM,
		PW_NEUTRALFLAG,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	{
		"item_redcube",
		"sound/player/pickupenergy.wav",
        { "models/powerups/orb/r_orb.md3",
		0, 0, 0 },
/* view */		NULL,
/* icon */		"icons/iconh_rorb",
/* pickup *///	"Red Cube",
		0,
		IT_TEAM,
		0,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	{
		"item_bluecube",
		"sound/player/pickupenergy.wav",
        { "models/powerups/orb/b_orb.md3",
		0, 0, 0 },
/* view */		NULL,
/* icon */		"icons/iconh_borb",
/* pickup *///	"Blue Cube",
		0,
		IT_TEAM,
		0,
/* precache */ "",
/* sounds */ "",
		""					// description
	},

	// end of list marker
	{NULL}
};

int		bg_numItems = sizeof(bg_itemlist) / sizeof(bg_itemlist[0]) - 1;

/*
==============
BG_FindItemForHoldable
==============
*/
gitem_t	*BG_FindItemForHoldable( holdable_t pw ) {
	int		i;

	for ( i = 0 ; i < bg_numItems ; i++ ) {
		if ( bg_itemlist[i].giType == IT_HOLDABLE && bg_itemlist[i].giTag == pw ) {
			return &bg_itemlist[i];
		}
	}

	Com_Error( ERR_DROP, "HoldableItem not found" );

	return NULL;
}
