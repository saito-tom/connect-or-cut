/* connect-or-cut -- block unwanted connect() calls.
 *
 * Copyright Ⓒ 2015, 2016, 2017  Thomas Girard <thomas.g.girard@free.fr>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 *  * Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <resolv.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <AvailabilityMacros.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 1070
#define MISSING_STRNDUP
#endif
#endif

#ifdef __SunOS_5_11
#undef MISSING_STRNDUP
#endif

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/param.h>
#if defined(BSD)
#define HAVE_GETPROGNAME
#endif
#endif

#if (defined(sun) || defined(__sun)) && (defined(__SVR4) || defined(__svr4__))
#define HAVE_GETEXECNAME
#include <libgen.h>
#endif

typedef enum coc_address_type
{
  COC_IPV4_ADDR = 1 << 0,	/* 1 */
  COC_IPV6_ADDR = 1 << 1,	/* 2 */
  COC_GLOB_ADDR = 1 << 2	/* 4 */
} coc_address_type_t;

#define COC_HOST_ADDR (1 << 3)  /* 8 */

typedef enum coc_log_level
{
  COC_SILENT_LOG_LEVEL = -1,	/* mapped from 0. */
  COC_ERROR_LOG_LEVEL = LOG_ERR,	/* mapped from 1 (to 3). */
  COC_BLOCK_LOG_LEVEL = LOG_WARNING,	/* mapped from 2 (to 4). */
  COC_ALLOW_LOG_LEVEL = LOG_INFO,	/* mapped from 3 (to 6). */
  COC_DEBUG_LOG_LEVEL = LOG_DEBUG	/* mapped from 4 (to 7). */
} coc_log_level_t;

typedef enum coc_log_target
{
  COC_STDERR_LOG = 1 << 0,	/* 1 */
  COC_SYSLOG_LOG = 1 << 1,	/* 2 */
  COC_FILE_LOG = 1 << 2		/* 4 */
} coc_log_target_t;

typedef enum coc_rule_type
{
  COC_ALLOW = 0,
  COC_BLOCK = 1
} coc_rule_type_t;

static const char *rule_type_name[] = {
  "ALLOW",
  "BLOCK"
};

static const char *address_type_name[] = {
  [COC_IPV4_ADDR] = "IPv4",
  [COC_IPV6_ADDR] = "IPv6",
  [COC_GLOB_ADDR] = "glob",
  [COC_HOST_ADDR] = "host"
};

typedef struct coc_entry {
  union {
    struct in_addr ipv4;
    struct in6_addr ipv6;
    char *glob;
  } addr;
  in_port_t port;
  coc_address_type_t addr_type;
  coc_rule_type_t rule_type;
  SLIST_ENTRY(coc_entry) entries;
} coc_entry_t;

static inline
coc_entry_t *
coc_entry_alloc (void)
{
  return (coc_entry_t *) malloc (sizeof(coc_entry_t));
}

SLIST_HEAD(coc_list, coc_entry) coc_list_head = { NULL };

#define COC_ALLOW_ENV_VAR_NAME "COC_ALLOW"
#define COC_BLOCK_ENV_VAR_NAME "COC_BLOCK"
#define COC_LOG_LEVEL_ENV_VAR_NAME "COC_LOG_LEVEL"
#define COC_LOG_PATH_ENV_VAR_NAME "COC_LOG_PATH"
#define COC_LOG_TARGET_ENV_VAR_NAME "COC_LOG_TARGET"
#if defined(__APPLE__) && defined(__MACH__)
#define COC_PRELOAD_ENV_VAR_NAME "DYLD_INSERT_LIBRARIES"
#else
#define COC_PRELOAD_ENV_VAR_NAME "LD_PRELOAD"
#endif

#if defined(HAVE_GETEXECNAME)
static inline const char *
getprogname ()
{
  const char *fullpath = getexecname ();
  return basename ((char *) fullpath);
}

