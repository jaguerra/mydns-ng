/**************************************************************************************************
	$Id: task.c,v 1.86 2006/01/18 20:50:39 bboy Exp $

	Copyright (C) 2002-2005  Don Moore <bboy@bboy.net>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at Your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**************************************************************************************************/

#include "named.h"

/* Make this nonzero to enable debugging for this source file */
#define	DEBUG_TASK	1


extern void server_status(void);
extern void named_cleanup(int);

static uint16_t		internal_id = 0;
static uint8_t		*taskvec = NULL;

char *
task_exec_name(taskexec_t rv) {
  switch(rv) {

  case TASK_ABANDONED:			return "Task Abandoned";
  case TASK_FAILED:			return "Task Failed";

  case TASK_COMPLETED:			return "Task Completed";
  case TASK_FINISHED:			return "Task Finished";
  case TASK_TIMED_OUT:			return "Task Timed Out";

  case TASK_EXECUTED:			return "Task Executed";
  case TASK_DID_NOT_EXECUTE:		return "Task did not execute";
  case TASK_CONTINUE:			return "Task Continue";

  default:
    {
      static char msg[32];
      snprintf(msg, sizeof(msg), "Task Exec Code %d", rv);
      return msg;
    }
  }
  /*NOTREACHED*/
}

char *
task_type_name(int type) {
  switch (type) {

  case NORMAL_TASK:			return "Normal Task";
  case IO_TASK:				return "IO Driven Task";
  case PERIODIC_TASK:			return "Clock Driven Task";

  default:
    {
      static char msg[32];
      snprintf(msg, sizeof(msg), "Task Type %d", type);
      return msg;
    }
  }
  /*NOTREACHED*/
}

char *
task_priority_name(int priority) {
  switch (priority) {

  case HIGH_PRIORITY_TASK:		return "High Priority";
  case NORMAL_PRIORITY_TASK:		return "Normal Priority";
  case LOW_PRIORITY_TASK:		return "Low Priority";

  default:
    {
      static char msg[32];
      snprintf(msg, sizeof(msg), "Task Priority %d", priority);
      return msg;
    }
  }
  /*NOTREACHED*/
}

char *
task_status_name(TASK *t) {

  switch (t->status) {

  case NEED_READ:			return "NEED_READ";
  case NEED_IXFR:			return "NEED_IXFR";
  case NEED_ANSWER:			return "NEED_ANSWER";
  case NEED_WRITE:			return "NEED_WRITE";

  case NEED_RECURSIVE_FWD_CONNECT:	return "NEED_RECURSIVE_FWD_CONNECT";
  case NEED_RECURSIVE_FWD_CONNECTING:	return "NEED_RECURSIVE_FWD_CONNECTING";
  case NEED_RECURSIVE_FWD_WRITE:	return "NEED_RECURSIVE_FWD_WRITE";
  case NEED_RECURSIVE_FWD_RETRY:	return "NEED_RECURSIVE_FWD_RETRY";
  case NEED_RECURSIVE_FWD_READ:		return "NEED_RECURSIVE_FWD_READ";

  case NEED_NOTIFY_READ:		return "NEED_NOTIFY_READ";
  case NEED_NOTIFY_WRITE:		return "NEED_NOTIFY_WRITE";
  case NEED_NOTIFY_RETRY:		return "NEED_NOTIFY_RETRY";

  case NEED_TASK_RUN:			return "NEED_TASK_RUN";
  case NEED_AXFR:			return "NEED_AXFR";
  case NEED_TASK_READ:			return "NEED_TASK_READ";

  case NEED_COMMAND_READ:		return "NEED_COMMAND_READ";
  case NEED_COMMAND_WRITE:		return "NEED_COMMAND_WRITE";

  default:
    {
      static char msg[32];
      snprintf(msg, sizeof(msg), "Task Status %X", t->status);
      return msg;
    }
  }
  /*NOTREACHED*/
}

TASK *
task_find_by_id(TASK *t, QUEUE *TaskQ, unsigned long id) {
  TASK * ThisT;

  for (ThisT = TaskQ->head; ThisT ; ThisT = ThisT->next) {
    if (ThisT->internal_id == id) return ThisT;
  }
#if DEBUG_ENABLED && DEBUG_TASK
  Debug("%s: task_find_by_id(%s, %ld) cannot find task on queue", desctask(t), TaskQ->queuename, id);
#endif
  return NULL;
}

