/************************************************************************
 *   IRC - Internet Relay Chat, src/send.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *		      University of Oulu, Computing Center
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id$ */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "h.h"
#include <stdio.h>
#include "numeric.h"

#ifdef	IRCII_KLUDGE
#define	NEWLINE	"\n"
#else
#define NEWLINE	"\r\n"
#endif

#ifdef ALWAYS_SEND_DURING_SPLIT
extern int currently_processing_netsplit;
#endif

static char sendbuf[2048];
static int  send_message(aClient *, char *, int);

static int  sentalong[MAXCONNECTIONS];

int format(char *, char *, va_list vl);

/*
 * dead_link
 *
 * somewhere along the lines of sending out, there was an error.
 * we can't close it from the send loop, so mark it as dead
 * and close it from the main loop.
 *
 * if this link is a server, tell routing people.
 */

static int dead_link(aClient *to, char *notice) {

   int errtmp = errno;  /* so we don't munge this later */

   to->flags |= FLAGS_DEADSOCKET;
   /*
    * If because of BUFFERPOOL problem then clean dbuf's now so that
    * notices don't hurt operators below.
    */
   DBufClear(&to->recvQ);
   DBufClear(&to->sendQ);
   /* Ok, if the link we're dropping is a server, send a routing
    * notice..
    */
   if (IsServer(to) && !(to->flags & FLAGS_CLOSING))
   {
      char fbuf[512];

      ircsprintf(fbuf, "from %s: %s", me.name, notice);
      sendto_gnotice(fbuf, get_client_name(to, HIDEME), strerror(errtmp));
      ircsprintf(fbuf, ":%s GNOTICE :%s", me.name, notice);
      sendto_serv_butone(to, fbuf, get_client_name(to, HIDEME), strerror(errtmp));
   }  
 
return -1;
}
/*
 * * flush_connections *      Used to empty all output buffers for
 * all connections. Should only *       be called once per scan of
 * connections. There should be a select in *   here perhaps but that
 * means either forcing a timeout or doing a poll. *    When flushing,
 * all we do is empty the obuffer array for each local *        client
 * and try to send it. if we cant send it, it goes into the sendQ *
 * -avalon
 */
void flush_connections(int fd) {
   Reg int     i;
   Reg aClient *cptr;
   
   if (fd == me.fd) {
      for (i = highest_fd; i >= 0; i--)
	if ((cptr = local[i]) && DBufLength(&cptr->sendQ) > 0)
	  send_queued(cptr);
   }
   else if (fd >= 0 && (cptr = local[fd]) && DBufLength(&cptr->sendQ) > 0)
     send_queued(cptr);
}

/*
 * * send_message *   Internal utility which delivers one message
 * buffer to the *      socket. Takes care of the error handling and
 * buffering, if *      needed.
 */
static int send_message(aClient *to, char *msg, int len) {
   static int  SQinK;
   
#ifdef DUMP_DEBUG
   fprintf(dumpfp, "-> %s: %s"\n, (to->name ? to->name : "*"), msg);
#endif
   if(len>509) {
		msg[510]='\r';
		msg[511]='\n';
		msg[512]='\0';
		len=512;
	}
	else {
		msg[len] = '\r';
		msg[len+1] = '\n';
		msg[len+2] = '\0';
		len+=2;
	}   
   if (to->from)
     to = to->from;   /* shouldn't be necessary */
   
   if (IsMe(to)) {
      sendto_ops("Trying to send to myself! [%s]", msg);
      return 0;
   }
   
   if (IsDead(to))
     return 0;
   if (DBufLength(&to->sendQ) > get_sendq(to)) {
      /* this would be a duplicate notice, but it contains some useful information that
         would be spamming the rest of the network. Kept in. - lucas */
      if (IsServer(to)) 
	sendto_ops("Max SendQ limit exceeded for %s: %d > %d",
		get_client_name(to, HIDEME), DBufLength(&to->sendQ), get_sendq(to));
      if (IsClient(to))
	to->flags |= FLAGS_SENDQEX;
      return dead_link(to, "Max Sendq exceeded for %s, closing link");
   }
   else if (dbuf_put(&to->sendQ, msg, len) < 0)
     return dead_link(to, "Buffer allocation error for %s, closing link");
   /*
    * * Update statistics. The following is slightly incorrect *
    * because it counts messages even if queued, but bytes * only
    * really sent. Queued bytes get updated in SendQueued.
    */
   to->sendM += 1;
   me.sendM += 1;
   if (to->acpt != &me)
     to->acpt->sendM += 1;
   /*
    * * This little bit is to stop the sendQ from growing too large
    * when * there is no need for it to. Thus we call send_queued()
    * every time * 2k has been added to the queue since the last
    * non-fatal write. * Also stops us from deliberately building a
    * large sendQ and then * trying to flood that link with data
    * (possible during the net * relinking done by servers with a large
    * load).
    */
   /*
    * Well, let's try every 4k for clients, and immediately for servers
    * -Taner
    */

#ifdef ALWAYS_SEND_DURING_SPLIT
   if (currently_processing_netsplit && !(to->flags & FLAGS_BLOCKED))
   {
      send_queued(to);
      return 0;
   }
#endif

   SQinK = (DBufLength(&to->sendQ) >> 10);
   if (IsServer(to)) {
      if (SQinK > to->lastsq)
	send_queued(to);
   }
   else {
      if (SQinK > (to->lastsq + 4))
	send_queued(to);
   }
   return 0;
}