#elif !defined(HAVE_GETPROGNAME)
extern char *__progname;

static inline const char *
getprogname ()
{
  return __progname;
}
#endif

static const char version[] = "connect-or-cut v1.0.2";
static volatile bool initialized = false;
static bool needs_dns_lookup = false;
static coc_log_level_t log_level = COC_BLOCK_LOG_LEVEL;
static coc_log_target_t log_target = COC_STDERR_LOG;
static char *log_file_name = NULL;

#ifndef __SUNPRO_C
static void
coc_log (coc_log_level_t level, const char *format, ...)
__attribute__ ((__format__ (__printf__, 2, 3)));
#endif

static void
coc_log (coc_log_level_t level, const char *format, ...)
{
  if (log_level >= level)
    {
      struct tm now_tm;
      time_t now;
      char buffer[sizeof ("mmm DD HH:MM:SS ")];
      time (&now);
      localtime_r (&now, &now_tm);
      strftime (buffer, sizeof (buffer), "%h %e %T ", &now_tm);

      if ((log_target & COC_FILE_LOG) == COC_FILE_LOG)
	{
	  va_list ap;
	  va_start (ap, format);
	  FILE *log_stream = fopen (log_file_name, "a");

	  if (!log_stream)
            {
              fprintf (stderr, "Unable to create log file %s; discarding file logs\n", log_file_name);
              log_target &= ~COC_FILE_LOG;
            }
          else
            {
	      fprintf (log_stream, "%s", buffer);
	      vfprintf (log_stream, format, ap);
	      fclose (log_stream);
	    }

	  va_end (ap);
	}

      if ((log_target & COC_STDERR_LOG) == COC_STDERR_LOG)
	{
	  va_list ap;
	  va_start (ap, format);
	  fprintf (stderr, "%s", buffer);
	  vfprintf (stderr, format, ap);
	  fflush (stderr);
	  va_end (ap);
	}

      if ((log_target & COC_SYSLOG_LOG) == COC_SYSLOG_LOG)
	{
	  va_list ap;
	  va_start (ap, format);
	  /*
	   * syslog will be automatically opened at first invocation.
	   *
	   * We do not change anything to this because:
	   *  - we know that syslog will not be used during initialization
	   *    of our library
	   *  - the program that will be invoked after our library is
	   *    preloaded can use syslog with non default settings. So
	   *    we reuse these in order not to interfere with the
	   *    program.
	   */
	  vsyslog (level, format, ap);
	  va_end (ap);
	}
    }
}

#define DIE(...) do { \
    coc_log (COC_ERROR_LOG_LEVEL,"ERROR " __VA_ARGS__); \
    exit (EXIT_FAILURE); \
  } while (0)

#ifdef MISSING_STRNDUP
static inline char *
strndup (const char *s, size_t n)
{
  size_t len = strlen (s);

  if (len > n)
    {
      len = n;
    }

  char *ns = (char *) malloc (len + 1);

  if (ns != NULL)
    {
      ns[len] = '\0';
      memcpy (ns, s, len);
    }

  return ns;
}
#endif

