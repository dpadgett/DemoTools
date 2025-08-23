#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

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

void MSG_ReadDeltaPlayerstateWithForcedFields( msg_t* msg, playerState_t* from, playerState_t* to, playerState_t* forcedFields, qboolean isVehiclePS );
