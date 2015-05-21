#include <stulibc.h>
#include "common.h"

struct ServiceRegistration service_repository;
char *program_name;
static char port[20] = {0};
static bool verbose = false;
static bool waitIndef = false;

void update_repository();
void register_service(struct ServiceRegistration* service_registration);
void UnpackServiceRegistrationBuffer(char* buffer,int buflen, struct ServiceRegistration* unpacked);
void acknowledgement();
void find_server(char* buffer, int len, Destination *dest);
void find_client(char* buffer, int len, Destination *dest);
void forward_request(char* buffer, int len);
void forward_response();

static void server( SOCKET s, struct sockaddr_in *peerp )
{
    // 1. Read the size of packet.
    // 2. Read the packet data
    // 3. Do stuff based on the packet data

    struct packet pkt;

    int n_rc = netReadn( s,(char*) &pkt.len, sizeof(uint32_t));
    pkt.len = ntohl(pkt.len);

    if( n_rc < 1 )
        netError(1, errno,"Failed to receiver packet size\n");
    
    if(verbose) 
        PRINT("Received %d bytes and interpreted it as length of %u\n", n_rc,pkt.len );
    
    pkt.buffer = (char*) malloc( sizeof(char) * pkt.len);
    int d_rc  = netReadn( s, pkt.buffer, sizeof( char) * pkt.len);

    if( d_rc < 1 )
        netError(1, errno,"failed to receive message\n");
    
    if(verbose)
    {
        PRINT("Read %d bytes of data.\n",d_rc);
    }

    int request_type = -1;
   
    // What is this data we got?
    if( (request_type = determine_request_type(&pkt)) == REQUEST_SERVICE )
    {
        PRINT("Incomming Service Request.\n");

        forward_request(pkt.buffer, pkt.len);
    }
    else if ( request_type == REQUEST_REGISTRATION )
    {
        PRINT("Incomming Registration Request.\n");

        struct ServiceRegistration *sr_buf = Alloc( sizeof( struct ServiceRegistration ));
        
        UnpackServiceRegistrationBuffer(pkt.buffer, pkt.len,sr_buf); 
        register_service(sr_buf);
        

    }
    else 
    {
        PRINT("Unrecongnised request type:%d \n", request_type);    
        exit(1);
    }

    Destination *dest = Alloc( sizeof( Destination ));
    find_client(pkt.buffer, pkt.len, dest); // the socket is already connected to the client if this is synchrnous
    forward_response();
}

static void setPortNumber(char* arg)
{
    CHECK_STRING(arg, IS_NOT_EMPTY);
    strncpy( port, arg, strlen(arg));
}

static void setVerbose(char* arg)
{
    verbose = true;
}

static void setWaitIndefinitely(char* arg)
{
    waitIndef = true;
}

// listens for broker connections and dispatches them to the server() method
void main_event_loop()
{
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
    struct timeval timeout = {.tv_sec = 60, .tv_usec=0}; 
    // get a socket, bound to this address thats configured to listen.
    // NB: This is always ever non-blocking 
    s = netTcpServer("localhost",port);

    FD_SET(s, &readfds);

    do
    {
        if(verbose) PRINT("Listening.\n");
        // wait/block on this listening socket...
        int res = 0;
       if( waitIndef )
          res =  select( s+1, &readfds, NULL, NULL, NULL);//&timeout);
       else
          res =  select( s+1, &readfds, NULL, NULL, &timeout);


        if( res == 0 )
        {
            LOG( "timeout");
            netError(1,errno,"timeout!");
        }
        else if( res == -1 )
        {
            LOG("Select error!");
            netError(1,errno,"select error!!");
        }
        else
        {
            peerlen = sizeof( peer );

            if( FD_ISSET(s,&readfds ))
            {
                s1 = accept( s, ( struct sockaddr * )&peer, &peerlen );

                if ( !isvalidsock( s1 ) )
                    netError( 1, errno, "accept failed" );

                // do network functionality on this socket that now represents a connection with the peer (client) 
                server( s1, &peer );
                CLOSE( s1 );
            }
            else
            {
                DBG("Not our socket. continuing");
                continue;
            }
        }
    } while ( 1 );

}