static int
coc_rule_add (const char *str, size_t len, size_t rule_type)
{
  int type = COC_IPV4_ADDR | COC_IPV6_ADDR | COC_GLOB_ADDR | COC_HOST_ADDR;
  const char *p = str;
  const char *service = NULL;
  enum
  {
    IPV6_SB_NONE,
    IPV6_SB_OPEN,
    IPV6_SB_CLOSE
  } sb = IPV6_SB_NONE;
  size_t colon_count = 0;
  size_t ipv4_segment = 1;
  uint16_t segment = 0;		/* IPv4 or IPv6 */

  while (p < str + len)
    {
      unsigned char c = *p;

      /* `*' allowed only for glob. Let's go on to check for errors. */
      if (c == '*')
	{
	  /* `*' not allowed if only remaining choices are IPv4 or IPv6. */
	  if (!(type & ~(COC_IPV4_ADDR | COC_IPV6_ADDR)))
	    {
	      DIE ("`*' not allowed for IPv4 or IPv6, aborting\n");
	    }
	  else
	    {
	      type = COC_GLOB_ADDR;
	    }
	}

      /* `[' allowed only at beginning of address for IPv6. */
      else if (c == '[')
	{
	  if (p == str)
	    {
	      type = COC_IPV6_ADDR;
	      sb = IPV6_SB_OPEN;
	    }
	  else
	    {
	      DIE ("`[' allowed only once for IPv6, aborting\n");
	    }
	}

      else if (c == ']')
	{
	  if (type == COC_IPV6_ADDR && sb == IPV6_SB_OPEN)
	    {
	      sb = IPV6_SB_CLOSE;
	    }
	  else
	    {
	      DIE ("`]' unexpected, aborting\n");
	    }
	}

      else if (c == ':')
	{
	  /* IPv6 uses `:' as a segment separator. So we count them.
	   * Validation for IPv6 is done later with inet_pton. */
	  colon_count++;
	  /* Assume `:' precedes port. We will check if not empty later. */
	  service = p + 1;

	  if (colon_count > 1)
	    {
	      if ((type & COC_IPV6_ADDR) == COC_IPV6_ADDR)
		{
		  type = COC_IPV6_ADDR;
		}

	      if (type != COC_IPV6_ADDR ||
		  ((sb == IPV6_SB_OPEN && colon_count > 7) ||
		   colon_count > 8))
		{
		  DIE ("Extra `:' unexpected, aborting\n");
		}

	      if (colon_count == 8 || sb == IPV6_SB_CLOSE)
		{
		  break;
		}
	      else
		{
		  service = NULL;
		}
	    }

	  else
	    {
	      /* If so far IPv4 was still a valid choice, then it is it now. */
	      if (p != str && ((type & COC_IPV4_ADDR) == COC_IPV4_ADDR))
		{
		  type = COC_IPV4_ADDR;

		  if (ipv4_segment == 4)
		    {
		      break;
		    }
		}
	    }
	}

      /* We can see dots for IPv4, glob, or hostnames.  We can rule
       * out IPv4 when we have a segment that does not fit what is
       * expected (e.g. higher than 255).
       */
      else if (c == '.')
	{
	  if (type == COC_IPV6_ADDR)
	    {
	      DIE ("`.' not allowed for IPv6, aborting\n");
	    }

	  type &= ~COC_IPV6_ADDR;

	  segment = 0;
	  ipv4_segment++;

	  if (ipv4_segment > 4)
	    {
	      if (type == COC_IPV4_ADDR)	/* not possible for now. */
		{
		  DIE ("Extra `.' unexpected, aborting\n");
		}
	      else
		{
		  type &= ~COC_IPV4_ADDR;
		}
	    }
	}

      else if (isdigit (c))
	{
	  if ((type & COC_IPV4_ADDR) == COC_IPV4_ADDR)
	    {
	      /* INT30-C */
	      if ((segment > UINT16_MAX / 10) ||
		  (UINT16_MAX - (c - '0') < segment * 10))
		{
		  DIE ("Invalid IPv4 segment, aborting\n");
		}
	      else
		{
		  segment = segment * 10 + (c - '0');
		}

	      if (segment > 255)
		{
		  if (type == COC_IPV4_ADDR)	/* not possible */
		    {
		      DIE ("Invalid IPv4 address, aborting\n");
		    }
		  else
		    {
		      coc_log (COC_DEBUG_LOG_LEVEL,
			       "DEBUG %hd... is not an IPv4 segment\n",
			       segment);
		      type &= ~COC_IPV4_ADDR;
		    }
		}
	    }
	}

      else if (isxdigit (c))	/* and not a digit */
	{
	  if (type == COC_IPV4_ADDR)	/* not possible */
	    {
	      DIE ("Invalid IPv4 address, aborting\n");
	    }
	  else
	    {
	      type &= ~COC_IPV4_ADDR;
	    }
	}

      else if (isalnum (c))	/* not a digit nor an xdigit */
	{
	  if (!(type & ~(COC_IPV4_ADDR | COC_IPV6_ADDR)))
	    {
	      DIE ("`%c' unexpected, aborting\n", c);
	    }
	  else
	    {
	      type &= ~(COC_IPV4_ADDR | COC_IPV6_ADDR);
	    }
	}

      else if (c == '-' || c == '_')
	{
	  if (!(type & ~(COC_IPV4_ADDR | COC_IPV6_ADDR)) || p == str)
	    {
	      DIE ("`%c' unexpected, aborting\n", c);
	    }
	  else
	    {
	      type &= ~(COC_IPV4_ADDR | COC_IPV6_ADDR);
	    }
	}

      else
	{
	  /* We may revisit this later for UTF-8 */
	  DIE ("`%c' unexpected here, aborting\n", c);
	}

      p++;
    }

  /* If so far IPv4 was still a valid choice, then it is it now. */
  if ((type & COC_IPV4_ADDR) == COC_IPV4_ADDR)
    {
      type = COC_IPV4_ADDR;
    }

  /* If so far host was still a valid choice, then it is it now. */
  else if ((type & COC_HOST_ADDR) == COC_HOST_ADDR)
    {
      type = COC_HOST_ADDR;
    }

  if (type == COC_IPV6_ADDR && sb == IPV6_SB_OPEN)
    {
      DIE ("`]' missing, aborting\n");
    }

  assert (type == COC_IPV4_ADDR ||
	  type == COC_IPV6_ADDR ||
	  type == COC_HOST_ADDR || type == COC_GLOB_ADDR);

  in_port_t port = 0;
  /* If we have a port here, check if everything after that port is valid. */
  if (service != NULL)
    {
      const char *t = service;
      bool getservbyname_needed = false;

      if (t == str + len)
	{
	  DIE ("No port specified after `:', aborting\n");
	}

      while (t < str + len)
	{
	  unsigned char u = *t;

	  if (isdigit (u))
	    {
	      /* INT30-C */
	      if ((port > UINT16_MAX / 10) ||
		  (UINT16_MAX - (u - '0') < port * 10))
		{
		  DIE ("Invalid port number, aborting\n");
		}
	      else
		{
		  port = port * 10 + (u - '0');
		}
	    }

	  else if (isalnum (u) || u == '-')
	    {
	      getservbyname_needed = true;
	    }

	  else
	    {
	      DIE ("`%c' unexpected for port, aborting\n", u);
	    }

	  t++;
	}

      if (getservbyname_needed)
	{
	  char *svc = strndup (service, len - (service - str));
	  struct servent *svt = getservbyname (svc, "tcp");

	  if (svt != NULL)
	    {
	      port = ntohs (svt->s_port);
	      free (svc);
	    }
	  else
	    {
	      fprintf (stderr, "service `%s' not found, aborting\n", svc);
	      free (svc);
	      exit (EXIT_FAILURE);
	    }
	}

      if (port == 0)
	{
	  DIE ("`0' not allowed for port, aborting\n");
	}
    }

  char *host = NULL;
  size_t skip_last = 1;

  if (sb == IPV6_SB_CLOSE)
    {
      str++;
      len--;
      skip_last = 2;
    }

  host = strndup (str, service ? service - str - skip_last : len);

  coc_log (COC_DEBUG_LOG_LEVEL,
	   "DEBUG Adding %s rule for %s connection to %s:%hu\n",
	   rule_type_name[rule_type], address_type_name[type], host, port);

  switch (type)
    {
    case COC_IPV6_ADDR:
      {
	coc_entry_t *e = coc_entry_alloc ();
	e->rule_type = rule_type;
	e->addr_type = COC_IPV6_ADDR;
	e->port = htons (port);

	if (inet_pton (AF_INET6, host, &e->addr.ipv6) != 1)
	  {
	    DIE ("Invalid IPv6 address: `%s', aborting\n", host);
	  }

	SLIST_INSERT_HEAD (&coc_list_head, e, entries);
	free (host);
	break;
      }

    case COC_IPV4_ADDR:
      {
	coc_entry_t *e = coc_entry_alloc ();
	e->rule_type = rule_type;
	e->addr_type = COC_IPV4_ADDR;
	e->port = htons (port);

	if (inet_pton (AF_INET, host, &e->addr.ipv4) != 1)
	  {
	    DIE ("Invalid IPv4 address: `%s', aborting\n", host);
	  }

	SLIST_INSERT_HEAD (&coc_list_head, e, entries);
	free (host);
	break;
      }

    case COC_GLOB_ADDR:
      {
	coc_entry_t *e = coc_entry_alloc ();
	e->rule_type = rule_type;
	e->addr_type = COC_GLOB_ADDR;
	e->port = htons (port);
	/* Here we transfer ownership of `host' to the entry. */
	e->addr.glob = host;
	SLIST_INSERT_HEAD (&coc_list_head, e, entries);

	/* Do not perform DNS lookups for '*' rules. We don't need to. */
	if (host[0] != '*' || host[1] != '\0')
	  {
	    needs_dns_lookup = true;
	  }
	break;
      }

    case COC_HOST_ADDR:
      {
	struct addrinfo *ailist, *aip;
	struct addrinfo hints;
	int err;
	memset (&hints, 0, sizeof(struct addrinfo));
#ifdef AI_ADDRCONFIG
	hints.ai_flags |= AI_ADDRCONFIG;
#endif
	hints.ai_family = AF_UNSPEC;	/* IPv4 or IPv6 or others */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if ((err = getaddrinfo (host, NULL, &hints, &ailist)) != 0)
	  {
	    DIE ("%s, aborting\n", gai_strerror (err));
	  }

	for (aip = ailist; aip != NULL; aip = aip->ai_next)
	  {
	    if (aip->ai_family == AF_INET)
	      {
		struct sockaddr_in *sa = (struct sockaddr_in *) aip->ai_addr;
		coc_entry_t *e = coc_entry_alloc ();
		e->rule_type = rule_type;
		e->addr_type = COC_IPV4_ADDR;
		e->port = htons (port);
		e->addr.ipv4 = sa->sin_addr;
		SLIST_INSERT_HEAD (&coc_list_head, e, entries);
	      }

	    else if (aip->ai_family == AF_INET6)
	      {
		struct sockaddr_in6 *sa =
		  (struct sockaddr_in6 *) aip->ai_addr;
		coc_entry_t *e = coc_entry_alloc ();
		e->rule_type = rule_type;
		e->addr_type = COC_IPV6_ADDR;
		e->port = htons (port);
		e->addr.ipv6 = sa->sin6_addr;
		SLIST_INSERT_HEAD (&coc_list_head, e, entries);
	      }
	  }

	freeaddrinfo (ailist);
	free (host);
	break;
      }
    }

  return 0;
}

