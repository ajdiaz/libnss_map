/**
 * libnss_map;  http://connectical.com/projects/libnss_map
 * Copyright 2009 Connectical Technologies; Distributed under GPL.
 * --
 * A nss module which maps all users and groups to one system user/group
 * specified in configuration file.
 * --
 * Copyright (C) 2009  Andrés J. Díaz <ajdiaz@connectical.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 * --
 * This work is heavly based on nss_ato module by Pietro Donatini.
 **/

#include <sys/param.h>
#include <errno.h>
#include <grp.h>
#include <nss.h>
#include <pthread.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* yeahh, a mutex! */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_file;

/* Some services, like ssh fails when shadow is locked with ! character, to
 * prevent this behaviour we can use other character here. */
#define LOCKED_PASSWD "NP"

/* for security reasons */
#ifndef MIN_UID_NUMER
#	define MIN_UID_NUMBER   500
#endif
#ifndef MIN_GIG_NUMBER
#	define MIN_GID_NUMBER   500
#endif
#ifndef MAIN_CONF_FILE
#	define MAIN_CONF_FILE   "/etc/nssmap.conf"
#endif
#ifndef USER_CONF_FILE
#	define USER_CONF_FILE ".nssmaprc"
#endif
#ifndef NGROUPS_MAX
#	define NGROUPS_MAX 256
#endif
#ifdef DEBUG
#	undef DEBUG /* remove previous definition */
#	define DEBUG(fmt, args...) fprintf(stderr, fmt, ##args)
#else
#	define DEBUG(fmt, ...)
#endif

/*
   This comment is a little remember note :D

   struct passwd
   {
   char *pw_name;                Username. 
   char *pw_passwd;              Password. 
   __uid_t pw_uid;               User ID. 
   __gid_t pw_gid;               Group ID.
   char *pw_gecos;               Real name.
   char *pw_dir;                 Home directory.
   char *pw_shell;               Shell program. 
   };

   group structure in <grp.h>

   struct group {
   char   *gr_name;       // group name
   char   *gr_passwd;     // group password
   gid_t   gr_gid;        // group ID
   char  **gr_mem;        // group members
   };


   shadow passwd structure in <shadow.h>

   struct spwd 
   {
   char *sp_namp;          Login name 
   char *sp_pwdp;          Encrypted password 
   long sp_lstchg;         Date of last change
   long sp_min;            Min #days between changes
   long sp_max;            Max #days between changes
   long sp_warn;           #days before pwd expires to warn user to change it
   long sp_inact;          #days after pwd expires until account is disabled
   long sp_expire;         #days since 1970-01-01 until account is disabled 
   unsigned long sp_flag;  Reserved 
   };

*/

/*  What can be configured in passwd configuration files */
typedef struct _map_conf_s {
	char *pw_name;
	char *pw_gecos;
	char *pw_dir;
	char *pw_shell;
	__uid_t pw_uid;
	__gid_t pw_gid;
} map_conf_t;


/* fun: new_conf
 * txt: get a new empty configuration type */
map_conf_t *
new_conf(void)
{
	map_conf_t *conf;

	if ((conf = (map_conf_t *) malloc(sizeof (map_conf_t))) == NULL) 
		return NULL;

	conf->pw_name   = NULL;
	conf->pw_gecos  = NULL;
	conf->pw_dir    = NULL;
	conf->pw_shell  = NULL;

	return conf;
}

/* Common return code routine for all *ent_r_locked functions.
 * We need to return TRYAGAIN if the underlying files guy raises ERANGE,
 * so that our caller knows to try again with a bigger buffer.
 */

static inline enum nss_status
_nss_map_ent_bad_return_code(int errnop) {
	enum nss_status ret;

	switch (errnop) {
		case ERANGE:
			DEBUG("%s:%d:bad_return:ERANGE:try again with a bigger buffer\n",
					__FILE__, __LINE__);
			ret = NSS_STATUS_TRYAGAIN;
			break;
		case ENOENT:
		default:
			DEBUG("%s:%d:bad_return:ENOENT/default:not found more.\n",
					__FILE__, __LINE__);
			ret = NSS_STATUS_NOTFOUND;
	};
	return ret;
}

