#pragma once

#include "client/client.h"

typedef void (*demoOutputHook_t)( const void *data, size_t len, void *userData );
void SetDemoOutputHook( demoOutputHook_t hook, void *userData );
void DemoOutputWrite( const void *data, size_t len, FILE *fp );

void writeDemoHeader( FILE *fp );
void writeDemoHeaderWithServerCommands( FILE* fp, int reliableAcknowledge, int serverCommandSequence, int serverCommandOffset );
void CL_WriteDemoMessage ( msg_t *msg, int headerBytes, FILE *fp );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta );
void writeDeltaSnapshot( int firstServerCommand, FILE *fp, qboolean forceNonDelta, int serverCommandOffset );