static int
coc_rules_add (const char *rules, size_t rule_type)
{
  size_t count = 0;

  if (rules)
    {
      const char *p = rules + strlen (rules);
      size_t len = 0;

      while (p >= rules)
	{
	  if (*p == ';')
	    {
	      if (len > 1)
		{
		  coc_rule_add (p + 1, len - 1, rule_type);
		}

	      len = 1;
	      count++;
	    }
	  else
	    {
	      len++;
	    }

	  p--;
	}

      if (len - 1 > 0)
	{
	  coc_rule_add (p + 1, len - 1, rule_type);
	  count++;
	}
    }

  return count;
}

static long
coc_long_value (const char *name, const char *value, long lower_bound,
		long upper_bound)
{
  char *endptr;
  errno = 0;
  long v = strtol (value, &endptr, 10);
  if ((errno == ERANGE && (v == LONG_MAX || v == LONG_MIN)) ||
      (errno != 0 && value == 0L) ||
      *endptr != '\0' || (v < lower_bound || v > upper_bound))
    {
      DIE ("`%s' not valid for %s (should be between `%ld' and `%ld')\n",
	   value, name, lower_bound, upper_bound);
    }

  return v;
}


/* The real connect function. WARNING: cancellation point. */
static int (*real_connect) (int fd, const struct sockaddr * addr,
			    socklen_t addrlen);