/* fun: free_conf conf
 * txt: delete and free memory associated with configuration struct called
 *      conf. */
int
free_conf(map_conf_t *conf) 
{
	if ( conf->pw_name != NULL )
		free(conf->pw_name);
	if ( conf->pw_gecos != NULL )
		free(conf->pw_gecos);
	if ( conf->pw_dir != NULL )
		free(conf->pw_dir);
	if ( conf->pw_shell != NULL )
		free(conf->pw_shell);
	if ( conf != NULL )
		free(conf);
	return 0;
}

/*
 * the configuration /etc/libnss-map.conf is just one line
 * whith the local user data as in /etc/passwd. For example:
 * dona:x:1001:1001:P D ,,,:/home/dona:/bin/bash 
 */

static map_conf_t *
read_conf()
{
	map_conf_t *conf;
	static FILE *fd;
	char buff[BUFSIZ];
	char *b;
	char *value;

	if ((conf = new_conf()) == NULL) 
		return NULL;

	if ((fd = fopen(MAIN_CONF_FILE, "r")) == NULL ) {
		free_conf(conf);
		return NULL;
	}

	if (fgets (buff, BUFSIZ, fd) == NULL) {
		fclose(fd);
		free_conf(conf);
		return NULL;
	}
	fclose(fd);

	/* start reading configuration file */
	b = buff;

	/* pw_name */
	value = b;

	while (*b != ':' && *b != '\0')
		b++;

	if (*b != ':') 
		goto format_error;

	*b = '\0';
	b++;

	conf->pw_name = strdup(value);

	/* NOT USED pw_passwd will be set equal to  x (we like shadows) */
	while (*b != ':' && *b != '\0')
		b++;

	if ( *b != ':' )
		goto format_error;

	b++;

	/* pw_uid */
	value = b;

	while (*b != ':' && *b != '\0')
		b++;

	if (*b != ':')
		goto format_error;

	b++;

	conf->pw_uid = atoi(value);

	if ( conf->pw_uid < MIN_UID_NUMBER )
		conf->pw_uid = MIN_UID_NUMBER;

	/* pw_gid */
	value = b;

	while (*b != ':' && *b != '\0')
		b++;

	if (*b != ':')
		goto format_error;

	b++;
	conf->pw_gid = atoi(value);
	if ( conf->pw_gid < MIN_GID_NUMBER )
		conf->pw_gid = MIN_GID_NUMBER;

	/* pw_gecos */  
	value = b;

	while (*b != ':' && *b != '\0')
		b++;

	if (*b != ':')
		goto format_error;

	*b = '\0';
	b++;

	conf->pw_gecos = strdup (value);

	/* pw_dir */  
	value = b;

	while (*b != ':' && *b != '\0')
		b++;

	if (*b != ':')
		goto format_error;

	*b = '\0';
	b++;
	
	conf->pw_dir = strdup (value);

	/* pw_shell takes the rest */  
	/* Kyler Laird suggested to strip end line */
	value = b;

	while (*b != '\n' && *b != '\0')
		b++;

	*b = '\0';

	conf->pw_shell = strdup(value);

	return conf;

format_error:
	free (conf);
	return NULL;
}


/* fun: get_static buffer buflen len
 * txt: allocate some space from the nss static buffer. The buffer and
 * buflen are the pointers passed in by the C library to the
 * _nss_ntdom_* functions. This piece of code is taken from glibc.
 */
static char *
get_static(char **buffer, size_t *buflen, int len)
{
	char *result;

	/* Error check.  We return false if things aren't set up right, or
	 * there isn't enough buffer space left. */

	if ((buffer == NULL) || (buflen == NULL) || (*buflen < len)) {
		return NULL;
	}

	/* Return an index into the static buffer */

	result = *buffer;
	*buffer += len;
	*buflen -= len;

	return result;
}

/* fun: _nss_map_getpwuid
 * txt: get the passwd struct from uid, for mapped users, the value of user
 *		name is taken from the environment, using the LOGNAME variable.
 */