/**************************************************************************************************
	TASK_NEW
	Given a request (TCP or UDP), populates task structure.
	'socktype' should be either SOCK_STREAM or SOCK_DGRAM.
	Returns 0 on success, -1 on error, TASK_ABANDONED if the task is now invalid.
**************************************************************************************************/
taskexec_t
task_new(TASK *t, unsigned char *data, size_t len) {
  unsigned char *qname, *src, *qdtop;
  task_error_t errcode;

#if DEBUG_ENABLED && DEBUG_TASK
  Debug("task_new(%p, %p, %u)", t, data, len);
#endif

  /* Query needs to at least contain a proper header */
  if (len < DNS_HEADERSIZE)
    return formerr(t, DNS_RCODE_FORMERR, ERR_MALFORMED_REQUEST,
		   _("query so small it has no header"));

  /* Refuse queries that are too long */
  if (len > ((t->protocol == SOCK_STREAM) ? DNS_MAXPACKETLEN_TCP : DNS_MAXPACKETLEN_UDP)) {
    Warnx("%s: FORMERR in query - too large", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_MALFORMED_REQUEST, _("query too large"));
  }

  /* Parse query header data */
  src = data;
  DNS_GET16(t->id, src);
  memcpy(&t->hdr, src, SIZE16); src += SIZE16;
  DNS_GET16(t->qdcount, src);
  DNS_GET16(t->ancount, src);
  DNS_GET16(t->nscount, src);
  DNS_GET16(t->arcount, src);

  /*
   * Discard queries where the response bit is set;
   * it might be a spoofed packet
   *  asking us to talk to ourselves
   */
  if (t->hdr.qr) {
#if DEBUG_ENABLED && DEBUG_TASK
    Debug("%s: task_new(): %s %s %s",
	  desctask(t), mydns_rcode_str(DNS_RCODE_FORMERR),
	  err_reason_str(t, ERR_RESPONSE_BIT_SET),
	  _("response bit set on query"));
#endif
    return (TASK_ABANDONED);
  }

#if DEBUG_ENABLED && DEBUG_TASK
  Debug("%s: id=%u qr=%u opcode=%s aa=%u tc=%u rd=%u ra=%u z=%u rcode=%u", desctask(t),
	t->id, t->hdr.qr, mydns_opcode_str(t->hdr.opcode),
	t->hdr.aa, t->hdr.tc, t->hdr.rd, t->hdr.ra, t->hdr.z, t->hdr.rcode);
  Debug("%s: qd=%u an=%u ns=%u ar=%u", desctask(t),
	t->qdcount, t->ancount, t->nscount, t->arcount);
#endif

  task_init_header(t);					/* Initialize header fields for reply */

  t->qdlen = len - DNS_HEADERSIZE;			/* Fill in question data */
  if (t->qdlen <= 0) {
    Warnx("%s: FORMERR in query - zero length", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_MALFORMED_REQUEST, _("question has zero length"));
  }

  if (!(t->qd = malloc(t->qdlen)))
    Err(_("out of memory"));

  memcpy(t->qd, src, t->qdlen);
  qdtop = src;

  /* Get query name */
  if (!(qname = (unsigned char*)name_unencode2((char*)t->qd, t->qdlen, (char**)&src, &errcode))) {
    Warnx("%s: FORMERR in query decoding name", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, errcode, NULL);
  }
  strncpy(t->qname, (char*)qname, sizeof(t->qname)-1);
  Free(qname);

  /* Now we have question data, so initialize encoding */
  if (reply_init(t) < 0) {
    Warnx("%s: %s", desctask(t), _("failed to initialize reply"));
    return (TASK_FAILED);
  }

  /* Get query type */
  if (src + SIZE16 > data + len) {
    Warnx("%s: FORMERR in query - too short no qtype", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_MALFORMED_REQUEST, _("query too short; no qtype"));
  }

  DNS_GET16(t->qtype, src);

  /* If this request is TCP and TCP is disabled, refuse the request */
  if (t->protocol == SOCK_STREAM && !tcp_enabled && (t->qtype != DNS_QTYPE_AXFR || !axfr_enabled)) {
    Warnx("%s: REFUSED query - TCP not enabled", desctask(t));
    return formerr(t, DNS_RCODE_REFUSED, ERR_TCP_NOT_ENABLED, NULL);
  }

  /* Get query class */
  if (src + SIZE16 > data + len) {
    Warnx("%s: FORMERR in query - too short no qclass", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_MALFORMED_REQUEST, _("query too short; no qclass"));
  }

  DNS_GET16(t->qclass, src);

  t->qdlen = src - qdtop;

  /* Request must have at least one question */
  if (!t->qdcount) {
    Warnx("%s: FORMERR in query - no questions", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_NO_QUESTION, _("query contains no questions"));
  }

  /* Server can't handle more than 1 question per packet */
  if (t->qdcount > 1) {
    Warnx("%s: FORMERR in query - more than one question", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_MULTI_QUESTIONS,
		   _("query contains more than one question"));
  }

  /* Server won't accept truncated query */
  if (t->hdr.tc) {
    Warnx("%s: FORMERR in query - truncated query", desctask(t));
    return formerr(t, DNS_RCODE_FORMERR, ERR_QUESTION_TRUNCATED, _("query is truncated"));
  }

  /*
   * If this is an UPDATE operation, save a copy of the whole query for later parsing
   * If this is a QUERY operation and the query is an IXFR,
   * save a copy of the whole query for later parsing
   */
  if (!t->query) {
    if ((t->hdr.opcode == DNS_OPCODE_UPDATE)
	|| (t->hdr.opcode == DNS_OPCODE_QUERY && t->qtype == DNS_QTYPE_IXFR)) {
      t->len = len;
      if (!(t->query = malloc(t->len)))
	Err(_("out of memory"));
      memcpy(t->query, data, t->len);
    }
  }

  /* If DNS updates are enabled and the opcode is UPDATE, do the update */
  if (dns_update_enabled && t->hdr.opcode == DNS_OPCODE_UPDATE)
    return (dns_update(t));

  /* Handle Notify messages - currently do nothing so return not implemented */
  if (t->hdr.opcode == DNS_OPCODE_NOTIFY)
    return formerr(t, DNS_RCODE_NOTIMP, ERR_UNSUPPORTED_OPCODE, NULL);

  /* Server only handles QUERY opcode */
  if (t->hdr.opcode != DNS_OPCODE_QUERY) {
    Warnx("%s: FORMERR in query - not a supported opcode", desctask(t));
    return formerr(t, DNS_RCODE_NOTIMP, ERR_UNSUPPORTED_OPCODE, NULL);
  }

  /* Check class (only IN or ANY are allowed unless status is enabled) */
  if ((t->qclass != DNS_CLASS_IN) && (t->qclass != DNS_CLASS_ANY)
#if STATUS_ENABLED
      && (t->qclass != DNS_CLASS_CHAOS)
#endif
      ) {
    Warnx("%s: NOTIMP - qclass not availbale", desctask(t));
    return formerr(t, DNS_RCODE_NOTIMP, ERR_NO_CLASS, NULL);
  }

  if (t->qtype == DNS_QTYPE_IXFR && !dns_ixfr_enabled)
    return formerr(t, DNS_RCODE_REFUSED, ERR_IXFR_NOT_ENABLED, NULL);

  /* If AXFR is requested, it must be TCP, and AXFR must be enabled */
  if (t->qtype == DNS_QTYPE_AXFR && (!axfr_enabled || t->protocol != SOCK_STREAM))
    return formerr(t, DNS_RCODE_REFUSED, ERR_NO_AXFR, NULL);

  /* If this is AXFR, fork to handle it so that other requests don't block */
  if (t->protocol == SOCK_STREAM && t->qtype == DNS_QTYPE_AXFR) {
    task_change_type_and_priority(t, PERIODIC_TASK, NORMAL_PRIORITY_TASK);
    t->status = NEED_AXFR;
  } else if (t->qtype == DNS_QTYPE_IXFR) {
    t->status = NEED_IXFR;
    task_change_priority(t, NORMAL_PRIORITY_TASK);
  } else {
    t->status = NEED_ANSWER;
  }

  return TASK_CONTINUE;
}
/*--- task_new() --------------------------------------------------------------------------------*/