#define NS "nameserver "

typedef struct coc_resolver {
  union {
    struct in_addr ipv4;
    struct in6_addr ipv6;
  } addr;
  bool isv6;
} coc_resolver_t;

static void
coc_read_resolv (coc_resolver_t * out, size_t * index)
{
  char buffer[1024];
  FILE *resolv = fopen ("/etc/resolv.conf", "r");

  if (!resolv)
    {
      DIE ("Could not read /etc/resolv.conf, aborting\n");
    }

  *index = 0;

  while (fgets (buffer, sizeof (buffer), resolv) && *index < MAXNS)
    {
      if (!strncmp (buffer, NS, sizeof (NS) - 1))
	{
	  char *ns = buffer + sizeof (NS) - 1;
	  size_t len = strlen (ns);

	  if (ns[len - 1] == '\n')
	    {
	      ns[len - 1] = '\0';
	    }

	  coc_log (COC_DEBUG_LOG_LEVEL, "DEBUG Found nameserver: %s\n", ns);

	  if (inet_pton (AF_INET, ns, &out[*index].addr.ipv4) == 1)
	    {
	      out[(*index)++].isv6 = false;
	    }
	  else if (inet_pton (AF_INET6, ns, &out[*index].addr.ipv6) == 1)
	    {
	      out[(*index)++].isv6 = true;
	    }
	  else
	    {
	      DIE ("Cannot process nameserver: `%s'\n", ns);
	    }
	}
    }

  fclose (resolv);
}

