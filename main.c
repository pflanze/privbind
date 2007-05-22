/*
 * privbind - allow unpriviledged apps to bind to priviledged ports
 * Copyright (C) 2006-2007 Shachar Shemesh
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ipc.h"

#define FALSE (0!=0)
#define TRUE (0==0)

struct cmdoptions {
    uid_t uid; /* UID to turn into */
    gid_t gid; /* GID to turn into */
    int numbinds; /* Number of binds to catch before program can make do on its own */
} options;

void usage(char *progname)
{
    fprintf(stderr, "Usage: %s -u UID [-g GID] [-n NUM] command line ...\n", progname);
    fprintf(stderr, "Run '%s -h' for more information.\n", progname);
}
void help(progname)
{
    printf("%s - run a program as an unpriviledged user, while still being\n",
	   PACKAGE_STRING);
    printf("     able to bind to low ports.\n");
    printf("Usage: %s -u UID [-g GID] [-n NUM] command line ...\n", progname);
    printf("\n"
	"-u - Name or id of user to run as (mandatory)\n"
	"-g - Name or id of group to run as (default: the user's default group)\n"
	"-n - number of binds to catch. After this many binds have happened,\n"
        "     all root proccesses exit.\n"
	"-h - This help screen\n");
}

int parse_cmdline( int argc, char *argv[] )
{
    /* Fill in default values */
    options.numbinds=0;
    options.uid=0;
    options.gid=0;
    
    int opt;

    while( (opt=getopt(argc, argv, "+n:du:g:h" ))!=-1 ) {
	switch(opt) {
	case 'n':
	    options.numbinds=atoi(optarg);
	    break;
	case 'u':
	     {
		struct passwd *pw=getpwnam(optarg);
		if( pw!=NULL ) {
		    options.uid=pw->pw_uid;
		    /* set the user's default group */
		    if( options.gid==0 ) {
			options.gid=pw->pw_gid;
		    }
		} else {
		    options.uid=atoi(optarg);
		    if( options.uid==0 ) {
			fprintf(stderr, "Username '%s' not found\n", optarg);
			exit(1);
		    }
		}
	    }
	    break;
	case 'g':
            {
		struct group *gr=getgrnam(optarg);
		if( gr!=NULL ) {
		    options.gid=gr->gr_gid;
		} else {
		    options.gid=atoi(optarg);
		    if( options.gid==0 ) {
			fprintf(stderr, "Group name '%s' not found\n", optarg);
			exit(1);
		    }
		}
	    }
	    break;
	case 'h':
	    help(argv[0]);
	    exit(0);
	case '?':
	    usage(argv[0]);
	    exit(1);
	}
    }

    if(options.uid==0){
	fprintf(stderr, "Missing UID (-u) option.\n");
	usage(argv[0]);
	exit(1);
    }
    if(options.gid==0){
	fprintf(stderr, "Missing GID (-g) option.\n");
	usage(argv[0]);
	exit(1);
    }

    if( (argc-optind)<=0 ) {
	fprintf(stderr, "ERROR: missing a command to run.\n");
	usage(argv[0]);
	exit(1);
    }
    return optind;
}