/**************************************************************************************************
	CLIENTADDR
	Given a task, returns the client's IP address in printable format.
**************************************************************************************************/
char *
clientaddr(TASK *t) {
  /* Needs to use dynamic memory and reallocate when required - in task structure? */
  static char buf[256];

  buf[0] = '\0';

#if HAVE_IPV6
  if (t->family == AF_INET6)
    inet_ntop(AF_INET6, &t->addr6.sin6_addr, buf, sizeof(buf) - 1);
  else
#endif
    if (t->family == AF_INET)
      inet_ntop(AF_INET, &t->addr4.sin_addr, buf, sizeof(buf) - 1);
    else
      return("Address Unknown");
  return (buf);
}
/*--- clientaddr() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	DESCTASK
	Describe a task; used by error/warning/debug output.
**************************************************************************************************/
char *
desctask(TASK *t)
{
	static char desc[1024];

	snprintf(desc, sizeof(desc), "%s: %s %s (%u) %s, %s %s",
		 clientaddr(t), mydns_qtype_str(t->qtype),
		 t->qname ? (char *)t->qname : "<NONE>",
		 t->internal_id, task_status_name(t), task_priority_name(t->priority),
		 task_type_name(t->type));
	return (desc);
}
/*--- desctask() --------------------------------------------------------------------------------*/


