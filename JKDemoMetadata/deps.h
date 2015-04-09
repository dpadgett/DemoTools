#ifndef DEPS_H
#define DEPS_H

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

void CL_ConfigstringModified( void );
qboolean CL_ReadDemoMessage( fileHandle_t demofile, msg_t *msg );
long QDECL FS_ReadCount( fileHandle_t fileHandle );

#endif
