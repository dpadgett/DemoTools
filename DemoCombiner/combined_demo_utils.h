#pragma once

#include "client/client.h"

void writeMergedDemoHeader( FILE* fp );
void writeMergedGamestateData( FILE* fp );
void writeTruncatedMessage( FILE* fp, int demoIdx, int serverMessageSequence, msg_t* truncatedMsg );
//void CL_WriteDemoMessage( msg_t* msg, int headerBytes, FILE* fp );
void writeMergedDeltaSnapshot( int firstServerCommand, FILE* fp, qboolean forceNonDelta );
void writeMergedDeltaSnapshot( int firstServerCommand, FILE* fp, qboolean forceNonDelta, int serverCommandOffset );