/*
 * * send_queued *    This function is called from the main
 * select-loop (or whatever) *  when there is a chance the some output
 * would be possible. This *    attempts to empty the send queue as far
 * as possible...
 */
int send_queued(aClient *to) {
   char       *msg;
   int         len, rlen;
	
   /*
    * * Once socket is marked dead, we cannot start writing to it, *
    * even if the error is removed...
    */
   if (IsDead(to)) {
      /*
       * * Actually, we should *NEVER* get here--something is * not
       * working correct if send_queued is called for a * dead
       * socket... --msa
       */
      return -1;
   }
   
   while (DBufLength(&to->sendQ) > 0) {
      msg = dbuf_map(&to->sendQ, &len);
      /*
       * Returns always len > 0 
       */
      if ((rlen = deliver_it(to, msg, len)) < 0)
	return dead_link(to, "Write error to %s, closing link (%s)");
      (void) dbuf_delete(&to->sendQ, rlen);
      to->lastsq = (DBufLength(&to->sendQ) >> 10);
      if (rlen < len)
	/* ..or should I continue until rlen==0? */
	/* no... rlen==0 means the send returned EWOULDBLOCK... */
	break;
   }

   if (to->flags & FLAGS_SOBSENT && !(to->flags & FLAGS_BURST) && DBufLength(&to->sendQ) < 20480) { /* 20k */
     sendto_one(to, "BURST %d", DBufLength(&to->sendQ));
     to->flags &= (~FLAGS_SOBSENT);
     if (!(to->flags & FLAGS_EOBRECV)) { /* hey we're the last to synch.. let's notify */
       sendto_gnotice("from %s: synch to %s in %d %s", me.name, to->name, (timeofday-to->firsttime), 
         (timeofday-to->firsttime)==1?"sec":"secs");
   #ifdef HUB
       sendto_serv_butone(to, ":%s GNOTICE :synch to %s in %d %s", me.name, to->name,
         (timeofday-to->firsttime), (timeofday-to->firsttime)==1?"sec":"secs");
   #endif
   #ifdef HTM_LOCK_ON_NETBURST
       HTMLOCK = NO;
   #endif
     }
   }
   
   return (IsDead(to)) ? -1 : 0;
}
/*
 * * send message to single client
 */
void sendto_one(aClient *to, char *pattern, ...) {
   va_list vl;
   int len;		/* used for the length of the current message */
   
   va_start(vl, pattern);
   len = ircvsprintf(sendbuf, pattern, vl);
   
   if (to->from)
     to = to->from;
   if (IsMe(to)) {
      sendto_ops("Trying to send [%s] to myself!", sendbuf);
      return;
   }
   send_message(to, sendbuf, len);
   va_end(vl);
}

void vsendto_one(aClient *to, char *pattern, va_list vl) {
	int len;		/* used for the length of the current message */
   
   len = ircvsprintf(sendbuf, pattern, vl);
   
   if (to->from)
     to = to->from;
   if (IsMe(to) && to->fd >= 0) {
      sendto_ops("Trying to send [%s] to myself!", sendbuf);
      return;
   }
   send_message(to, sendbuf, len);
}

void sendto_channel_butone(aClient *one, aClient *from, aChannel *chptr, 
									char *pattern, ...) {
   chanMember   *cm;
   aClient *acptr;
   int     i;
   va_list vl;
   
   va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cm = chptr->members; cm; cm = cm->next) {
      acptr = cm->cptr;
      if (acptr->from == one)
		  continue;		/* ...was the one I should skip */
      i = acptr->from->fd;
      if (MyConnect(acptr) && IsRegisteredUser(acptr)) {
			vsendto_prefix_one(acptr, from, pattern, vl);
			sentalong[i] = 1;
      }
      else {
			/*
			 * Now check whether a message has been sent to this remote
			 * link already
			 */
			if (sentalong[i] == 0) {
				vsendto_prefix_one(acptr, from, pattern, vl);
				sentalong[i] = 1;
			}
      }
   }
   va_end(vl);
   return;
}
/*
 * sendto_server_butone
 * 
 * Send a message to all connected servers except the client 'one'.
 */