/**************************************************************************************************
	_TASK_INIT
	Allocates and initializes a new task, and returns a pointer to it.
	t = task_init(taskpriority, NEED_ZONE, fd, SOCK_DGRAM, AF_INET, &addr);
**************************************************************************************************/
TASK *
_task_init(
	   tasktype_t		type,			/* Type of task to create */
	   taskpriority_t	priority,		/* Priority of queue to use */
	   taskstat_t		status,			/* Initial status */
	   int 			fd,			/* Associated file descriptor for socket */
	   int			protocol,		/* Protocol (SOCK_DGRAM or SOCK_STREAM) */
	   int 			family,			/* Protocol (SOCK_DGRAM or SOCK_STREAM) */
	   void 		*addr,			/* Remote address */
	   const char		*file,
	   int			line
) {
  TASK				*new;
  QUEUE				**TaskQ = NULL;
  uint16_t			id;

  if (!taskvec) {
    taskvec = (uint8_t*)malloc(TASKVECSZ);
    TASKVEC_ZERO(taskvec);
  }

  if (!(new = calloc(1, sizeof(TASK))))
    Err(_("out of memory"));

  new->status = status;
  new->fd = fd;
  new->protocol = protocol;
  new->family = family;
  if (addr) {
    if (family == AF_INET) {
      memcpy(&new->addr4, addr, sizeof(struct sockaddr_in));
#if HAVE_IPV6
    } else if (family == AF_INET6) {
      memcpy(&new->addr6, addr, sizeof(struct sockaddr_in6));
#endif
    }
  }
  new->type = type;
  new->priority = priority;
  while(TASKVEC_ISSET(id = internal_id++, taskvec)) continue;
  TASKVEC_SET(id, taskvec);
  new->internal_id = id;
  new->timeout = current_time + task_timeout;
  new->minimum_ttl = DNS_MINIMUM_TTL;
  new->reply_cache_ok = 1;

  new->extension = NULL;
  new->freeextension = NULL;
  new->runextension = NULL;
  new->timeextension = NULL;

  TaskQ = &(TaskArray[type][priority]);

  if (enqueue(TaskQ, new) < 0) {
    task_free(new);
    return (NULL);
  }

  return (new);
}
/*--- _task_init() -------------------------------------------------------------------------------*/

void
_task_change_type(TASK *t, tasktype_t type, taskpriority_t priority) {
  requeue(&TaskArray[type][priority], t);
  t->type = type;
  t->priority = priority;
}

/**************************************************************************************************
	_TASK_FREE
	Free the memory used by a task.
**************************************************************************************************/
void
_task_free(TASK *t, const char *file, int line) {

  if (!t) return;

#if DEBUG_ENABLED && DEBUG_TASK
  Debug("%s: Freeing task at %s:%d", desctask(t), file, line);
#endif

  if (t->protocol == SOCK_STREAM) {
    close(t->fd);
  }

  if (t->extension && t->freeextension) {
    t->freeextension(t, t->extension);
  }
	
  Free(t->extension);
	  
#if DYNAMIC_NAMES
  {
    register int n;
    for (n = 0; n < t->numNames; n++)
      Free(t->Names[n]);
    if (t->numNames)
      Free(t->Names);
    Free(t->Offsets);
  }
#endif

  Free(t->query);
  Free(t->qd);
  rrlist_free(&t->an);
  rrlist_free(&t->ns);
  rrlist_free(&t->ar);
  Free(t->rdata);
  Free(t->reply);

  TASKVEC_CLR(t->id, taskvec);

  Free(t);

  if (answer_then_quit && (Status.udp_requests + Status.tcp_requests) >= answer_then_quit)
    named_cleanup(SIGQUIT);
}
/*--- _task_free() ------------------------------------------------------------------------------*/

