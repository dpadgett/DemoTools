#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "client/client.h"

/*
=================
CL_ConnectionlessPacket

Responses to broadcasts, etc
=================
*/
void CL_ConnectionlessPacket( netadr_t from, msg_t *msg ) {
	char	*s;
	char	*c;
	int challenge = 0;

	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );	// skip the -1

	s = MSG_ReadStringLine( msg );

	Cmd_TokenizeString( s );

	c = Cmd_Argv(0);

	Com_DPrintf ("CL packet %s: %s\n", NET_AdrToString(from), c);

	// challenge from the server we are connecting to
	if ( !Q_stricmp(c, "challengeResponse") )
	{
		if ( cls.state != CA_CONNECTING )
		{
			Com_Printf( "Unwanted challenge response received.  Ignored.\n" );
			return;
		}

		c = Cmd_Argv(2);
		if(*c)
			challenge = atoi(c);

		if(!NET_CompareAdr(from, clc.serverAddress))
		{
			// This challenge response is not coming from the expected address.
			// Check whether we have a matching client challenge to prevent
			// connection hi-jacking.

			if(!*c || challenge != clc.challenge)
			{
				Com_DPrintf("Challenge response received from unexpected source. Ignored.\n");
				return;
			}
		}

		// start sending challenge response instead of challenge request packets
		clc.challenge = atoi(Cmd_Argv(1));
		cls.state = CA_CHALLENGING;
		clc.connectPacketCount = 0;
		clc.connectTime = -99999;

		// take this address as the new server address.  This allows
		// a server proxy to hand off connections to multiple servers
		clc.serverAddress = from;
		Com_DPrintf ("challengeResponse: %d\n", clc.challenge);
		return;
	}

	// server connection
	if ( !Q_stricmp(c, "connectResponse") ) {
		if ( cls.state >= CA_CONNECTED ) {
			Com_Printf ("Dup connect received. Ignored.\n");
			return;
		}
		if ( cls.state != CA_CHALLENGING ) {
			Com_Printf ("connectResponse packet while not connecting. Ignored.\n");
			return;
		}
		if ( !NET_CompareAdr( from, clc.serverAddress ) ) {
			Com_Printf( "connectResponse from wrong address. Ignored.\n" );
			return;
		}
		Netchan_Setup (NS_CLIENT, &clc.netchan, from, Cvar_VariableValue( "net_qport" ) );
		cls.state = CA_CONNECTED;
		clc.lastPacketSentTime = -9999;		// send first packet immediately
		return;
	}

	// a disconnect message from the server, which will happen if the server
	// dropped the connection but it is still getting packets from us
	if (!Q_stricmp(c, "disconnect")) {
		//CL_DisconnectPacket( from );
		return;
	}

	Com_DPrintf ("Unknown connectionless packet command.\n");
}

/*
=================
CL_CheckForResend

Resend a connect message if the last one has timed out
=================
*/
void CL_CheckForResend( void ) {
	int		port;
	char	info[MAX_INFO_STRING];
	char	data[MAX_INFO_STRING];

	// don't send anything if playing back a demo
	if ( clc.demoplaying ) {
		return;
	}

	// resend if we haven't gotten a reply yet
	if ( cls.state != CA_CONNECTING && cls.state != CA_CHALLENGING ) {
		return;
	}

	if ( cls.realtime - clc.connectTime < RETRANSMIT_TIMEOUT ) {
		return;
	}

	clc.connectTime = cls.realtime;	// for retransmit requests
	clc.connectPacketCount++;


	switch ( cls.state ) {
	case CA_CONNECTING:
		// requesting a challenge

		// The challenge request shall be followed by a client challenge so no malicious server can hijack this connection.
		Com_sprintf(data, sizeof(data), "getchallenge %d", clc.challenge);

		NET_OutOfBandPrint(NS_CLIENT, clc.serverAddress, data);
		break;

	case CA_CHALLENGING:
		// sending back the challenge
		port = (int) Cvar_VariableValue ("net_qport");

		//Q_strncpyz( info, Cvar_InfoString( CVAR_USERINFO ), sizeof( info ) );
		*info = 0;
		Info_SetValueForKey( info, "protocol", va("%i", PROTOCOL_VERSION ) );
		Info_SetValueForKey( info, "qport", va("%i", port ) );
		Info_SetValueForKey( info, "challenge", va("%i", clc.challenge ) );

		Com_sprintf(data, sizeof(data), "connect \"%s\"", info );
		NET_OutOfBandData( NS_CLIENT, clc.serverAddress, (byte *)data, strlen(data) );

		// the most current userinfo has been sent, so watch for any
		// newer changes to userinfo variables
		//cvar_modifiedFlags &= ~CVAR_USERINFO;
		break;

	default:
		Com_Error( ERR_FATAL, "CL_CheckForResend: bad cls.state" );
	}
}

/*
==================
Com_RandomBytes

fills string array with len radom bytes, peferably from the OS randomizer
==================
*/
void Com_RandomBytes( byte *string, int len )
{
	int i;

	//if( Sys_RandomBytes( string, len ) )
	//	return;

	Com_Printf( "Com_RandomBytes: using weak randomization\n" );
	for( i = 0; i < len; i++ )
		string[i] = (unsigned char)( rand() % 255 );
}

/*
================
CL_Connect_f

================
*/
void CL_Connect_f( void ) {
	char	*server;
	const char	*serverString;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: connect [server]\n");
		return;
	}

	// save arguments for reconnect
	//Q_strncpyz( cl_reconnectArgs, Cmd_Args(), sizeof( cl_reconnectArgs ) );

	//Cvar_Set("ui_singlePlayerActive", "0");

	// fire a message off to the motd server
	//CL_RequestMotd();

	// clear any previous "server full" type messages
	clc.serverMessage[0] = 0;

	server = Cmd_Argv (1);

	//if ( com_sv_running->integer && !strcmp( server, "localhost" ) ) {
	//	// if running a local server, kill it
	//	SV_Shutdown( "Server quit\n" );
	//}

	// make sure a local server is killed
	//Cvar_Set( "sv_killserver", "1" );
	//SV_Frame( 0 );

	//CL_Disconnect( qtrue );
	//Con_Close();

	Q_strncpyz( cls.servername, server, sizeof(cls.servername) );

	if (!NET_StringToAdr( cls.servername, &clc.serverAddress) ) {
		Com_Printf ("Bad server address\n");
		cls.state = CA_DISCONNECTED;
		return;
	}
	if (clc.serverAddress.port == 0) {
		clc.serverAddress.port = BigShort( PORT_SERVER );
	}

	serverString = NET_AdrToString(clc.serverAddress);

	Com_Printf( "%s resolved to %s\n", cls.servername, serverString );

	//if( cl_guidServerUniq->integer )
	//	CL_UpdateGUID( serverString, strlen( serverString ) );
	//else
	//	CL_UpdateGUID( NULL, 0 );

	// if we aren't playing on a lan, we need to authenticate
	if ( NET_IsLocalAddress( clc.serverAddress ) ) {
		cls.state = CA_CHALLENGING;
	} else {
		cls.state = CA_CONNECTING;

		// Set a client challenge number that ideally is mirrored back by the server.
		clc.challenge = ((rand() << 16) ^ rand()) ^ Com_Milliseconds();
	}

	//Key_SetCatcher( 0 );
	clc.connectTime = -99999;	// CL_CheckForResend() will fire immediately
	clc.connectPacketCount = 0;

	// server connection string
	//Cvar_Set( "cl_currentServerAddress", server );
	//Cvar_Set( "cl_currentServerIP", serverString );
}
