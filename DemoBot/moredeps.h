#ifndef MOREDEPS_H
#define MOREDEPS_H

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

void CL_CheckForResend( void );
void CL_ConnectionlessPacket( netadr_t from, msg_t *msg );
void CL_Connect_f( void );


#endif