void update_repository()
{
    
}
void print_service_repository()
{
    if(verbose)
        printf("Service registrations:\n");

    struct list_head *pos, *q;
    struct ServiceRegistration* tmp = Alloc( sizeof( struct ServiceRegistration ));
    int count = 0;

    list_for_each( pos, &service_repository.list)
    {
        tmp = list_entry( pos, struct ServiceRegistration, list );
        if( tmp == NULL )
        {
            PRINT("Null service!\n");
            return;
        }
        if( verbose )
            PRINT("In list_for_each\n");
        ServiceReg *unpacked = tmp;
        PRINT("Service Registration:\nService name:%s\nAddress: %s\nPort: %s\nNumber ofservices %d",unpacked->service_name,unpacked->address, unpacked->port,unpacked->num_services);
    }
   // free(tmp);
}

void UnpackServiceRegistrationBuffer(char* buffer, int buflen, struct ServiceRegistration* unpacked)
{
    // unpack service registration request
    
    if( verbose)
        PRINT("unpack service registration request\n");

    unpacked->num_services = 0;

    msgpack_unpacked result;
    msgpack_unpack_return ret;
    size_t off = 0;
    int i = 0;
    msgpack_unpacked_init(&result);

    ret = msgpack_unpack_next(&result, buffer, buflen, &off);

    while (ret == MSGPACK_UNPACK_SUCCESS) 
    {
        msgpack_object obj = result.data;
        
        char header_name[20];
        memset(header_name, '\0', 20);
        
        msgpack_object val = extract_header( &obj, header_name);
        //Protocolheaders headers; // will store all the protocol's headers

        if( val.type == MSGPACK_OBJECT_STR )
        {
            // EXTRACT STRING START
            int str_len = val.via.str.size;
            char* str = Alloc( str_len);
            memset( str, '\0', str_len);
            str[str_len] = '\0';
            strncpy(str, val.via.str.ptr,str_len); 
            // EXTRACT STRING END 
    
            if( STR_Equals( "sender-address", header_name ) == true)
            {
                unpacked->address = str;
            }
            else if( STR_Equals("reply-port",header_name) == true)
            {
                unpacked->port = str;
            }
            else if( STR_Equals("service-name",header_name) == true)
            {
                unpacked->service_name = str;
            }
        }
        else if(val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
        {
            if( STR_Equals("services-count",header_name) == true)
            {
                unpacked->num_services = val.via.i64;
                unpacked->services = malloc(sizeof(char)*val.via.i64);
            }
        }
        else if( val.type == MSGPACK_OBJECT_ARRAY )
        {
            if( verbose) 
                PRINT("Processign services...\n");

            msgpack_object_array array = val.via.array;
            for( int i = 0; i < array.size;i++)
            {
                // EXTRACT STRING START
                int str_len = array.ptr[i].via.str.size;
                char* str = Alloc( str_len);
                memset( str, '\0', str_len);
                str[str_len] = '\0';
                strncpy(str, array.ptr[i].via.str.ptr,str_len); 
                
                unpacked->services[i] = str;

                if(verbose)
                    PRINT("SERVICE! %s\n",str);
            }

        }//array processing end
        else
        {
            // this is not a header or array but something else
            printf("\n"); 
        }
        ret = msgpack_unpack_next(&result, buffer, buflen, &off);
    } // finished unpacking.

    msgpack_unpacked_destroy(&result);

    if (ret == MSGPACK_UNPACK_PARSE_ERROR) 
    {
        printf("The data in the buf is invalid format.\n");
    }
} // UnpackRegistrationBuffer

void register_service(struct ServiceRegistration* service_registration )
{
    if(verbose)
        PRINT("register service registration request\n");

    if( verbose)
    {
        for( int i = 0 ; i < service_registration->num_services;i++)
        {
            PRINT("Service %s\n", service_registration->services[i]);
        }
    }
    
    list_add( &(service_registration->list),&(service_repository.list));
    
    print_service_repository(); // prints all services registered so far

}

void acknowledgement()
{
    // Send a message back to sender(client or server) with general ACK
}

void find_server(char* buffer, int buflen, Destination *dest)
{
    dest->address = NULL;
    dest->port = NULL;

    msgpack_unpacked result;
    msgpack_unpack_return ret;
    size_t off = 0;
    int i = 0;
    msgpack_unpacked_init(&result);

    ret = msgpack_unpack_next(&result, buffer, buflen, &off);

    while (ret == MSGPACK_UNPACK_SUCCESS) 
    {
        msgpack_object obj = result.data;
        
        char header_name[20];
        memset(header_name, '\0', 20);
        
        msgpack_object val = extract_header( &obj, header_name);
        
        if( STR_Equals( "op", header_name) && val.type == MSGPACK_OBJECT_STR )
        {
            msgpack_object_str string = val.via.str;
            // EXTRACT STRING START
            int str_len = string.size;
            char* str = Alloc( str_len);
            memset( str, '\0', str_len);
            str[str_len] = '\0';
            strncpy(str, string.ptr,str_len); 

            if( verbose )
                PRINT("Looking for %s\n", str);

            struct list_head *pos, *q;
            struct ServiceRegistration* tmp = malloc( sizeof( struct ServiceRegistration ));
            int count = 0;
        
            list_for_each( pos, &service_repository.list)
            {
                tmp = list_entry( pos, struct ServiceRegistration, list );
                ServiceReg *sreg = tmp;;
                bool found = false;
                if(verbose)
                    PRINT("Current SR is %s\n", sreg->service_name);
                for( int i = 0 ; i < sreg->num_services;i++)
                {
                    if( verbose )
                        PRINT("is %s == %s\n",str,sreg->services[i]);
                    if( STR_Equals( str, sreg->services[i]))
                    {
                        dest->address = sreg->address;
                        dest->port = sreg->port;
                        found = true; 
                        PRINT("FOUND service for required service %s at %s:%s\n",str, dest->address,dest->port);
                        goto done;
                    }
                }
            }
        }

        ret = msgpack_unpack_next(&result, buffer, buflen, &off);

    } // finished unpacking.
done:
    if(verbose)
        PRINT("finished.\n");

    msgpack_unpacked_destroy(&result);

    if (ret == MSGPACK_UNPACK_PARSE_ERROR) 
    {
        printf("The data in the buf is invalid format.\n");
    }
}

void find_client(char *buffer, int len, Destination *dest)
{
}

void forward_request(char* buffer, int len)
{
    if(verbose)
        PRINT("Forwarding request...\n");
    Destination *dest = Alloc( sizeof( Destination ));
    find_server(buffer, len, dest );
    
    if(verbose) 
        PRINT("About to send request to service at %s:%s\n", dest->address, dest->port);
    send_request( buffer, len, dest->address, dest->port);
}

void forward_response()
{
}



int main( int argc, char **argv )
{
    LIB_Init();
    INIT_LIST_HEAD(&service_repository.list);;

    struct Argument* portNumber = CMD_CreateNewArgument("port",
                                                        "port <number>",
                                                        "Set the port that the broker will listen on",
                                                        true,
                                                        true,
                                                        setPortNumber);
    struct Argument* verboseArg = CMD_CreateNewArgument("verbose",
                                                        "",
                                                        "Prints all messages verbosly",
                                                        false,
                                                        false,
                                                        setVerbose);
    struct Argument* waitIndefArg = CMD_CreateNewArgument("waitindef",
                                                        "",
                                                        "Wait indefinitely for new connections,else 60 secs and then dies",
                                                        false,
                                                        false,
                                                        setWaitIndefinitely);
    CMD_AddArgument(waitIndefArg);
    CMD_AddArgument(portNumber);
    CMD_AddArgument(verboseArg);


    if( argc > 1 )
    {
        enum ParseResult result = CMD_Parse(argc,argv,true);
        if( result != PARSE_SUCCESS )
        {
            return 1;
        }
    }
    else
    {
        CMD_ShowUsages("broker <options>");
        exit(0);
    }

    if(verbose) PRINT("Broker starting.\n");

    INIT();
    
    main_event_loop();

    LIB_Uninit();
    EXIT( 0 );
}