void
task_add_extension(TASK *t, void *extension, FreeExtension freeextension, RunExtension runextension,
		   TimeExtension timeextension)
{
  if (t->extension && t->freeextension) {
    t->freeextension(t, t->extension);
  }
  Free(t->extension);

  t->extension = extension;
  t->freeextension = freeextension;
  t->runextension = runextension;
  t->timeextension = timeextension;
}

void
task_remove_extension(TASK *t)
{
  if (t->extension && t->freeextension) {
    t->freeextension(t, t->extension);
  }
  Free(t->extension);
  t->extension = NULL;
  t->freeextension = NULL;
  t->runextension = NULL;
  t->timeextension = NULL;
  return;
}

/**************************************************************************************************
	TASK_INIT_HEADER
	Sets and/or clears header fields and values as necessary.
**************************************************************************************************/
void
task_init_header(TASK *t)
{
	t->hdr.qr = 1;					/* This is the response, not the query */
	t->hdr.ra = forward_recursive;			/* Are recursive queries available? */
	t->hdr.rcode = DNS_RCODE_NOERROR;		/* Assume success unless told otherwise */
}
/*--- task_init_header() ------------------------------------------------------------------------*/


/**************************************************************************************************
	TASK_OUTPUT_INFO
**************************************************************************************************/
void
task_output_info(TASK *t, char *update_desc)
{
#if !DISABLE_DATE_LOGGING
	struct timeval tv;
	time_t tt;
	struct tm *tm;
	char datebuf[80];
#endif

	/* If we've already outputted the info for this (i.e. multiple DNS UPDATE requests), ignore */
	if (t->info_already_out)
		return;

	/* Don't output anything for TCP sockets in the process of closing */
	if (t->protocol == SOCK_STREAM && t->fd < 0)
		return;

#if !DISABLE_DATE_LOGGING
	gettimeofday(&tv, NULL);
	tt = tv.tv_sec;
	tm = localtime(&tt);

	strftime(datebuf, sizeof(datebuf)-1, "%d-%b-%Y %H:%M:%S", tm);
#endif

	Verbose(
#if !DISABLE_DATE_LOGGING
		"%s+%06lu "
#endif
		"#%u "
		"%u "		/* Client-provided ID */
		"%s "		/* TCP or UDP? */
		"%s "		/* Client IP */
		"%s "		/* Class */
		"%s "		/* Query type (A, MX, etc) */
		"%s " 		/* Name */
		"%s "		/* Return code (NOERROR, NXDOMAIN, etc) */
		"%s "		/* Reason */
		"%d "		/* Question section */
		"%d "		/* Answer section */
		"%d "		/* Authority section */
		"%d "		/* Additional section */
		"LOG "
		"%s "		/* Reply from cache? */
		"%s "		/* Opcode */
		"\"%s\""	/* UPDATE description (if any) */
		,
#if !DISABLE_DATE_LOGGING
		datebuf, tv.tv_usec,
#endif
		t->internal_id,
		t->id,
		(t->protocol == SOCK_STREAM)?"TCP"
		:(t->protocol == SOCK_DGRAM)?"UDP"
		:"UNKNOWN",
		clientaddr(t),
		mydns_class_str(t->qclass),
		mydns_qtype_str(t->qtype),
		t->qname,
		mydns_rcode_str(t->hdr.rcode),
		err_reason_str(t, t->reason),
		t->qdcount,
		t->an.size,
		t->ns.size,
		t->ar.size,
		(t->reply_from_cache ? "Y" : "N"),
		mydns_opcode_str(t->hdr.opcode),
		update_desc ? update_desc : ""
	);
}
/*--- task_output_info() ------------------------------------------------------------------------*/


/**************************************************************************************************
	TASK_TIMEDOUT
	Check for timed out tasks.
**************************************************************************************************/
taskexec_t
task_timedout(TASK *t) {
  taskexec_t res = TASK_FAILED;

  Status.timedout++;

  if (t->timeextension) {
    res = t->timeextension(t, t->extension);
    if (
	(res == TASK_DID_NOT_EXECUTE)
	|| (res == TASK_EXECUTED)
	|| (res == TASK_CONTINUE)
	) return res;
  }

  t->reason = ERR_TIMEOUT;
  t->hdr.rcode = DNS_RCODE_SERVFAIL;

  /* Close TCP connection */
  if (t->protocol == SOCK_STREAM)
    sockclose(t->fd);

  dequeue(t);

  return res;
}
/*--- task_timedout() --------------------------------------------------------------------*/

