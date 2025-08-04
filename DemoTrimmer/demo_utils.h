#pragma once

#include "client/client.h"

void writeDemoHeader( FILE *fp );
void writeDemoHeaderWithServerCommands( FILE* fp, int reliableAcknowledge, int serverCommandSequence, int serverCommandOffset );
void CL_WriteDemoMessage ( msg_t *msg, int headerBytes, FILE *fp );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta, int serverCommandOffset );