const char *
coc_version (void)
{
  return version;
}

static inline void
coc_sym_connect (void)
{
  if (real_connect == NULL)
    {
      real_connect =
	(int (*)(int, const struct sockaddr *, socklen_t)) dlsym (RTLD_NEXT,
								  "connect");

      if (real_connect == NULL)
	{
	  char *error = dlerror ();

	  if (error == NULL)
	    {
	      error = "connect is NULL";
	    }

	  DIE ("%s\n", error);
	}
    }
}

/* Called by dynamic linker when library is loaded. */
#ifdef __SUNPRO_C
#pragma init (coc_init)
#else
void coc_init (void) __attribute__ ((constructor));
#endif

void
coc_init (void)
{
  coc_sym_connect ();

  char *level = getenv (COC_LOG_LEVEL_ENV_VAR_NAME);
  if (level)
    {
      static const coc_log_level_t map[] = {
	COC_SILENT_LOG_LEVEL,
	COC_ERROR_LOG_LEVEL,
	COC_BLOCK_LOG_LEVEL,
	COC_ALLOW_LOG_LEVEL,
	COC_DEBUG_LOG_LEVEL
      };

      long lvl = coc_long_value (COC_LOG_LEVEL_ENV_VAR_NAME, level,
				 0,
				 sizeof (map) / sizeof (map[0]) - 1);

      log_level = map[(size_t) lvl];
    }

  char *target = getenv (COC_LOG_TARGET_ENV_VAR_NAME);
  if (target)
    {
      log_target = coc_long_value (COC_LOG_TARGET_ENV_VAR_NAME,
				   target, COC_STDERR_LOG,
				   COC_STDERR_LOG | COC_SYSLOG_LOG |
				   COC_FILE_LOG);

      if ((log_target & COC_FILE_LOG) == COC_FILE_LOG)
	{
	  const char *progname = getprogname ();
	  const char *log_path = getenv (COC_LOG_PATH_ENV_VAR_NAME);
	  if (!log_path)
	    {
	      log_path = ".";
	    }
	  log_file_name = malloc (strlen (log_path) + strlen (progname) + 6);
	  sprintf (log_file_name, "%s/%s.coc", log_path, progname);
	}
    }

  /* Initialize our singly-linked list. */
  SLIST_INIT (&coc_list_head);

  char *block = getenv (COC_BLOCK_ENV_VAR_NAME);
  coc_rules_add (block, COC_BLOCK);

  char *allow = getenv (COC_ALLOW_ENV_VAR_NAME);
  coc_rules_add (allow, COC_ALLOW);

  if (allow != NULL && block == NULL && needs_dns_lookup)
    {
      DIE ("Glob specified for ALLOW rule but no rule for BLOCK; aborting\n");
    }

  /* Fail if there is a glob and DNS is not allowed. */
  if (needs_dns_lookup)
    {
      bool dns_server_found = false;

      /* Read nameserver entries in /etc/resolv.conf */
      coc_resolver_t dns[MAXNS] = { 0 };
      size_t dns_count;
      coc_read_resolv (dns, &dns_count);

      /* Cycle in all allowed IP entries to check if we have one of these */
      coc_entry_t *e;
      SLIST_FOREACH (e, &coc_list_head, entries)
      {
	if (e->rule_type == COC_ALLOW &&
	    e->addr_type != COC_GLOB_ADDR &&
	    (!e->port || e->port == htons (53)))
	  {
	    size_t i;
	    for (i = 0; i < dns_count; i++)
	      {
		if ((e->addr_type == COC_IPV4_ADDR &&
		     !dns[i].isv6 &&
		     e->addr.ipv4.s_addr == dns[i].addr.ipv4.s_addr) ||
		    (e->addr_type == COC_IPV6_ADDR &&
		     dns[i].isv6 &&
		     IN6_ARE_ADDR_EQUAL (&e->addr.ipv6, &dns[i].addr.ipv6)))
		  {
		    dns_server_found = true;
		    break;
		  }
	      }
	  }
      }

      if (!dns_server_found)
	{
	  DIE ("No DNS allowed while some glob rule need one, aborting\n");
	}
    }

  initialized = true;
}

