#pragma once

#include "client/client.h"

void writeDemoHeader( FILE *fp );
void CL_WriteDemoMessage ( msg_t *msg, int headerBytes, FILE *fp );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta, int serverCommandOffset );