#include <stulibc.h>
#include "common.h"
#include "server_interface.h"
#include "server.h"





int main( int argc, char **argv )
{
    LIB_Init();
    PRINT("Server starting...");

    struct Argument* portNumber = 		CMD_CreateNewArgument("p","p <number>","Set the port that the server will listen on", true, true, setPortNumber);
    struct Argument* brokerAddressCMD = CMD_CreateNewArgument("ba","ba <address>","Set the address of the broker", true, true, setBrokerAddress);
    struct Argument* ourAddressCMD = 	CMD_CreateNewArgument("oa","oa <address>","Set the address we listen on (broker uses to deliver responses)", true, true, setOurAddress);
    struct Argument* brokerPortCMD = 	CMD_CreateNewArgument("bp","bp <port>","Set the port of the broker", true, true, setBrokerPort);
    struct Argument* waitIndefCMD = 	CMD_CreateNewArgument("w","","wait indefinitely for connections", false, false, setWaitIndef);
    struct Argument* beVerboseCMD = 	CMD_CreateNewArgument("v","","be verbose wtih messages", false, false, setBeVerbose);

    CMD_AddArgument(portNumber);
    CMD_AddArgument(waitIndefCMD);
    CMD_AddArgument(brokerAddressCMD);
    CMD_AddArgument(brokerPortCMD);
    CMD_AddArgument(beVerboseCMD);
    CMD_AddArgument(ourAddressCMD);

    struct sockaddr_in local;
    struct sockaddr_in peer;
    char *hname;
    char *sname;
    int peerlen;
    SOCKET s1;
    SOCKET s;
    fd_set readfds;
    FD_ZERO( &readfds);
    const int on = 1;
    List* settings = (void*)null;
    struct timeval timeout = {.tv_sec = 60, .tv_usec = 0};

    if (FILE_Exists(CONFIG_FILENAME) && !(argc > 1)) {
		DBG("Using config file located in '%s'", CONFIG_FILENAME);
		settings = LIST_GetInstance();
		if(INI_IniParse(CONFIG_FILENAME, settings) == 0) { // if successful parse
			// Get general config options
			setWaitIndef(INI_GetSetting(settings, "options", "wait"));
			setBeVerbose(INI_GetSetting(settings, "options", "verbose"));
			// Get networking configuration options
			setPortNumber(INI_GetSetting(settings, "networking", "port"));
			setOurAddress(INI_GetSetting(settings, "networking", "address"));
			// Get broker's connection details
			setBrokerAddress(INI_GetSetting(settings, "broker", "address"));
			setBrokerPort(INI_GetSetting(settings, "broker", "port"));

			PrintConfigDiagnostics(serverConfig.verbose, settings);
		}
		else {
			ERR_Print("Failed to parse config file", 1);
		}
	} else if(argc > 1) {
        enum ParseResult result = CMD_Parse(argc, argv, true);
        if(result != PARSE_SUCCESS) {
            PRINT("There was a problem parsing: %d \n", result);
            return 1;
        }
    } else {
        CMD_ShowUsages("server", "stumathews@gmail.com", "the server component");
        exit(0);
    }

    NETINIT();
    
    if(serverConfig.verbose )
        PRINT("Server listening...\n");

    // Register with the broker on startup.
    PRINT("broker address is %s, broker port is %s \n", brokerDetails.address, brokerDetails.port);
    if(service_register_with_broker(brokerDetails, serverDetails, serverConfig)){
    	PRINT("Sending registration request to broker at address '%s:%s'", brokerDetails.address, brokerDetails.port);
        registered_with_broker = true;
    }

    // get a socket, bound to this address thats configured to listen.
    // NB: This is always ever non-blocking 
    s = netTcpServer(serverDetails.address,serverDetails.port);
    FD_SET(s, &readfds);
    do {
        // wait/block on this listening socket...
        int res = 0;
        if(!serverConfig.waitIndef) {
            res = select(s+1, &readfds, NULL, NULL, &timeout);
        } else {
            res = select(s+1, &readfds, NULL, NULL, NULL);
        }

        if(res == 0) {
            LOG("timeout");
            netError(1,errno,"timeout!");
        }
        else if(res == -1) {
            LOG("Select error!");
            netError(1,errno,"select error!!");
        } else {
            peerlen = sizeof(peer);
            if(FD_ISSET(s,&readfds)) {
				// Fork of a new thread to deal with this request and go back to listening for next request
				THREAD_RunAndForget(thread_server, (void*)&s);
            } else {
                DBG("not our socket. continuing");
                continue;
            }
        }
    } while (1);

    LIST_FreeInstance(settings);
    LIB_Uninit();
#ifdef __linux__
	pthread_exit(NULL);
#endif
    EXIT( 0 );
}