#define INET4_FMLY(a) (a->sa_family == AF_INET)
#define INET4_CAST(a) ((const struct sockaddr_in *) a)
#define INET6_CAST(a) ((const struct sockaddr_in6 *) a)
#define INET4_PORT(a) (INET4_CAST (a)->sin_port)
#define INET6_PORT(a) (INET6_CAST (a)->sin6_port)
#define INETX_PORT(a) (INET4_FMLY (a) ? INET4_PORT (a) : INET6_PORT (a))
#define INET4_ADDR(a) (&INET4_CAST (a)->sin_addr)
#define INET6_ADDR(a) (&INET6_CAST (a)->sin6_addr)
#define INETX_ADDR(a) (INET4_FMLY (a) ? (void *) INET4_ADDR (a) : (void *) INET6_ADDR (a))
#define INET4_IN_6(i) ((struct in_addr *) ((i)->s6_addr + 12))

static inline
bool
coc_rule_match (coc_entry_t *e, const struct sockaddr *addr, const char *buf)
{
  switch (e->addr_type)
    {
    case COC_IPV6_ADDR:
      if (addr->sa_family == AF_INET6)
	{
	  return (IN6_ARE_ADDR_EQUAL (&e->addr.ipv6, INET6_ADDR (addr)) &&
		  (!e->port || e->port == INET6_PORT (addr)));
	}
      else if (addr->sa_family == AF_INET)
	{
	  if (IN6_IS_ADDR_V4MAPPED (&e->addr.ipv6))
	    {
	      return (INET4_IN_6 (&e->addr.ipv6)->s_addr == INET4_ADDR (addr)->s_addr &&
		      (!e->port || e->port == INET4_PORT (addr)));
	    }
	}
      break;

    case COC_IPV4_ADDR:
      if (addr->sa_family == AF_INET)
	{
	  return (e->addr.ipv4.s_addr == INET4_ADDR (addr)->s_addr &&
		  (!e->port || e->port == INET4_PORT (addr)));
	}
      else if (addr->sa_family == AF_INET6)
	{
	  if (IN6_IS_ADDR_V4MAPPED (INET6_ADDR (addr)))
	    {
	      return (e->addr.ipv4.s_addr == INET4_IN_6 (INET6_ADDR (addr))->s_addr &&
		      (!e->port || e->port == INET6_PORT (addr)));
	    }
	}
      break;

    case COC_GLOB_ADDR:
      return (((e->addr.glob[0] == '*' && e->addr.glob[1] == '\0')
	       || !fnmatch (e->addr.glob, buf, 0))
	      && (!e->port || e->port == INETX_PORT (addr)));
    }

  return false;
}

