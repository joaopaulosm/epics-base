
/* $Id$ */
/*
 *      Author:		Jeff Hill 
 *      Date:       04-05-94 
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Controls and Computing Group
 *              Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define epicsExportSharedSymbols
#include "osiSock.h"
#include "epicsAssert.h"
#include "errlog.h"

#ifdef DEBUG
#   define ifDepenDebugPrintf(argsInParen) printf argsInParen
#else
#   define ifDepenDebugPrintf(argsInParen)
#endif
    
/*
 * Move to the next ifreq structure
 * Made difficult by the fact that addresses larger than the structure
 * size may be returned from the kernel.
 */
static struct ifreq * ifreqNext ( struct ifreq *pifreq )
{
    size_t size;

    size = ifreq_size ( pifreq );
    if ( size < sizeof ( *pifreq ) ) {
	    size = sizeof ( *pifreq );
    }

    return ( struct ifreq * )( size + ( char * ) pifreq );
}


/*
 * osiSockDiscoverBroadcastAddresses ()
 */
epicsShareFunc void epicsShareAPI osiSockDiscoverBroadcastAddresses
     (ELLLIST *pList, SOCKET socket, const osiSockAddr *pMatchAddr)
{
    static const unsigned           nelem = 100;
    int                             status;
    struct ifconf                   ifconf;
    struct ifreq                    *pIfreqList;
    struct ifreq                    *pIfreqListEnd;
    struct ifreq                    *pifreq;
    struct ifreq                    *pnextifreq;
    osiSockAddrNode                 *pNewNode;
     
    /*
     * use pool so that we avoid using too much stack space
     *
     * nelem is set to the maximum interfaces 
     * on one machine here
     */
    pIfreqList = (struct ifreq *) calloc ( nelem, sizeof(*pifreq) );
    if (!pIfreqList) {
        errlogPrintf ("osiSockDiscoverBroadcastAddresses(): no memory to complete request\n");
        return;
    }
    
    ifconf.ifc_len = nelem * sizeof(*pifreq);
    ifconf.ifc_req = pIfreqList;
    status = socket_ioctl (socket, SIOCGIFCONF, &ifconf);
    if (status < 0 || ifconf.ifc_len == 0) {
        errlogPrintf ("osiSockDiscoverBroadcastAddresses(): unable to fetch network interface configuration\n");
        free (pIfreqList);
        return;
    }
    
    pIfreqListEnd = (struct ifreq *) (ifconf.ifc_len + (char *) pIfreqList);
    pIfreqListEnd--;

    for ( pifreq = pIfreqList; pifreq <= pIfreqListEnd; pifreq = pnextifreq ) {

        /*
         * find the next if req
         */
        pnextifreq = ifreqNext (pifreq);

        /*
         * If its not an internet interface then dont use it 
         */
        if ( pifreq->ifr_addr.sa_family != AF_INET ) {
             ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): interface \"%s\" was not AF_INET\n", pifreq->ifr_name) );
             continue;
        }

        /*
         * if it isnt a wildcarded interface then look for
         * an exact match
         */
        if ( pMatchAddr->sa.sa_family != AF_UNSPEC ) {
            if ( pMatchAddr->sa.sa_family != AF_INET ) {
                continue;
            }
            if ( pMatchAddr->ia.sin_addr.s_addr != htonl (INADDR_ANY) ) {
                 struct sockaddr_in *pInetAddr = (struct sockaddr_in *) &pifreq->ifr_addr;
                 if ( pInetAddr->sin_addr.s_addr != pMatchAddr->ia.sin_addr.s_addr ) {
                     ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): net intf \"%s\" didnt match\n", pifreq->ifr_name) );
                     continue;
                 }
            }
        }

        status = socket_ioctl ( socket, SIOCGIFFLAGS, pifreq );
        if ( status ) {
            errlogPrintf ("osiSockDiscoverBroadcastAddresses(): net intf flags fetch for \"%s\" failed\n", pifreq->ifr_name);
            continue;
        }
        
        /*
         * dont bother with interfaces that have been disabled
         */
        if ( ! ( pifreq->ifr_flags & IFF_UP ) ) {
             ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): net intf \"%s\" was down\n", pifreq->ifr_name) );
             continue;
        }

        /*
         * dont use the loop back interface
         */
        if ( pifreq->ifr_flags & IFF_LOOPBACK ) {
             ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): ignoring loopback interface: \"%s\"\n", pifreq->ifr_name) );
             continue;
        }

        pNewNode = (osiSockAddrNode *) calloc (1, sizeof (*pNewNode) );
        if ( pNewNode == NULL ) {
            errlogPrintf ( "osiSockDiscoverBroadcastAddresses(): no memory available for configuration\n" );
            free ( pIfreqList );
            return;
        }

        /*
         * If this is an interface that supports
         * broadcast fetch the broadcast address.
         *
         * Otherwise if this is a point to point 
         * interface then use the destination address.
         *
         * Otherwise CA will not query through the 
         * interface.
         */
        if ( pifreq->ifr_flags & IFF_BROADCAST ) {
            status = socket_ioctl (socket, SIOCGIFBRDADDR, pifreq);
            if ( status ) {
                errlogPrintf ("osiSockDiscoverBroadcastAddresses(): net intf \"%s\": bcast addr fetch fail\n", pifreq->ifr_name);
                free ( pNewNode );
                continue;
            }
            pNewNode->addr.sa = pifreq->ifr_broadaddr;
            ifDepenDebugPrintf ( ( "found broadcast addr = %x\n", ntohl ( pNewNode->addr.ia.sin_addr.s_addr ) ) );
            status = socket_ioctl (socket, SIOCGIFNETMASK, pifreq);
            if ( status ) {
                errlogPrintf ( "osiSockDiscoverBroadcastAddresses(): net intf \"%s\": net mask fetch fail\n", pifreq->ifr_name );
                free ( pNewNode );
                continue;
            }
            pNewNode->netMask.sa = pifreq->ifr_addr;
            ifDepenDebugPrintf ( ( "found net mask = %x\n", ntohl ( pNewNode->netMask.ia.sin_addr.s_addr ) ) );
        }