void sendto_serv_butone(aClient *one, char *pattern, ...) {
	int i;
	aClient *cptr;
	int j, k = 0;
	fdlist send_fdlist;
	va_list vl;
	
	va_start(vl, pattern);
   for (i = serv_fdlist.entry[j = 1];
		  j <= serv_fdlist.last_entry; i = serv_fdlist.entry[++j]) {
      if (!(cptr = local[i]) || (one && cptr == one->from))
		  continue;
      send_fdlist.entry[++k] = i;
   }
   send_fdlist.last_entry = k;
   if (k)
	  vsendto_fdlist(&send_fdlist, pattern, vl);
	va_end(vl);
   return;
}

/*
 * sendto_noquit_servs_butone
 * 
 * Send a message to all noquit servs if noquit = 1,
 * or all non-noquit servs if noquit = 0
 * we omit "one", too.
 */
void sendto_noquit_servs_butone(int noquit, aClient *one, char *pattern, ...) {
	int i;
	aClient *cptr;
	int j, k = 0;
	fdlist send_fdlist;
	va_list vl;
	
	va_start(vl, pattern);
   for (i = serv_fdlist.entry[j = 1];
		  j <= serv_fdlist.last_entry; i = serv_fdlist.entry[++j]) {
      if (!(cptr = local[i]) || 
           (noquit && !IsNoQuit(cptr)) || 
           (!noquit && IsNoQuit(cptr)) || 
            one == cptr)
		  continue;

      send_fdlist.entry[++k] = i;
   }
   send_fdlist.last_entry = k;
   if (k)
	  vsendto_fdlist(&send_fdlist, pattern, vl);
	va_end(vl);
   return;
}


/*
 * sendto_common_channels()
 * 
 * Sends a message to all people (inclusing user) on local server who are
 * in same channel with user.
 */
void sendto_common_channels(aClient *user, char *pattern, ...)
{
	Link *channels;
	chanMember *users;
	aClient *cptr;
	va_list vl;
	
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   if (user->fd >= 0)
	  sentalong[user->fd] = 1;
   if (user->user)
	  for (channels = user->user->channel; channels; channels = channels->next)
		 for (users = channels->value.chptr->members; users; users = users->next) {
			 cptr = users->cptr;
			 if (!MyConnect(cptr) || sentalong[cptr->fd])
				continue;
			 sentalong[cptr->fd]++;
			 vsendto_prefix_one(cptr, user, pattern, vl);;
		 }
   if (MyConnect(user))
	  vsendto_prefix_one(user, user, pattern, vl);
	va_end(vl);
	return;
}
#ifdef FLUD
void sendto_channel_butlocal(aClient *one, aClient *from, aChannel *chptr, char *pattern, ...)
{
	chanMember *cm;
	aClient *acptr;
	int i;
	int sentalong[MAXCONNECTIONS];
	va_list vl;
	  
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cm = chptr->members; cm; cm = cm->next) {
      acptr = cm->cptr;
      if (acptr->from == one)
		  continue;		/* ...was the one I should skip */
      i = acptr->from->fd;
      if (!MyFludConnect(acptr)) {
			/*
			 * Now check whether a message has been sent to this remote
			 * link already
			 */
			if (sentalong[i] == 0) {
				vsendto_prefix_one(acptr, from, pattern, vl);
				sentalong[i] = 1;
			}
      }
   }
	va_end(vl);
   return;
}
#endif /* FLUD */

/*
 * sendto_channel_butserv
 * 
 * Send a message to all members of a channel that are connected to this
 * server.
 */
void
sendto_channel_butserv(aChannel *chptr, aClient *from, char *pattern, ...)
{
	chanMember  *cm;
	aClient *acptr;
	va_list vl;
	
	va_start(vl, pattern);
   for (cm = chptr->members; cm; cm = cm->next)
	  if (MyConnect(acptr = cm->cptr))
		 vsendto_prefix_one(acptr, from, pattern, vl);
	va_end(vl);
   return;
}

/*
 * sendto_ssjoin_servs
 * 
 * send to all servers with ssjoin capability (or not)
 * 
 */
void sendto_ssjoin_servs(int ssjoin, aChannel *chptr, aClient *from, char *pattern, ...)
{
	int j, k = 0;
	fdlist      send_fdlist;
	int     i;
	aClient *cptr;
	va_list vl;
	
   if (chptr) {
      if (*chptr->chname == '&')
		  return;
   }
	va_start(vl, pattern);
   for (i = serv_fdlist.entry[j = 1]; j <= serv_fdlist.last_entry; i = serv_fdlist.entry[++j]) {
      if (!(cptr = local[i]) || 
           (cptr == from) ||
           (ssjoin && !IsSSJoin(cptr)) ||
           (!ssjoin && IsSSJoin(cptr)))
		  continue;

      send_fdlist.entry[++k] = i;
   }
   send_fdlist.last_entry = k;
   if (k)
	  vsendto_fdlist(&send_fdlist, pattern, vl);
	va_end(vl);
   return;
}