/**************************************************************************************************
	TASK_PROCESS
	Process the specified task, if possible.  Returns a pointer to the next task.
**************************************************************************************************/
static taskexec_t
task_process_query(TASK *t, fd_set *rfd, fd_set*wfd, fd_set *efd) {
  taskexec_t	res = TASK_DID_NOT_EXECUTE;

  switch (TASKIOTYPE(t->status)) {

  case Needs2Read:

    switch (t->status) {

    case NEED_READ:
      /*
      **  NEED_READ: Need to read query
      */
      if (FD_ISSET(t->fd, rfd) || FD_ISSET(t->fd, efd)) {
	switch (t->protocol) {

	case SOCK_DGRAM:
	  Warnx("%s: %s", desctask(t), _("invalid state for UDP query"));
	  return TASK_FAILED;

	case SOCK_STREAM:
	  res = read_tcp_query(t);
	  if (res == TASK_ABANDONED) {
	    sockclose(t->fd);
	    return TASK_FAILED;
	  }
	  if (!((res == TASK_FAILED) || (res == TASK_EXECUTED) || (res == TASK_CONTINUE))) {
	    Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from read_tcp_query"));
	    return TASK_FAILED;
	  }
	  return res;

	default:
	  Warnx("%s: %d: %s", desctask(t), t->protocol, _("unknown/unsupported protocol"));
	  return TASK_FAILED;
	}
      }
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  case Needs2Exec:

    switch (t->status) {

    case NEED_IXFR:

      if (reply_cache_find(t)) {
	char *dest = t->reply;
	DNS_PUT16(dest, t->id);
	DNS_PUT(dest, &t->hdr, SIZE16);
      } else {
	if ((res = ixfr(t, ANSWER, t->qtype, t->qname, 0)) > TASK_FAILED) {
	  build_reply(t, 0);
	  if (t->hdr.tc) {
	    abandon_reply(t);
	    if ((res = ixfr(t, ANSWER, t->qtype, t->qname, 1)) <= TASK_FAILED) goto IXFRFAILED;
	    build_reply(t, 0);
	  }
	  if (t->reply_cache_ok) add_reply_to_cache(t);
	}
      IXFRFAILED:
	;
      }
      t->status = NEED_WRITE;
      return TASK_CONTINUE;

    case NEED_ANSWER:
      /*
      **  NEED_ANSWER: Need to resolve query
      */
      if (reply_cache_find(t)) {
	char *dest = t->reply;
	DNS_PUT16(dest, t->id);						/* Query ID */
	DNS_PUT(dest, &t->hdr, SIZE16);					/* Header */
      } else {
	resolve(t, ANSWER, t->qtype, t->qname, 0);
	if (TaskIsRecursive(t->status & Needs2Recurse)) {
	  return TASK_CONTINUE;
	} else {
	  build_reply(t, 1);
	  if (t->reply_cache_ok)
	    add_reply_to_cache(t);
	}
      }
      t->status = NEED_WRITE;
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  case Needs2Write:

    switch (t->status) {

    case NEED_WRITE:
      /*
      **  NEED_WRITE: Need to write reply
      */
      if (FD_ISSET(t->fd, wfd) || FD_ISSET(t->fd, efd)) {
	switch (t->protocol) {

	case SOCK_DGRAM:
	  res = write_udp_reply(t);
	  if (res == TASK_EXECUTED) return TASK_EXECUTED;
	  if (res == TASK_CONTINUE) return TASK_CONTINUE;
	  if (res == TASK_FAILED) return TASK_FAILED;
	  if (res == TASK_COMPLETED) return TASK_COMPLETED;
	  Warnx("%s: %d: %s", desctask(t), (int)res,
		_("unexpected result from write_udp_reply"));
	  return TASK_FAILED;

	case SOCK_STREAM:
	  res = write_tcp_reply(t);
	  if (res == TASK_EXECUTED)  return TASK_EXECUTED;
	  if (res == TASK_ABANDONED) return TASK_ABANDONED;
	  if (res == TASK_COMPLETED) return TASK_COMPLETED;
	  if (res == TASK_CONTINUE) return TASK_CONTINUE;
	  Warnx("%s: %d: %s", desctask(t), (int)res,
		_("unexpected result from write_tcp_reply"));
	  return TASK_FAILED;

	default:
	  Warnx("%s: %d: %s", desctask(t), t->protocol, _("unknown/unsupported protocol"));
	  return TASK_FAILED;
	}
      }
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
  }
  return TASK_FAILED;
}

static taskexec_t
task_process_recursive(TASK *t, fd_set *rfd, fd_set*wfd, fd_set *efd) {
  taskexec_t	res = TASK_DID_NOT_EXECUTE;

  switch (TASKIOTYPE(t->status)) {

  case Needs2Connect:
	
    switch(t->status) {

    case NEED_RECURSIVE_FWD_CONNECT:
      /*
      **  NEED_RECURSIVE_FWD_CONNECT: Need to connnect to recursive forwarder
      */
      res = recursive_fwd_connect(t);
      if (res == TASK_FAILED) return TASK_FAILED;
      if (res != TASK_CONTINUE) {
	Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from recursive_fwd_connect"));
	return TASK_FAILED;
      }
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  case Needs2Write:

    switch (t->status) {

    case NEED_RECURSIVE_FWD_CONNECTING:
      res = recursive_fwd_connecting(t);
      if (res == TASK_FAILED) return TASK_FAILED;
      if (res == TASK_CONTINUE) return TASK_CONTINUE;
      Warnx("%s: %d: %s", desctask(t), (int)res,
	    _("unexpected result from recursive_fwd_connecting"));
      return TASK_FAILED;
	  
    case NEED_RECURSIVE_FWD_WRITE:
      /*
      **  NEED_RECURSIVE_FWD_WRITE: Need to write request to recursive forwarder
      */
      res = recursive_fwd_write(t);
      if (res == TASK_FAILED) return TASK_FAILED;
      if (res == TASK_CONTINUE) return TASK_CONTINUE;		/* means try again */
      Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from recursive_fwd_write"));
      return TASK_FAILED;
	  
    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  case Needs2Read:

    switch (t->status) {

    case NEED_RECURSIVE_FWD_READ:
      /*
      **  NEED_RECURSIVE_FWD_READ: Need to read reply from recursive forwarder
      */
      if(FD_ISSET(t->fd, rfd) || FD_ISSET(t->fd, efd)) {
	res = recursive_fwd_read(t);
	if (res == TASK_FAILED) return TASK_FAILED;	/* The master task just keeps running */
	if (res == TASK_COMPLETED) return TASK_COMPLETED;/* The master task has finished */
	if (res == TASK_CONTINUE) return TASK_CONTINUE;
	Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from recursive_fwd_read"));
	return TASK_FAILED;
      }
      return TASK_CONTINUE;
	    
    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;;
    }
    break;

  default:


    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;
  }
  return TASK_FAILED;
}

static taskexec_t
task_process_request(TASK *t, fd_set *rfd, fd_set*wfd, fd_set *efd) {
  taskexec_t	res = TASK_DID_NOT_EXECUTE;

  switch (TASKIOTYPE(t->status)) {

  case Needs2Write:

    switch (t->status) {

    case NEED_NOTIFY_WRITE:
      /*
      ** NEED_NOTIFY_WRITE: need to write to slaves
      */
      if (FD_ISSET(t->fd, wfd)) { /* && (t->timeout <= current_time)) {*/
	res = notify_write(t);
	if (res == TASK_COMPLETED) return TASK_COMPLETED;
	if( res == TASK_CONTINUE) return TASK_CONTINUE;
	Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from notify_run"));
	return TASK_FAILED;
      }
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  case Needs2Read:

    switch (t->status) {

    case NEED_NOTIFY_READ:
      /*
      ** NEED_NOTIFY_READ: need to read from a slave
      */
      if (FD_ISSET(t->fd, rfd)) {
	res = notify_read(t);
	if (res == TASK_COMPLETED) return TASK_COMPLETED;
	if( res == TASK_CONTINUE) return TASK_CONTINUE;
	Warnx("%s: %d: %s", desctask(t), (int)res, _("unexpected result from notify_run"));
	return TASK_FAILED;
      }
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;
    }
    break;

  default:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;
  }
  return TASK_FAILED;
}

static taskexec_t
task_process_ticktask(TASK *t, fd_set *rfd, fd_set*wfd, fd_set *efd) {

  switch (TASKIOTYPE(t->status)) {

  case Needs2Read:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;

  case Needs2Write:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;


  case Needs2Connect:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;

  case Needs2Exec:
    
    switch (t->status) {

    case NEED_RECURSIVE_FWD_RETRY:
      if (t->timeout <= current_time)
	t->status = NEED_RECURSIVE_FWD_WRITE;
      return TASK_CONTINUE;

    case NEED_NOTIFY_RETRY:
      if (t->timeout <= current_time)
	t->status = NEED_NOTIFY_WRITE;
      return TASK_CONTINUE;

    default:
      Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
      return TASK_FAILED;

    }

  default:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;

  }
  return TASK_FAILED;
}

static taskexec_t
task_process_runtask(TASK *t, fd_set *rfd, fd_set*wfd, fd_set *efd) {
  taskexec_t	res = TASK_DID_NOT_EXECUTE;

  switch (t->status) {

  case NEED_AXFR:
    axfr_fork(t);
    return TASK_COMPLETED;

  case NEED_COMMAND_WRITE:
    if (!(FD_ISSET(t->fd, wfd) || FD_ISSET(t->fd, efd))) return TASK_CONTINUE;
    break;

  case NEED_TASK_READ:
  case NEED_COMMAND_READ:
    if (!(FD_ISSET(t->fd, rfd) || FD_ISSET(t->fd, efd))) return TASK_CONTINUE;
    break;

  case NEED_TASK_RUN:
    break;

  default:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    return TASK_FAILED;

  }

  /* Call the run function */
  if (!t->runextension) return TASK_CONTINUE;

  res = t->runextension(t, t->extension);

  return res;
}

int
task_process(TASK *t, fd_set *rfd, fd_set *wfd, fd_set *efd) {
  taskexec_t	res = TASK_DID_NOT_EXECUTE;

  switch (TASKCLASS(t->status)) {

  case QueryTask:

    switch (TaskIsRecursive(t->status)) {

    default: /* Not a recursive query */

      res = task_process_query(t, rfd, wfd, efd);
      break;

    case Needs2Recurse:

      res = task_process_recursive(t, rfd, wfd, efd);
      break;

    }
    break;

  case ReqTask:

    res = task_process_request(t, rfd, wfd, efd);
    break;

  case TickTask:

    res = task_process_ticktask(t, rfd, wfd, efd);
    break;

  case RunTask:

    res = task_process_runtask(t, rfd, wfd, efd);
    break;

  default:
    Warnx("%s: %d %s", desctask(t), t->status, _("unrecognised task status"));
    res = TASK_ABANDONED;
    break;
  }

  switch(res) {

  case TASK_CONTINUE:
    break;

  case TASK_DID_NOT_EXECUTE:
  case TASK_EXECUTED:

    if (current_time > t->timeout)
      if ((res = task_timedout(t)) > TASK_TIMED_OUT) break;
    goto DEQUEUETASK;
      
  default:
    Warnx("%s: %d %s", desctask(t), res, _("unrecognised task exec state"));
    res = TASK_ABANDONED;

  case TASK_COMPLETED:
  case TASK_FINISHED:
  case TASK_FAILED:
  case TASK_ABANDONED:
  case TASK_TIMED_OUT:

  DEQUEUETASK:

#if DEBUG_ENABLED && DEBUG_TASK
    Debug("%s: dequeuing task because %s", desctask(t), task_exec_name(res));
#endif

    if ((res == TASK_ABANDONED) || (res == TASK_TIMED_OUT))
      sockclose(t->fd);

    dequeue(t);
    break;
  }

  return (res != TASK_DID_NOT_EXECUTE);
}
/*--- task_process() ----------------------------------------------------------------------------*/

static taskexec_t
check_all_tasks(TASK *mytask, void *mydata) {
  TASK *t, *nextt = NULL;
  int i, j;

  /* Reset my timeout so I do not get run again and I do not process myself ;-( */
  mytask->timeout = current_time + task_timeout;

  for (i = NORMAL_TASK; i <= PERIODIC_TASK; i++) {
    for (j = HIGH_PRIORITY_TASK; j <= LOW_PRIORITY_TASK; j++) {
      for (t = TaskArray[i][j]->head; t; t = nextt) {
	nextt = t->next;
	if (current_time > t->timeout)
	  task_timedout(t);
      }
    }
  }

  return (TASK_CONTINUE); /* Run continuously */
}

void
task_start() {

  TASK *inittask;

  /*
   * Allocate a housekeeping task that will check all of the queues for stuck tasks.
   */

  inittask = Ticktask_init(LOW_PRIORITY_TASK, NEED_TASK_RUN, -1, 0, AF_UNSPEC, NULL);
  task_add_extension(inittask, NULL, NULL, NULL, check_all_tasks);
  inittask->timeout = current_time + task_timeout;

}

/* vi:set ts=3: */
/* NEED_PO */