int main( int argc, char *argv[] )
{
    int skipcount=parse_cmdline( argc, argv );

    /* Warn if we're run as SUID */
    if( getuid()!=geteuid() ) {
	fprintf(stderr, "!!!!Running privbind SUID is a security risk!!!!\n");
    }

    // Create a couple of sockets for communication with our children
    int sv[2];
    if( socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)<0 ) {
	perror("privbind: socketpair");
	return 2;
    }

    pid_t child_pid=fork();
    switch(child_pid) {
    case -1:
	perror("privbind: fork");
	exit(1);

    case 0:
	/* We are the child */

	/* Drop privileges */
	if( setgroups(0, NULL )<0 ) {
	    perror("privbind: setgroups");
	    exit(2);
	}
	if( setgid(options.gid)<0 ) {
	    perror("privbind: setgid");
	    close(sv[0]);
	    exit(2);
	}
	if( setuid(options.uid)<0 ) {
	    perror("privbind: setuid");
	    close(sv[0]);
	    exit(2);
	}

	/* Close the parent socket */
	close(sv[1]);

	/* Rename the child socket to the pre-determined fd */
	if( dup2(sv[0], COMM_SOCKET)<0 ) {
	    perror("privbind: dup2");
	    exit(2);
	}
	close(sv[0]);

	/* Set the LD_PRELOAD environment variable */
	char *ldpreload=getenv("LD_PRELOAD");
	if( ldpreload==NULL ) {
	    setenv("LD_PRELOAD", PRELOADLIBNAME, FALSE );
	} else {
	    char *newpreload=malloc(strlen(ldpreload)+sizeof(PRELOADLIBNAME)+1);
	    if( newpreload==NULL ) {
		fprintf(stderr, "privbind: Error creating preload environment - out of memory\n");
		exit(2);
	    }

	    sprintf( newpreload, "%s:%s", PRELOADLIBNAME, ldpreload );

	    setenv("LD_PRELOAD", newpreload, TRUE );

	    free(newpreload);
	}

	/* Set up the variables for exec */
	char **new_argv=calloc(argc-skipcount+1, sizeof(char*) );
	if( new_argv==NULL ) {
	    fprintf(stderr, "privbind: Error creating new command line: out of memory\n");
	    exit(2);
	}

	int i;
	for( i=0; i<argc-skipcount; ++i ) {
	    new_argv[i]=argv[i+skipcount];
	}
	
	execvp(new_argv[0], new_argv);
	perror("privbind: exec");
	return 2;
	break;
    default:
	/* We are the parent */

	/* Close the child socket */
	close(sv[0]);

	/* wait for request from the child */
	do {
	  struct msghdr msghdr={0};
	  struct cmsghdr *cmsg;
	  char buf[CMSG_SPACE(sizeof(int))];
	  struct ipc_msg_req request;
	  struct iovec iov;
	  struct ipc_msg_reply reply = {0};
	  int recvbytes;

	  msghdr.msg_control=buf;
	  msghdr.msg_controllen=sizeof(buf);

	  iov.iov_base = &request;
	  iov.iov_len = sizeof request;

	  msghdr.msg_iov = &iov;
	  msghdr.msg_iovlen = 1;

	  if ( (recvbytes = recvmsg( sv[1], &msghdr, 0)) > 0) {
	    if ((cmsg = (struct cmsghdr *)CMSG_FIRSTHDR(&msghdr)) != NULL) {
	      switch (request.type) {
	      case MSG_REQ_NONE:
	      	reply.type = MSG_REP_NONE;
		if (send(sv[1], &reply, sizeof reply, 0) != sizeof reply)
		  perror("privbind: send");
		break;
	      case MSG_REQ_BIND:
	      	reply.type = MSG_REP_STAT;
	  	int sock;
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(int))
		    && cmsg->cmsg_level == SOL_SOCKET
		    && cmsg->cmsg_type == SCM_RIGHTS)
		  sock = *((int*)CMSG_DATA(cmsg));
		else {
		  sock = -1;
		}
		reply.data.stat.retval =
		  bind(sock, (struct sockaddr *)&request.data.bind.addr,
		       sizeof request.data.bind.addr);
		if (reply.data.stat.retval < 0)
		  reply.data.stat.error = errno;
		if (send(sv[1], &reply, sizeof reply, 0) != sizeof reply)
		  perror("privbind: send");
		if (sock > -1 && close(sock))
		  perror("privbind: close");
		break;
	      default:
		fprintf(stderr, "privbind: bad request type: %d\n",
		  request.type);
		break;
	      }
	    } else {
	      fprintf(stderr, "privbind: empty request\n");
	    }
	  } else if (recvbytes == 0) {
	    /* If the child closed its end of the socket, it means the
               child has exited. Let's exit with its exit code. */
	    int status;
            waitpid(child_pid, &status, 0);
	    exit (WIFEXITED(status) ? WEXITSTATUS(status) : 1);

	    /*break;*/
	  } else {
	    perror("privbind: recvmsg");
	  }
	} while (options.numbinds == 0 || --options.numbinds > 0);


	/* If we got here, the child has done the number of binds
	   specified by the -n option, and we have nothing more to do
	   and should exit, leaving behind no root process */
    }

    return 0;
}