/*
 * * send a msg to all ppl on servers/hosts that match a specified mask *
 * (used for enhanced PRIVMSGs) *
 * 
 * addition -- Armin, 8jun90 (gruner@informatik.tu-muenchen.de)
 */

static int match_it(aClient *one, char *mask, int what)
{
   if (what == MATCH_HOST)
	  return (match(mask, one->user->host) == 0);
   else
	  return (match(mask, one->user->server) == 0);
}
/*
 * sendto_match_servs
 * 
 * send to all servers which match the mask at the end of a channel name
 * (if there is a mask present) or to all if no mask.
 */
void sendto_match_servs(aChannel *chptr, aClient *from, char *pattern, ...)
{
	int j, k = 0;
	fdlist      send_fdlist;
	int     i;
	aClient *cptr;
	va_list vl;
	
   if (chptr) {
      if (*chptr->chname == '&')
		  return;
   }
	va_start(vl, pattern);
   for (i = serv_fdlist.entry[j = 1]; j <= serv_fdlist.last_entry; i = serv_fdlist.entry[++j]) {
      if (!(cptr = local[i]))
		  continue;
      if (cptr == from)
		  continue;
      send_fdlist.entry[++k] = i;
   }
   send_fdlist.last_entry = k;
   if (k)
	  vsendto_fdlist(&send_fdlist, pattern, vl);
	va_end(vl);
   return;
}

/*
 * sendto_match_butone
 * 
 * Send to all clients which match the mask in a way defined on 'what';
 * either by user hostname or user servername.
 */
void sendto_match_butone(aClient *one, aClient *from, char *mask, int what, 
								 char *pattern, ...)
{
	int     i;
	aClient *cptr, *acptr;
	char cansendlocal, cansendglobal;
	va_list vl;
	
	va_start(vl, pattern);
	if (MyConnect(from)) {
		cansendlocal = (OPCanLNotice(from)) ? 1 : 0;
		cansendglobal = (OPCanGNotice(from)) ? 1 : 0;
	} 
	else 
	  cansendlocal = cansendglobal = 1;
   for (i = 0; i <= highest_fd; i++) {
      if (!(cptr = local[i]))
		  continue;		/* that clients are not mine */
      if (cptr == one)		/* must skip the origin !! */
		  continue;
      if (IsServer(cptr)) {
			if (!cansendglobal) continue;
			for (acptr = client; acptr; acptr = acptr->next)
			  if (IsRegisteredUser(acptr)
					&& match_it(acptr, mask, what)
					&& acptr->from == cptr)
				 break;
			/*
			 * a person on that server matches the mask, so we * send *one*
			 * msg to that server ...
			 */
			if (acptr == NULL)
			  continue;
			/*
			 * ... but only if there *IS* a matching person 
			 */
      }
      /*
       * my client, does he match ? 
       */
      else if (!cansendlocal || !(IsRegisteredUser(cptr) &&
											 match_it(cptr, mask, what)))
		  continue;
      vsendto_prefix_one(cptr, from, pattern, vl);
   }
	va_end(vl);
   return;
}
/*
 * sendto_all_butone.
 * 
 * Send a message to all connections except 'one'. The basic wall type
 * message generator.
 */
void
sendto_all_butone(aClient *one, aClient *from, char *pattern, ...)
{
	int     i;
	aClient *cptr;
	va_list vl;
	
	va_start(vl, pattern);
   for (i = 0; i <= highest_fd; i++)
	  if ((cptr = local[i]) && !IsMe(cptr) && one != cptr)
		 vsendto_prefix_one(cptr, from, pattern, vl);
	va_end(vl);
   return;
}
/*
 * sendto_ops_lev
 * 
 * Send to *local* ops only at a certain level... 0 = normal +s 1 = client
 * connect/disconnect   (+c) [IRCOPS ONLY] 2 = bot rejection
 * (+r) 3 = server kills                      (+k)
 */
