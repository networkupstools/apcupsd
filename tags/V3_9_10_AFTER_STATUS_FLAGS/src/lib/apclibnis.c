/*
 * Network Utility Routines
 *
 *  Copyright (C) 1999 Kern Sibbald
 *
 *  apcupsd.c -- Simple Daemon to catch power failure signals from a
 *		 BackUPS, BackUPS Pro, or SmartUPS (from APCC).
 *	      -- Now SmartMode support for SmartUPS and BackUPS Pro.
 *
 *  Copyright (C) 1996-99 Andre M. Hedrick <andre@suse.com>
 *  All rights reserved.
 *
 */

/*
 *   Kern Sibbald, November 1999
 *
 *   part of this code is derived from the Prentice Hall book
 *   "Unix Network Programming" by W. Richard Stevens
 *
 *  Developers, please note: do not include apcupsd headers
 *  or other apcupsd internal information in this file
 *  as it is used by independent client programs such as the cgi programs.
 *
 */

/*
 *		       GNU GENERAL PUBLIC LICENSE
 *			  Version 2, June 1991
 *
 *  Copyright (C) 1989, 1991 Free Software Foundation, Inc.
 *			     675 Mass Ave, Cambridge, MA 02139, USA
 *  Everyone is permitted to copy and distribute verbatim copies
 *  of this license document, but changing it is not allowed.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "apc.h"

#ifndef   INADDR_NONE
#define   INADDR_NONE	 -1
#endif

struct sockaddr_in tcp_serv_addr;     /* socket information */
int net_errno = 0;		      /* error number -- not yet implemented */
char *net_errmsg = NULL;	      /* pointer to error message */
char net_errbuf[256];		      /* error message buffer for messages */


/*
 * Read a nbytes from the network.
 * It is possible that the total bytes require in several
 * read requests
 */

static int read_nbytes(int fd, char *ptr, int nbytes)
{
    int nleft, nread;
       
    nleft = nbytes;
    errno = 0;
    while (nleft > 0) {
	do {
	    nread = read(fd, ptr, nleft);    
	} while (nread == -1 && (errno == EINTR || errno == EAGAIN));
	if (nread <= 0) {
	    net_errno = errno;
	    return(nread);		 /* error, or EOF */
	}
	nleft -= nread;
	ptr += nread;
    }
    return(nbytes - nleft);	       /* return >= 0 */
}

/*
 * Write nbytes to the network.
 * It may require several writes.
 */

static int write_nbytes(int fd, char *ptr, int nbytes)
{
    int nleft, nwritten;

    nleft = nbytes;
    while (nleft > 0) {
	nwritten = write(fd, ptr, nleft);
	if (nwritten <= 0) {
	    net_errno = errno;
	    return (nwritten);		 /* error */
	}

	nleft -= nwritten;
	ptr += nwritten;
    }
    return(nbytes-nleft);
}

/* 
 * Receive a message from the other end. Each message consists of
 * two packets. The first is a header that contains the size
 * of the data that follows in the second packet.
 * Returns number of bytes read
 * Returns 0 on end of file
 * Returns -1 on hard end of file (i.e. network connection close)
 * Returns -2 on error
 */
int net_recv(int sockfd, char *buff, int maxlen)
{
    int nbytes;
    short pktsiz;

    /* get data size -- in short */
    if ((nbytes = read_nbytes(sockfd, (char *)&pktsiz, sizeof(short))) <= 0) {
	/* probably pipe broken because client died */
	return -1;			/* assume hard EOF received */
    }
    if (nbytes != sizeof(short)) {
	return -2;
    }

    pktsiz = ntohs(pktsiz);	     /* decode no. of bytes that follow */
    if (pktsiz > maxlen) {
        net_errmsg = "net_recv: record length too large\n";
	return -2;
    }
    if (pktsiz == 0)
	return 0;			/* soft EOF */

    /* now read the actual data */
    if ((nbytes = read_nbytes(sockfd, buff, pktsiz)) <=  0) {
        net_errmsg = "net_recv: read_nbytes error\n";
	return -2;
    }
    if (nbytes != pktsiz) {
        net_errmsg = "net_recv: error in read_nbytes\n";
	return -2;
    }
    return(nbytes);		       /* return actual length of message */
}

/*
 * Send a message over the network. The send consists of
 * two network packets. The first is sends a short containing
 * the length of the data packet which follows.
 * Returns number of bytes sent
 * Returns -1 on error
 */
int net_send(int sockfd, char *buff, int len)
{
    int rc;
    short pktsiz;

    pktsiz = htons((short)len);
    /* send short containing size of data packet */
    rc = write_nbytes(sockfd, (char *)&pktsiz, sizeof(short));
    if (rc != sizeof(short)) {
        net_errmsg = "net_send: write_nbytes error of length prefix\n";
	return -1;
    }

    /* send data packet */
    rc = write_nbytes(sockfd, buff, len);
    if (rc != len) {
        net_errmsg = "net_send: write_nbytes error\n";
	return -1;
    }
    return rc;
}

/*     
 * Open a TCP connection to the UPS network server
 * Returns -1 on error
 * Returns socket file descriptor otherwise
 */
int net_open(char *host, char *service, int port)
{
    int sockfd;
    unsigned int inaddr;	       /* Careful here to use unsigned int for */
				       /* compatibility with Alpha */
    struct hostent *hp;

    /* 
     * Fill in the structure serv_addr with the address of
     * the server that we want to connect with.
     */
    memset((char *) &tcp_serv_addr, 0, sizeof(tcp_serv_addr));
    tcp_serv_addr.sin_family = AF_INET;
    tcp_serv_addr.sin_port = htons(port);

    if ((inaddr = inet_addr(host)) != INADDR_NONE) {
       tcp_serv_addr.sin_addr.s_addr = inaddr;
    } else {
       if ((hp = gethostbyname(host)) == NULL) {
          net_errmsg ="tcp_open: hostname error\n";
	  return -1;
       }
       if (hp->h_length != sizeof(inaddr) || hp->h_addrtype != AF_INET) {
           net_errmsg ="tcp_open: funny gethostbyname value\n";
	   return -1;
       }
       tcp_serv_addr.sin_addr.s_addr = *(unsigned int *)hp->h_addr;
    }
    

    /* Open a TCP socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        net_errmsg = "tcp_open: cannot open stream socket\n";
	return -1;
    }
    /* connect to server */
    if (connect(sockfd, (struct sockaddr *) &tcp_serv_addr, sizeof(tcp_serv_addr)) < 0) {
        sprintf(net_errbuf, "tcp_open: cannot connect to server %s on port %d.\n\
ERR=%s\n", host, port, strerror(errno));
	net_errmsg = net_errbuf;
	return -1;
    }
    return sockfd;
}

/* Close the network connection */
void net_close(int sockfd)
{
    short pktsiz = 0;
    /* send EOF sentinel */
    write_nbytes(sockfd, (char *)&pktsiz, sizeof(short));
    close(sockfd);
}
 

int	upserror, syserrno;