/***
 * Wrapper function to accept connection and process it - so that it confirms to void* func(void*) prototype so can pass as a thread function
 *
 * @param params SOCKET* socket that is ready to read from
 */

void CheckValidSocket(SOCKET s1) {
	if (!isvalidsock(s1)) {
		netError(1, errno, "accept failed");
	}
}

#ifdef __linux__
void* thread_server(void* params)
#else
unsigned __stdcall thread_server(void* params)
#endif
{
	int peerlen;
	struct sockaddr_in peer;
	peerlen = sizeof(peer);
	SOCKET* s = (SOCKET*) params;
	SOCKET s1 = accept(*s,(struct sockaddr *)&peer, &peerlen);
	CheckValidSocket(s1);
	ReadAndProcessDataOnSocket(s1, &peer);
	NETCLOSE( s1 );
#ifdef __linux__
	return (void*)0;
#else
	return 0;
#endif
}

static void ReadAndProcessDataOnSocket(SOCKET s, struct sockaddr_in *peerp )
{
    Packet pkt;
    int n_rc = netReadn( s,(char*) &pkt.len, sizeof(uint32_t));
    pkt.len = ntohl(pkt.len);
    char* dbuf = (char*) malloc( sizeof(char) * pkt.len);
    int d_rc  = netReadn(s, dbuf, sizeof( char) * pkt.len);

    if(serverConfig.verbose) {
        PRINT("received %d bytes and interpreted it as length of %u\n", n_rc,pkt.len );
    }
    if(n_rc < 1) {
        netError(1, errno,"failed to receiver packet size\n");
    }
    if(d_rc < 1) {
        netError(1, errno,"failed to receive message\n");
    }
    if(serverConfig.verbose) {
        PRINT("read %d bytes of data\n",d_rc);
    }


    unpack_marshal_call_send(dbuf, pkt.len, brokerDetails, serverConfig );
}
// ===============================
// Command line handling routines
// ===============================
static void setBrokerPort( char* arg)
{
    CHECK_STRING(arg, IS_NOT_EMPTY);
    strncpy(brokerDetails.port, arg, strlen(arg));
}

static void setPortNumber(char* arg)
{
	CHECK_STRING(arg, IS_NOT_EMPTY);
	strncpy(serverDetails.port, arg, strlen(arg));
}

static void setBrokerAddress(char* arg)
{
    CHECK_STRING(arg, IS_NOT_EMPTY );
    strncpy(brokerDetails.address, arg, strlen(arg) );
}
static void setOurAddress(char* arg)
{
    CHECK_STRING(arg, IS_NOT_EMPTY );
    strncpy(serverDetails.address, arg, strlen(arg) );
}

static void setWaitIndef(char* arg)
{
    serverConfig.waitIndef = true;
}

static void setBeVerbose(char* arg)
{
    if(STR_Equals(arg,"true") || STR_Equals(arg,"1")) {
    	serverConfig.verbose = true;
    } else {
    	serverConfig.verbose = false;
    }
}