void sendto_ops_lev(int lev, char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	va_list vl;
	
	va_start(vl,pattern);
	for (i = 0; i <= highest_fd; i++)
	  if ((cptr = local[i]) && !IsServer(cptr) && !IsMe(cptr)) {
		  switch (lev) {
			case CCONN_LEV:
			  if (!SendCConnNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			case REJ_LEV:
			  if (!SendRejNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			case SKILL_LEV:
			  if (!SendSkillNotice(cptr))
				 continue;
			  break;
			case SPY_LEV:
			  if (!SendSpyNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			case FLOOD_LEV:
			  if (!SendFloodNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			case SPAM_LEV:
			  if (!SendSpamNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			case DEBUG_LEV:
			  if (!SendDebugNotice(cptr) || !IsAnOper(cptr))
				 continue;
			  break;
			  
			default:		/* this is stupid, but oh well */
			  if (!SendServNotice(cptr))
				 continue;
		  }
		  (void) ircsprintf(nbuf, ":%s NOTICE %s :*** Notice -- ",
								  me.name, cptr->name);
		  (void) strncat(nbuf, pattern,
							  sizeof(nbuf) - strlen(nbuf));
		  vsendto_one(cptr, nbuf, vl);
	  }
	va_end(vl);
	return;
}				
/*
 * sendto_ops
 * 
 * Send to *local* ops only.
 */
void sendto_ops(char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	va_list vl;
	
	va_start(vl, pattern);
   for (i = 0; i <= highest_fd; i++)
	  if ((cptr = local[i]) && !IsServer(cptr) && !IsMe(cptr) &&
			IsAnOper(cptr) && SendServNotice(cptr)) {
		  (void) ircsprintf(nbuf, ":%s NOTICE %s :*** Notice -- ",
								  me.name, cptr->name);
		  (void) strncat(nbuf, pattern,
							  sizeof(nbuf) - strlen(nbuf));
		  vsendto_one(cptr, nbuf, vl);
	  }
	va_end(vl);
   return;
}

/*
 * * sendto_ops_butone *      Send message to all operators. * one -
 * client not to send message to * from- client which message is from
 * *NEVER* NULL!!
 */
void sendto_ops_butone(aClient *one, aClient *from, char *pattern, ...)
{
	int     i;
	aClient *cptr;
   va_list vl;
	   
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cptr = client; cptr; cptr = cptr->next) {
      if (!SendWallops(cptr))
		  continue;
      /*
       * we want wallops if (MyClient(cptr) && !(IsServer(from) ||
       * IsMe(from))) continue;
       */
      i = cptr->from->fd;	/*
									 * find connection oper is on 
									 */
      if (sentalong[i])		/*
									 * sent message along it already ? 
									 */
		  continue;
      if (cptr->from == one)
		  continue;		/*
							 * ...was the one I should skip 
							 */
      sentalong[i] = 1;
      vsendto_prefix_one(cptr->from, from, pattern, vl);
   }
	va_end(vl);
   return;
}
/*
 * * sendto_wallops_butone *      Send message to all operators. * one
 * - client not to send message to * from- client which message is from
 * *NEVER* NULL!!
 */
void sendto_wallops_butone(aClient *one, aClient *from, char *pattern, ...)
{
	int     i;
	aClient *cptr;
   va_list vl;
	
	va_start(vl, pattern);
   for(i=0;i<=highest_fd;i++) {
      if((cptr=local[i])!=NULL) {
	 if(!(IsRegistered(cptr) && (SendWallops(cptr) || IsServer(cptr))) || cptr==one)
	    continue;
         vsendto_prefix_one(cptr, from, pattern, vl);
      }
   }
	va_end(vl);
   return;
}

void send_globops(char *pattern, ...)
{
	aClient    *cptr;
	int         i;
	char        nbuf[1024];
   va_list vl;
	
	va_start(vl, pattern);
   for (i = 0; i <= highest_fd; i++)
	  if ((cptr = local[i]) && !IsServer(cptr) && IsAnOper(cptr) &&
			!IsMe(cptr) && SendGlobops(cptr)) {
		  (void) sprintf(nbuf, ":%s NOTICE %s :*** Global -- ",
							  me.name, cptr->name);
		  (void) strncat(nbuf, pattern,
							  sizeof(nbuf) - strlen(nbuf));
		  vsendto_one(cptr, nbuf, vl);
	  }
	va_end(vl);
   return;
}
void send_chatops(char *pattern, ...)
{
	aClient    *cptr;
	int         i;
	char        nbuf[1024];
	va_list vl;
	
	va_start(vl, pattern);
   for (i = 0; i <= highest_fd; i++)
	  if ((cptr = local[i]) && !IsServer(cptr) && IsAnOper(cptr) &&
			!IsMe(cptr) && SendChatops(cptr)) {
		  (void) sprintf(nbuf, ":%s NOTICE %s :*** ChatOps -- ",
							  me.name, cptr->name);
		  (void) strncat(nbuf, pattern,
							  sizeof(nbuf) - strlen(nbuf));
		  vsendto_one(cptr, nbuf, vl);
	  }
	va_end(vl);
   return;
}
/*
 * to - destination client from - client which message is from
 * 
 * NOTE: NEITHER OF THESE SHOULD *EVER* BE NULL!! -avalon
 */
void sendto_prefix_one(aClient *to, aClient *from, char *pattern, ...)
{
	static char sender[HOSTLEN + NICKLEN + USERLEN + 5];
	anUser *user;
	char       *par;
	static char temp[1024];
	int         flag = 0;
   va_list vl;
	va_list vl2;
	va_start(vl, pattern);
	vl2=vl;
	
   par = va_arg(vl2, char *);
   /*
    * Optimize by checking if (from && to) before everything 
    */
   if (to && from) {
      if (!MyClient(from) && IsPerson(to) && (to->from == from->from)) {
			if (IsServer(from)) {
				(void) ircvsprintf(temp, pattern, vl);
				sendto_ops("Send message (%s) to %s[%s] dropped from %s(Fake Dir)", temp,
							  to->name, to->from->name, from->name);
				return;
			}
			sendto_ops("Ghosted: %s[%s@%s] from %s[%s@%s] (%s)",
						  to->name, to->user->username, to->user->host,
						  from->name, from->user->username, from->user->host,
						  to->from->name);
			sendto_serv_butone(NULL, ":%s KILL %s :%s (%s[%s@%s] Ghosted %s)",
									 me.name, to->name, me.name, to->name,
									 to->user->username, to->user->host, to->from->name);
			to->flags |= FLAGS_KILLED;
			(void) exit_client(NULL, to, &me, "Ghosted client");
			if (IsPerson(from))
			  sendto_one(from, err_str(ERR_GHOSTEDCLIENT),
							 me.name, from->name, to->name, to->user->username,
							 to->user->host, to->from);
			return;
      }
      if (MyClient(to) && IsPerson(from) && !mycmp(par, from->name)) {
			user = from->user;
			(void) strcpy(sender, from->name);
			if (user) {
				if (*user->username) {
					(void) strcat(sender, "!");
					(void) strcat(sender, user->username);
				}
				if (*user->host && !MyConnect(from)) {
					(void) strcat(sender, "@");
					(void) strcat(sender, user->host);
					flag = 1;
				}
			}
			/*
			 * * flag is used instead of index(sender, '@') for speed and *
			 * also since username/nick may have had a '@' in them.
			 * -avalon
			 */
			if (!flag && MyConnect(from) && *user->host) {
				(void) strcat(sender, "@");
				(void) strcat(sender, from->sockhost);
			}
			par = sender;
      }
   }				/*
					 * if (from && to) 
					 */
	/* okay, we more or less know that our sendto_prefix crap is going to be :%s <blah>,
	 * so it's easy to fix these lame problems...joy */
	ircsprintf(temp, ":%s%s", par, pattern+3);
	vsendto_one(to, temp, vl2);
	va_end(vl);
}
void vsendto_prefix_one(aClient *to, aClient *from, char *pattern, va_list vl)
{
	static char sender[HOSTLEN + NICKLEN + USERLEN + 5];
	anUser *user;
	char       *par;
	static char temp[1024];
	int         flag = 0;
	va_list vl2;
	vl2=vl;
	
   par = va_arg(vl2, char *);
   /*
    * Optimize by checking if (from && to) before everything 
    */
   if (to && from) {
      if (!MyClient(from) && IsPerson(to) && (to->from == from->from)) {
			if (IsServer(from)) {
				(void) ircvsprintf(temp, pattern, vl);
				sendto_ops("Send message (%s) to %s[%s] dropped from %s(Fake Dir)", temp,
							  to->name, to->from->name, from->name);
				return;
			}
			sendto_ops("Ghosted: %s[%s@%s] from %s[%s@%s] (%s)",
						  to->name, to->user->username, to->user->host,
						  from->name, from->user->username, from->user->host,
						  to->from->name);
			sendto_serv_butone(NULL, ":%s KILL %s :%s (%s[%s@%s] Ghosted %s)",
									 me.name, to->name, me.name, to->name,
									 to->user->username, to->user->host, to->from->name);
			to->flags |= FLAGS_KILLED;
			(void) exit_client(NULL, to, &me, "Ghosted client");
			if (IsPerson(from))
			  sendto_one(from, err_str(ERR_GHOSTEDCLIENT),
							 me.name, from->name, to->name, to->user->username,
							 to->user->host, to->from);
			return;
      }
      if (MyClient(to) && IsPerson(from) && !mycmp(par, from->name)) {
			user = from->user;
			(void) strcpy(sender, from->name);
			if (user) {
				if (*user->username) {
					(void) strcat(sender, "!");
					(void) strcat(sender, user->username);
				}
				if (*user->host && !MyConnect(from)) {
					(void) strcat(sender, "@");
					(void) strcat(sender, user->host);
					flag = 1;
				}
			}
			/*
			 * * flag is used instead of index(sender, '@') for speed and *
			 * also since username/nick may have had a '@' in them.
			 * -avalon
			 */
			if (!flag && MyConnect(from) && *user->host) {
				(void) strcat(sender, "@");
				(void) strcat(sender, from->sockhost);
			}
			par = sender;
      }
   }				/*
					 * if (from && to) 
					 */
	/* okay, we more or less know that our sendto_prefix crap is going to be :%s <blah>,
	 * so it's easy to fix these lame problems...joy */
	ircsprintf(temp, ":%s%s", par, pattern+3);
	vsendto_one(to, temp, vl2);
}

void sendto_fdlist(fdlist *listp, char *pattern, ...)
{
	int len, j, fd;
	va_list vl;

	va_start(vl, pattern);
   len = ircvsprintf(sendbuf, pattern, vl);
	
   for (fd = listp->entry[j = 1]; j <= listp->last_entry; fd = listp->entry[++j])
	  send_message(local[fd], sendbuf, len);
	va_end(vl);
}

void vsendto_fdlist(fdlist *listp, char *pattern, va_list vl)
{
	int len, j, fd;
   len = ircvsprintf(sendbuf, pattern, vl);
	
   for (fd = listp->entry[j = 1]; j <= listp->last_entry; fd = listp->entry[++j])
	  send_message(local[fd], sendbuf, len);
}


/*
 * sendto_realops
 * 
 * Send to *local* ops only but NOT +s nonopers.
 * If it's to local ops only and not +s nonopers, then SendServNotice is
 * wrong. Changed to IsAnOper. -mjs
 */
void sendto_realops(char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	fdlist     *l;
	int         fd;
	va_list vl;
	  
	va_start(vl, pattern);
   l = &oper_fdlist;
   for (fd = l->entry[i = 1]; i <= l->last_entry; fd = l->entry[++i]) {
      if (!(cptr = local[fd]))
		  continue;
      if (IsAnOper(cptr)) {
			(void) ircsprintf(nbuf, ":%s NOTICE %s :*** Notice -- %s",
									me.name, cptr->name, pattern);
			vsendto_one(cptr, nbuf, vl);
      }
   }
	va_end(vl);
   return;
}

void vsendto_realops(char *pattern, va_list vl)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	fdlist     *l;
	int         fd;

   l = &oper_fdlist;
   for (fd = l->entry[i = 1]; i <= l->last_entry; fd = l->entry[++i]) {
      if (!(cptr = local[fd]))
		  continue;
      if (IsAnOper(cptr)) {
			(void) ircsprintf(nbuf, ":%s NOTICE %s :*** Notice -- %s",
									me.name, cptr->name, pattern);
			vsendto_one(cptr, nbuf, vl);
      }
   }
   return;
}

/*
 * sendto_realops_lev
 * 
 * Send to *local* ops only but NOT +s nonopers at a certain level
 */
void sendto_realops_lev(int lev, char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	fdlist     *l;
	int         fd;
	va_list vl;
	
   l = &oper_fdlist;
	va_start(vl, pattern);
   for (fd = l->entry[i = 1]; i <= l->last_entry; fd = l->entry[++i]) {
      if (!(cptr = local[fd]))
		  continue;
      switch (lev) {
		 case CCONN_LEV:
			if (!SendCConnNotice(cptr))
			  continue;
			break;
		 case REJ_LEV:
			if (!SendRejNotice(cptr))
			  continue;
			break;
		 case SKILL_LEV:	/*
								 * This should not be sent, since this
								 * * can go to normal people 
								 */
			if (!SendSkillNotice(cptr))
			  continue;
			break;
		 case SPY_LEV:
			if (!SendSpyNotice(cptr))
			  continue;
			break;
		 case FLOOD_LEV:
			if (!SendFloodNotice(cptr))
			  continue;
			break;
		 case SPAM_LEV:
			if (!SendSpamNotice(cptr))
			  continue;
			break;
		 case DEBUG_LEV:
			if (!SendDebugNotice(cptr))
			  continue;
			break;
      }
      (void) ircsprintf(nbuf, ":%s NOTICE %s :*** Notice -- ",
								me.name, cptr->name);
      (void) strncat(nbuf, pattern,
							sizeof(nbuf) - strlen(nbuf));
      vsendto_one(cptr, nbuf, vl);
   }
	va_end(vl);
   return;
}

/*
 * * ts_warn *      Call sendto_ops, with some flood checking (at most
 * 5 warnings *      every 5 seconds)
 */

void ts_warn(char * pattern, ...)
{
	static ts_val last = 0;
	static int  warnings = 0;
	ts_val now;
	va_list vl;
	
	va_start(vl, pattern);
   /*
    * * if we're running with TS_WARNINGS enabled and someone does *
    * something silly like (remotely) connecting a nonTS server, *
    * we'll get a ton of warnings, so we make sure we don't send * more
    * than 5 every 5 seconds.  -orabidoo
    */
   /*
    * th+hybrid servers always do TS_WARNINGS -Dianora
    */
   now = time(NULL);
   if (now - last < 5) {
      if (++warnings > 5)
	 return;
   }
   else {
      last = now;
      warnings = 0;
   }

   vsendto_realops(pattern, vl);
	va_end(vl);
   return;
}
/*
 * sendto_locops
 */
void sendto_locops(char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	fdlist     *l;
	int         fd;
	va_list vl;
	
	va_start(vl, pattern);
   l = &oper_fdlist;
   for (fd = l->entry[i = 1]; i <= l->last_entry; fd = l->entry[++i]) {
      if (!(cptr = local[fd]))
		  continue;
      if (SendGlobops(cptr)) {
			(void) ircsprintf(nbuf, ":%s NOTICE %s :*** LocOps -- ",
									me.name, cptr->name);
			(void) strncat(nbuf, pattern,
								sizeof(nbuf) - strlen(nbuf));
			vsendto_one(cptr, nbuf, vl);
      }
   }
   va_end(vl);
   return;
}
/*
 * sendto_gnotice - send a routing notice to all local +n users.
 */
void sendto_gnotice(char *pattern, ...)
{
	aClient *cptr;
	int     i;
	char        nbuf[1024];
	va_list vl;
	
	va_start(vl, pattern);

   for (i = 0; i <= highest_fd; i++) {
      if ((cptr = local[i]) && !IsServer(cptr) && !IsMe(cptr) && SendRnotice(cptr)) {
 			
			(void) ircsprintf(nbuf, ":%s NOTICE %s :*** Routing -- ",
									me.name, cptr->name);
			(void) strncat(nbuf, pattern,
								sizeof(nbuf) - strlen(nbuf));
			vsendto_one(cptr, nbuf, vl);
      }
   }
   va_end(vl);
   return;
}

/*
 * sendto_channelops_butone
 *   Send a message to all OPs in channel chptr that
 *   are directly on this server and sends the message
 *   on to the next server if it has any OPs.
 */
void sendto_channelops_butone(aClient *one, aClient *from, aChannel *chptr, 
										char *pattern, ...)
{
	chanMember   *cm;
	aClient *acptr;
	int     i;
	va_list vl;
	
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cm = chptr->members; cm; cm = cm->next) {
      acptr = cm->cptr;
      if (acptr->from == one ||
	  !(cm->flags & CHFL_CHANOP))
	 continue;
      i = acptr->from->fd;
      if (MyConnect(acptr) && IsRegisteredUser(acptr)) {
			vsendto_prefix_one(acptr, from, pattern, vl);
			sentalong[i] = 1;
      }
      else {
			/*
			 * Now check whether a message has been sent to this
			 * *      * remote link already 
			 */
			if (sentalong[i] == 0) {
				vsendto_prefix_one(acptr, from, pattern, vl);

				sentalong[i] = 1;
			}
      }
   }
	va_end(vl);
   return;
}
/*
 * sendto_channelvoice_butone
 *   Send a message to all voiced users in channel chptr that
 *   are directly on this server and sends the message
 *   on to the next server if it has any voiced users.
 */
void sendto_channelvoice_butone(aClient *one, aClient *from, aChannel *chptr, 
										  char *pattern, ...)
{
	chanMember   *cm;
	aClient *acptr;
	int     i;
	va_list vl;
	
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cm = chptr->members; cm; cm = cm->next) {
      acptr = cm->cptr;
      if (acptr->from == one ||
			 !(cm->flags & CHFL_VOICE))
		  continue;
      i = acptr->from->fd;
      if (MyConnect(acptr) && IsRegisteredUser(acptr)) {
			vsendto_prefix_one(acptr, from, pattern, vl);
			sentalong[i] = 1;
      }
      else {
			/*
			 * Now check whether a message has been sent to this
			 * *      * remote link already 
			 */
			if (sentalong[i] == 0) {
				vsendto_prefix_one(acptr, from, pattern, vl);
				sentalong[i] = 1;
			}
      }
   }
	va_end(vl);
   return;
}
/*
 * sendto_channelvoiceops_butone
 *   Send a message to all OPs or voiced users in channel chptr that
 *   are directly on this server and sends the message
 *   on to the next server if it has any OPs or voiced users.
 */
void sendto_channelvoiceops_butone(aClient *one, aClient *from, aChannel 
											  *chptr, char *pattern, ...)
{
	chanMember   *cm;
	aClient *acptr;
	int     i;
	va_list vl;
	
	va_start(vl, pattern);
   memset((char *) sentalong, '\0', sizeof(sentalong));
   for (cm = chptr->members; cm; cm = cm->next) {
      acptr = cm->cptr;
      if (acptr->from == one ||
			 !((cm->flags & CHFL_VOICE) || (cm->flags & CHFL_CHANOP)))
		  continue;
      i = acptr->from->fd;
      if (MyConnect(acptr) && IsRegisteredUser(acptr)) {
			vsendto_prefix_one(acptr, from, pattern, vl);
			sentalong[i] = 1;
      }
      else {
			/*
			 * Now check whether a message has been sent to this
			 * *      * remote link already 
			 */
			if (sentalong[i] == 0) {
				vsendto_prefix_one(acptr, from, pattern, vl);
				sentalong[i] = 1;
			}
      }
   }
   return;
}

/*
 * * flush_fdlist_connections
 */

void
flush_fdlist_connections(listp)
     fdlist     *listp;
{
	int     i, fd;
	aClient *cptr;
	
   for (fd = listp->entry[i = 1]; i <= listp->last_entry;
		  fd = listp->entry[++i])
	  if ((cptr = local[fd]) && DBufLength(&cptr->sendQ) > 0)
		 (void) send_queued(cptr);
}