enum nss_status
_nss_map_getpwuid_r(uid_t uid, struct passwd *p,
		char *buffer, size_t buflen, int *errnop)
{

	map_conf_t *conf;
	char * name;

	/* XXX: The logname hack */
	if ( (name = getenv("LOGNAME")) == NULL )
		return NSS_STATUS_NOTFOUND;

	if ((conf = read_conf()) == NULL) {
		return NSS_STATUS_NOTFOUND;
	}

	/* If out of memory */
	if ((p->pw_name = get_static(&buffer, &buflen, strlen(name) + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	/* pw_name stay as the name given */
	strcpy(p->pw_name, name);

	if ((p->pw_passwd = get_static(&buffer, &buflen, strlen("x") + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_passwd, "x");

	p->pw_uid = conf->pw_uid; /* UID_NUMBER; */
	p->pw_gid = conf->pw_gid; /* GID_NUMBER; */

	if ((p->pw_gecos = get_static(&buffer, &buflen, strlen(conf->pw_gecos) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_gecos, conf->pw_gecos);

	if ((p->pw_dir = get_static(&buffer, &buflen, strlen(conf->pw_dir) + 1
					+ strlen(name) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	/* XXX: the dirname hack */
	strcpy(p->pw_dir, conf->pw_dir);
	strcat(p->pw_dir,"/");
	strcat(p->pw_dir,name);

	if ((p->pw_shell = get_static(&buffer, &buflen, strlen(conf->pw_shell) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_shell, conf->pw_shell);

	free_conf(conf);

	return NSS_STATUS_SUCCESS;

}

/* fun: _nss_map_getpwnam_r
 * txt: get a passwd struct from username mapped to generic user.
 */
enum nss_status
_nss_map_getpwnam_r( const char *name, struct passwd *p,
		char *buffer, size_t buflen, int *errnop)
{
	map_conf_t *conf;

	if ((conf = read_conf()) == NULL) {
		return NSS_STATUS_NOTFOUND;
	}

	/* If out of memory */
	if ((p->pw_name = get_static(&buffer, &buflen, strlen(name) + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	/* pw_name stay as the name given */
	strcpy(p->pw_name, name);

	if ((p->pw_passwd = get_static(&buffer, &buflen, strlen("x") + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_passwd, "x");

	p->pw_uid = conf->pw_uid; /* UID_NUMBER; */
	p->pw_gid = conf->pw_gid; /* GID_NUMBER; */

	if ((p->pw_gecos = get_static(&buffer, &buflen, strlen(conf->pw_gecos) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_gecos, conf->pw_gecos);

	if ((p->pw_dir = get_static(&buffer, &buflen, strlen(conf->pw_dir) + 1 +
					strlen(name) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_dir, conf->pw_dir);
	strcat(p->pw_dir,"/");
	strcat(p->pw_dir,name);

	if ((p->pw_shell = get_static(&buffer, &buflen, strlen(conf->pw_shell) + 1 )) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(p->pw_shell, conf->pw_shell);

	free_conf(conf);

	return NSS_STATUS_SUCCESS;
}


/* fun: _nss_map_getspnam_r
 * txt: get shadow struct by name mapped to users.
 */
enum nss_status
_nss_map_getspnam_r( const char *name, struct spwd *s,
		char *buffer, size_t buflen, int *errnop)
{

	/* If out of memory */
	if ((s->sp_namp = get_static(&buffer, &buflen, strlen(name) + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(s->sp_namp, name);

	if ((s->sp_pwdp = get_static(&buffer, &buflen, strlen("!") + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(s->sp_pwdp, LOCKED_PASSWD);

	s->sp_lstchg = 13571;
	s->sp_min    = 0;
	s->sp_max    = 99999;
	s->sp_warn   = 7;

	return NSS_STATUS_SUCCESS;
}

enum nss_status
_nss_map_getgrgid_r( gid_t gid, struct group *g,
		char *buffer, size_t buflen, int *errnop)
{
	char * name;

	/* XXX: The logname hack */
	if ( (name = getenv("LOGNAME")) == NULL )
		return NSS_STATUS_NOTFOUND;

	if ((g->gr_name = get_static(&buffer, &buflen, strlen(name) + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}
	strcpy(g->gr_name, name);

	if ((g->gr_passwd = get_static(&buffer, &buflen, strlen("x") + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(g->gr_passwd, "x");

	g->gr_gid = gid;
	g->gr_mem = NULL;

	return NSS_STATUS_SUCCESS;
}

enum nss_status
_nss_map_getgrnam_r( const char *name, struct group *g,
		char *buffer, size_t buflen, int *errnop) {

	map_conf_t *conf;

	if ((conf = read_conf()) == NULL) {
		return NSS_STATUS_NOTFOUND;
	}

	/* If out of memory */
	if ((g->gr_name = get_static(&buffer, &buflen, strlen(name) + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	/* gr_name stay as the name given */
	strcpy(g->gr_name, name);

	if ((g->gr_passwd = get_static(&buffer, &buflen, strlen("x") + 1)) == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(g->gr_passwd, "x");

	g->gr_gid = conf->pw_gid; /* GID_NUMBER; */

	free_conf(conf);

	return NSS_STATUS_SUCCESS;
}

static enum nss_status
_nss_map_setgrent_locked()
{
	map_conf_t *conf;
	char *dir;
	struct stat s;
	char *name;

	conf = read_conf();

	if (conf == NULL) {
		DEBUG("%s:%d:setgrent_r:unable to open configuration file (%s).\n",
				__FILE__, __LINE__, MAIN_CONF_FILE);
		return NSS_STATUS_UNAVAIL;
	}

	/* XXX: The logname hack */
	if ( (name = getenv("LOGNAME")) == NULL )
	{
		DEBUG("%s:%d:setgrent_r:environment LOGNAME is not set.\n",
				__FILE__, __LINE__);
		return NSS_STATUS_UNAVAIL;
	}

	if (( dir = (char *)malloc( (strlen(name) + 1 +
						strlen(conf->pw_dir)  + 1 +
						strlen(MAIN_CONF_FILE)) * sizeof(char) ) ) == NULL)
	{
		DEBUG("%s:%d:setgrent_r:unable to adquire memory for config.\n",
				__FILE__, __LINE__);
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(dir, conf->pw_dir);
	strcat(dir,"/");
	strcat(dir,name);
	strcat(dir,"/");
	strcat(dir,USER_CONF_FILE);

	free_conf(conf);

	/* some security checking */
	if ( stat(dir, &s) == -1 )
		goto format_error;

	if ( ! S_ISREG(s.st_mode) )
		goto format_error;

	if ( (s.st_mode & S_IWGRP) || (s.st_mode & S_IWOTH) )
		goto format_error;

	if ( (s.st_uid) || (s.st_gid) )
		goto format_error;

	g_file = fopen(dir, "r");

	if (g_file == NULL)
		goto format_error;

	return NSS_STATUS_SUCCESS;

format_error:
	if(g_file != NULL) fclose(g_file);
	free(dir);
    return NSS_STATUS_UNAVAIL;

}

enum nss_status
_nss_map_setgrent (void) {
	enum nss_status ret;

	pthread_mutex_lock(&mutex);
	ret = _nss_map_setgrent_locked();
	pthread_mutex_unlock(&mutex);
	return ret;
}

static enum nss_status
_nss_map_endgrent_locked() {

	if (g_file != NULL)
	{
		DEBUG("%s:%d:endgrent: closing user file (fileno: %d).\n",
				__FILE__, __LINE__, fileno(g_file));
		fclose(g_file);
		g_file = NULL;
	}
	return NSS_STATUS_SUCCESS;
}

enum nss_status
_nss_map_endgrent()
{
  enum nss_status ret;

  pthread_mutex_lock(&mutex);
  ret = _nss_map_endgrent_locked();
  pthread_mutex_unlock(&mutex);

  return ret;
}

static enum nss_status
_nss_map_getgrent_r_locked (struct group *g, char * buffer,
		size_t buflen, int * errnop)
{
	enum nss_status ret = NSS_STATUS_NOTFOUND;
	fpos_t position;

	fgetpos(g_file, &position);
	if ( fgetgrent_r(g_file, g, buffer, buflen, &g) == 0) {
		DEBUG("%s:%d:getgrent_r: returning group %s (%d)\n",
				__FILE__, __LINE__, g->gr_name, g->gr_gid);
		ret = NSS_STATUS_SUCCESS;
	} else {
		switch(errno)
		{
			case ERANGE:
				/* Rewind back to where we were just before, otherwise
				 * the data read into the buffer is probably going to be
				 * lost because there's no guarantee that the caller is
				 * going to have preserved the line we just read. Note
				 * that glibc's nss/nss_files/files-XXX.c does something
				 * similar in CONCAT(_nss_files_get,ENTNAME_r) (around
				 * line 242 in glibc 2.4 sources).
				 */
				fsetpos(g_file, &position);
				*errnop = errno;
				ret = _nss_map_ent_bad_return_code(*errnop); break;
			case ENOENT:
				/* XXX */
				return NSS_STATUS_NOTFOUND; break;
		}
	}
	return ret;
}

enum nss_status
_nss_map_getgrent_r(struct group *g,
		char *buffer, size_t buflen, int *errnop)
{
	enum nss_status ret;

	pthread_mutex_lock(&mutex);
	ret = _nss_map_getgrent_r_locked(g, buffer, buflen, errnop);
	pthread_mutex_unlock(&mutex);

	return ret;
}

static int
internal_gid_in_list (const gid_t *list, const gid_t g, long int len)
{
  while (len > 0)
    {
      if (*list == g)
    return 1;
      --len;
      ++list;
    }
  return 0;
}

enum nss_status
_nss_map_initgroups_dyn (const char *user, gid_t group, long int *start,
		long int *size, gid_t **groupsp, long int limit, int *errnop)
{
	gid_t *groups = *groupsp;
	FILE *fp = NULL;
	map_conf_t *conf;
	char *dir;
	struct stat s;
	struct group *g;

	conf = read_conf();

	if (conf == NULL) {
		DEBUG("%s:%d:initgroups_dyn:unable to open configuration file (%s).\n",
				__FILE__, __LINE__, MAIN_CONF_FILE);
		return NSS_STATUS_UNAVAIL;
	}

	if (( dir = (char *)malloc( (strlen(user) + 1 +
						strlen(conf->pw_dir)  + 1 +
						strlen(MAIN_CONF_FILE)) * sizeof(char) ) ) == NULL)
	{
		DEBUG("%s:%d:initgroups_dyn:unable to adquire memory for config.\n",
				__FILE__, __LINE__);
		return NSS_STATUS_TRYAGAIN;
	}

	strcpy(dir, conf->pw_dir);
	strcat(dir,"/");
	strcat(dir,user);
	strcat(dir,"/");
	strcat(dir,USER_CONF_FILE);
	free_conf(conf);

	/* some security checking */
	if ( stat(dir, &s) == -1 )
		goto format_error;

	if ( ! S_ISREG(s.st_mode) )
		goto format_error;

	if ( (s.st_mode & S_IWGRP) || (s.st_mode & S_IWOTH) )
		goto format_error;

	if ( (s.st_uid) || (s.st_gid) )
		goto format_error;

	if ( (fp = fopen(dir, "r")) == NULL )
		goto format_error;

	if ( (g = fgetgrent(fp)) == NULL )
		goto format_error;
	else
		fclose(fp);

	if (!internal_gid_in_list (groups, group, *start))
	{
		if (__builtin_expect (*start == *size, 0))
		{
			/* Need a bigger buffer.  */
			gid_t *newgroups;
			long int newsize;

			if (limit > 0 && *size == limit)
				/* We reached the maximum.  */
				goto done;

			if (limit <= 0)
				newsize = 2 * *size;
			else
				newsize = MIN (limit, 2 * *size);

			newgroups = realloc (groups, newsize * sizeof (*groups));
			if (newgroups == NULL)
				goto done;
			*groupsp = groups = newgroups;
			*size = newsize;
		}

		groups[(*start)++] = group;
	}

	groups[(*start)++] = g->gr_gid;

done:
	return NSS_STATUS_SUCCESS;

format_error:
	if(fp != NULL) fclose(fp);
	free(dir);
    return NSS_STATUS_UNAVAIL;

}