#if defined (IFF_POINTOPOINT)
        else if ( pifreq->ifr_flags & IFF_POINTOPOINT ) {
            status = socket_ioctl ( socket, SIOCGIFDSTADDR, pifreq);
            if ( status ) {
                ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): net intf \"%s\": pt to pt addr fetch fail\n", pifreq->ifr_name) );
                free ( pNewNode );
                continue;
            }
            pNewNode->addr.sa = pifreq->ifr_dstaddr;
            memset ( &pNewNode->netMask, '\0', sizeof ( pNewNode->netMask ) );
        }
#endif
        else {
            ifDepenDebugPrintf ( ( "osiSockDiscoverBroadcastAddresses(): net intf \"%s\": not point to point or bcast?\n", pifreq->ifr_name ) );
            free ( pNewNode );
            continue;
        }

        ifDepenDebugPrintf ( ("osiSockDiscoverBroadcastAddresses(): net intf \"%s\" found\n", pifreq->ifr_name) );

        /*
         * LOCK applied externally
         */
        ellAdd ( pList, &pNewNode->node );
    }

    free ( pIfreqList );
}
     
/*
 * osiLocalAddr ()
 */
epicsShareFunc osiSockAddr epicsShareAPI osiLocalAddr (SOCKET socket)
{
    static const unsigned   nelem = 100;
    static char             init = 0;
    static osiSockAddr      addr;
    int                     status;
    struct ifconf           ifconf;
    struct ifreq            *pIfreqList;
    struct ifreq            *pifreq;
    struct ifreq            *pIfreqListEnd;
    struct ifreq            *pnextifreq;

    if ( init ) {
        return addr;
    }

    memset ( (void *) &addr, '\0', sizeof ( addr ) );
    addr.sa.sa_family = AF_UNSPEC;
    
    pIfreqList = (struct ifreq *) calloc ( nelem, sizeof(*pIfreqList) );
    if ( ! pIfreqList ) {
        errlogPrintf ( "osiLocalAddr(): no memory to complete request\n" );
        return addr;
    }
 
    ifconf.ifc_len = nelem * sizeof ( *pIfreqList );
    ifconf.ifc_req = pIfreqList;
    status = socket_ioctl ( socket, SIOCGIFCONF, &ifconf );
    if ( status < 0 || ifconf.ifc_len == 0 ) {
        errlogPrintf (
            "osiLocalAddr(): SIOCGIFCONF ioctl failed because \"%s\"\n",
            SOCKERRSTR (SOCKERRNO) );
        free ( pIfreqList );
        return addr;
    }
    
    pIfreqListEnd = (struct ifreq *) ( ifconf.ifc_len + (char *) ifconf.ifc_req );
    pIfreqListEnd--;

    for ( pifreq = ifconf.ifc_req; pifreq <= pIfreqListEnd; pifreq = pnextifreq ) {
        osiSockAddr addrCpy;

        /*
         * find the next if req
         */
        pnextifreq = ifreqNext ( pifreq );

        if ( pifreq->ifr_addr.sa_family != AF_INET ) {
            ifDepenDebugPrintf ( ("osiLocalAddr(): interface %s was not AF_INET\n", pifreq->ifr_name) );
            continue;
        }

        addrCpy.sa = pifreq->ifr_addr;

        status = socket_ioctl ( socket, SIOCGIFFLAGS, pifreq );
        if ( status < 0 ) {
            errlogPrintf ( "osiLocalAddr(): net intf flags fetch for %s failed\n", pifreq->ifr_name );
            continue;
        }

        if ( ! ( pifreq->ifr_flags & IFF_UP ) ) {
            ifDepenDebugPrintf ( ("osiLocalAddr(): net intf %s was down\n", pifreq->ifr_name) );
            continue;
        }

        /*
         * dont use the loop back interface
         */
        if ( pifreq->ifr_flags & IFF_LOOPBACK ) {
            ifDepenDebugPrintf ( ("osiLocalAddr(): ignoring loopback interface: %s\n", pifreq->ifr_name) );
            continue;
        }

        ifDepenDebugPrintf ( ("osiLocalAddr(): net intf %s found\n", pifreq->ifr_name) );

        init = 1;
        addr = addrCpy;
        break;
    }

    free ( pIfreqList );
    return addr;
}