/*
 * Real work happens here.
 *
 * We don't do anything except for AF_INET and AF_INET6.
 */
int
connect (int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  if (!initialized)
    {
      /* With SELinux enabled `connect' gets called for audit purpose
         _before_ `coc_init' so we ensure we have the real `connect'
	 symbol address here to avoid segfaulting. */
      coc_sym_connect ();

      /* Do not check connections if initialization is not complete. */
      return real_connect (fd, addr, addrlen);
    }

  if (addr != NULL &&
      (addr->sa_family == AF_INET || addr->sa_family == AF_INET6))
    {
      char str[INET6_ADDRSTRLEN];
      in_port_t port = INETX_PORT (addr);
      inet_ntop (addr->sa_family, INETX_ADDR (addr), str, INET6_ADDRSTRLEN);

      char hbuf[NI_MAXHOST] = "*";

      bool dns_lookup_done = !needs_dns_lookup;

      /* We have access to IP address and port where the
       * connection is requested.
       *
       * Logic:
       *  1. check if this address appears in our ALLOW rules:
       *     - if so, proceed to the real connect call
       *  2. If not, check if it's in the BLOCK rules:
       *     - if so, call pthread_testcancel, then return EACCES
       *  3. Otherwise proceed with regular connect call. */
      coc_entry_t *e;
      SLIST_FOREACH (e, &coc_list_head, entries)
      {
	if (e->addr_type == COC_GLOB_ADDR)
	  {
	    if (!dns_lookup_done)
	      {
		int rc = getnameinfo (addr, addrlen, hbuf, sizeof (hbuf),
                                      NULL, 0, NI_NUMERICSERV);

		if (rc)
		  {
		    coc_log (COC_BLOCK_LOG_LEVEL, "ERROR resolving name: %s\n",
			     gai_strerror (rc));
		    continue;
		  }
                else
                  {
                    dns_lookup_done = true;
                  }
	      }
	  }

	coc_log (COC_DEBUG_LOG_LEVEL,
		 "DEBUG Checking %s rule for %s connection to %s:%hu\n",
		 rule_type_name[e->rule_type],
		 address_type_name[e->addr_type],
		 str, ntohs (e->port));

	if (coc_rule_match(e, addr, hbuf))
	  {
	    if (e->rule_type == COC_ALLOW)
	      {
		coc_log (COC_ALLOW_LOG_LEVEL, "ALLOW connection to %s:%hu\n", str,
			 ntohs (port));
		return real_connect (fd, addr, addrlen);
	      }
	    else
	      {
		pthread_testcancel ();
		coc_log (COC_BLOCK_LOG_LEVEL, "BLOCK connection to %s:%hu\n", str,
			 ntohs (port));
		errno = EACCES;
		return -1;
	      }
	  }
      }

      coc_log (COC_ALLOW_LOG_LEVEL, "ALLOW connection to %s:%hu\n", str,
	       ntohs (port));
    }

  return real_connect (fd, addr, addrlen);
}
