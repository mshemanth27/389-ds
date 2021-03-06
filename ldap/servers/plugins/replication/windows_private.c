/** BEGIN COPYRIGHT BLOCK
 * This Program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; version 2 of the License.
 * 
 * This Program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this Program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA 02111-1307 USA.
 * 
 * In addition, as a special exception, Red Hat, Inc. gives You the additional
 * right to link the code of this Program with code not covered under the GNU
 * General Public License ("Non-GPL Code") and to distribute linked combinations
 * including the two, subject to the limitations in this paragraph. Non-GPL Code
 * permitted under this exception must only link to the code of this Program
 * through those well defined interfaces identified in the file named EXCEPTION
 * found in the source code files (the "Approved Interfaces"). The files of
 * Non-GPL Code may instantiate templates or use macros or inline functions from
 * the Approved Interfaces without causing the resulting work to be covered by
 * the GNU General Public License. Only Red Hat, Inc. may make changes or
 * additions to the list of Approved Interfaces. You must obey the GNU General
 * Public License in all respects for all of the Program code and other code used
 * in conjunction with the Program except the Non-GPL Code covered by this
 * exception. If you modify this file, you may extend this exception to your
 * version of the file, but you are not obligated to do so. If you do not wish to
 * provide this exception without modification, you must delete this exception
 * statement from your version and license this file solely under the GPL without
 * exception. 
 * 
 * 
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2005 Red Hat, Inc.
 * All rights reserved.
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif


/* windows_private.c */

#include "repl.h"
#include "repl5.h"
#include "slap.h"
#include "slapi-plugin.h"
#include "winsync-plugin.h"
#include "windowsrepl.h"

struct windowsprivate {
  
  Slapi_DN *windows_subtree; /* DN of synchronized subtree  (on the windows side) */
  Slapi_DN *directory_subtree; /* DN of synchronized subtree on directory side */
                                /* this simplifies the mapping as it's simply
								   from the former to the latter container, or
								   vice versa */
  ber_int_t dirsync_flags;		
  ber_int_t dirsync_maxattributecount;
  char *dirsync_cookie; 
  int dirsync_cookie_len;
  PRBool dirsync_cookie_has_more;
  PRBool create_users_from_dirsync;
  PRBool create_groups_from_dirsync;
  char *windows_domain;
  int isnt4;
  int iswin2k3;
  /* This filter is used to determine if an entry belongs to this agreement.  We put it here
   * so we only have to allocate each filter once instead of doing it every time we receive a change. */
  Slapi_Filter *directory_filter; /* Used for checking if local entries need to be sync'd to AD */
  Slapi_Filter *deleted_filter; /* Used for checking if an entry is an AD tombstone */
  Slapi_Entry *raw_entry; /* "raw" un-schema processed last entry read from AD */
  int keep_raw_entry; /* flag to control when the raw entry is set */
  void *api_cookie; /* private data used by api callbacks */
  time_t sync_interval; /* how often to run the dirsync search, in seconds */
  int one_way; /* Indicates if this is a one-way agreement and which direction it is */
};

static void windows_private_set_windows_domain(const Repl_Agmt *ra, char *domain);

static int
true_value_from_string(char *val)
{
	if (strcasecmp (val, "on") == 0 || strcasecmp (val, "yes") == 0 ||
		strcasecmp (val, "true") == 0 || strcasecmp (val, "1") == 0)
	{
		return 1;
	} else 
	{
		return 0;
	}
}

static int
windows_parse_config_entry(Repl_Agmt *ra, const char *type, Slapi_Entry *e)
{
	char *tmpstr = NULL;
	int retval = 0;
	
	if (type == NULL || slapi_attr_types_equivalent(type,type_nsds7WindowsReplicaArea))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_nsds7WindowsReplicaArea);
		if (NULL != tmpstr)
		{
			windows_private_set_windows_subtree(ra, slapi_sdn_new_dn_passin(tmpstr) );
		}
		retval = 1;
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_nsds7DirectoryReplicaArea))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_nsds7DirectoryReplicaArea); 
		if (NULL != tmpstr)
		{
			windows_private_set_directory_subtree(ra, slapi_sdn_new_dn_passin(tmpstr) );
		}
		retval = 1;
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_nsds7CreateNewUsers))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_nsds7CreateNewUsers); 
		if (NULL != tmpstr && true_value_from_string(tmpstr))
		{
			windows_private_set_create_users(ra, PR_TRUE);
		}
		else
		{
			windows_private_set_create_users(ra, PR_FALSE);
		}
		retval = 1;
		slapi_ch_free((void**)&tmpstr);
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_nsds7CreateNewGroups))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_nsds7CreateNewGroups); 
		if (NULL != tmpstr && true_value_from_string(tmpstr))
		{
			windows_private_set_create_groups(ra, PR_TRUE);
		}
		else
		{
			windows_private_set_create_groups(ra, PR_FALSE);
		}
		retval = 1;
		slapi_ch_free((void**)&tmpstr);
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_nsds7WindowsDomain))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_nsds7WindowsDomain); 
		if (NULL != tmpstr)
		{
			windows_private_set_windows_domain(ra,tmpstr);
		}
		/* No need to free tmpstr because it was aliased by the call above */
		tmpstr = NULL;
		retval = 1;
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_winSyncInterval))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_winSyncInterval); 
		if (NULL != tmpstr)
		{
			windows_private_set_sync_interval(ra,tmpstr);
		}
		slapi_ch_free_string(&tmpstr);
		retval = 1;
	}
	if (type == NULL || slapi_attr_types_equivalent(type,type_oneWaySync))
	{
		tmpstr = slapi_entry_attr_get_charptr(e, type_oneWaySync);
		if (NULL != tmpstr)
		{
			if (strcasecmp(tmpstr, "fromWindows") == 0) {
				windows_private_set_one_way(ra, ONE_WAY_SYNC_FROM_AD);
			} else if (strcasecmp(tmpstr, "toWindows") == 0) {
				windows_private_set_one_way(ra, ONE_WAY_SYNC_TO_AD);
			} else {
				slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name,
					"Ignoring illegal setting for %s attribute in replication "
					"agreement \"%s\".  Valid values are \"toWindows\" or "
					"\"fromWindows\".\n", type_oneWaySync, slapi_entry_get_dn(e));
				windows_private_set_one_way(ra, ONE_WAY_SYNC_DISABLED);
			}
		}
		else
		{
			windows_private_set_one_way(ra, ONE_WAY_SYNC_DISABLED);
		}
		slapi_ch_free((void**)&tmpstr);
		retval = 1;
	}
	return retval;
}

/* Returns non-zero if the modify was ok, zero if not */
int
windows_handle_modify_agreement(Repl_Agmt *ra, const char *type, Slapi_Entry *e)
{
	/* Is this a Windows agreement ? */
	if (get_agmt_agreement_type(ra) == REPLICA_TYPE_WINDOWS)
	{
		return windows_parse_config_entry(ra,type,e);
	} else
	{
		return 0;
	}
}

void
windows_init_agreement_from_entry(Repl_Agmt *ra, Slapi_Entry *e)
{
	agmt_set_priv(ra,windows_private_new());

	windows_parse_config_entry(ra,NULL,e);

	windows_plugin_init(ra);
}

const char* windows_private_get_purl(const Repl_Agmt *ra)
{
	const char* windows_purl;
	char *hostname;

	hostname = agmt_get_hostname(ra);
	if(slapi_is_ipv6_addr(hostname)){
		/* need to put brackets around the ipv6 address */
		windows_purl = slapi_ch_smprintf("ldap://[%s]:%d", hostname, agmt_get_port(ra));
	} else {
		windows_purl = slapi_ch_smprintf("ldap://%s:%d", hostname, agmt_get_port(ra));
	}
	slapi_ch_free_string(&hostname);

	return windows_purl;
}

Dirsync_Private* windows_private_new()
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_new\n" );

	dp = (Dirsync_Private *)slapi_ch_calloc(sizeof(Dirsync_Private),1);

	dp->dirsync_maxattributecount = -1;
	dp->directory_filter = NULL;
	dp->deleted_filter = NULL;
	dp->sync_interval = PERIODIC_DIRSYNC_INTERVAL;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_new\n" );
	return dp;

}

void windows_agreement_delete(Repl_Agmt *ra)
{

	Dirsync_Private *dp = (Dirsync_Private *) agmt_get_priv(ra);
	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_delete\n" );

	PR_ASSERT(dp  != NULL);
	
	winsync_plugin_call_destroy_agmt_cb(ra, dp->directory_subtree,
										dp->windows_subtree);

	slapi_sdn_free(&dp->directory_subtree);
	slapi_sdn_free(&dp->windows_subtree);
	slapi_filter_free(dp->directory_filter, 1);
	slapi_filter_free(dp->deleted_filter, 1);
	slapi_entry_free(dp->raw_entry);
	dp->raw_entry = NULL;
	dp->api_cookie = NULL;
	slapi_ch_free((void **)dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_delete\n" );

}

int windows_private_get_isnt4(const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_isnt4\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);
		
		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_isnt4\n" );
	
		return dp->isnt4;	
}

void windows_private_set_isnt4(const Repl_Agmt *ra, int isit)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_isnt4\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		dp->isnt4 = isit;
		
		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_isnt4\n" );
}

int windows_private_get_iswin2k3(const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_iswin2k3\n" );

	PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_iswin2k3\n" );

		return dp->iswin2k3;
}

void windows_private_set_iswin2k3(const Repl_Agmt *ra, int isit)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_iswin2k3\n" );

	PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		dp->iswin2k3 = isit;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_iswin2k3\n" );
}

/* Returns a copy of the Slapi_Filter pointer.  The caller should not free it */
Slapi_Filter* windows_private_get_directory_filter(const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_directory_filter\n" );

	PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		if (dp->directory_filter == NULL) {
			char *string_filter = slapi_ch_strdup("(&(|(objectclass=ntuser)(objectclass=ntgroup))(ntUserDomainId=*))");
			/* The filter gets freed in windows_agreement_delete() */
                        dp->directory_filter = slapi_str2filter( string_filter );
			slapi_ch_free_string(&string_filter);
		}

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_directory_filter\n" );

		return dp->directory_filter;
}

/* Returns a copy of the Slapi_Filter pointer.  The caller should not free it */
Slapi_Filter* windows_private_get_deleted_filter(const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_deleted_filter\n" );

	PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		if (dp->deleted_filter == NULL) {
			char *string_filter = slapi_ch_strdup("(isdeleted=*)");
			/* The filter gets freed in windows_agreement_delete() */
			dp->deleted_filter = slapi_str2filter( string_filter );
			slapi_ch_free_string(&string_filter);
		}

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_deleted_filter\n" );

		return dp->deleted_filter;
}

/* Returns a copy of the Slapi_DN pointer, no need to free it */
const Slapi_DN* windows_private_get_windows_subtree (const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_windows_subtree\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);
		
		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_windows_subtree\n" );
	
		return dp->windows_subtree;	
}

const char *
windows_private_get_windows_domain(const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_windows_domain\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);
		
		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_windows_domain\n" );
	
		return dp->windows_domain;	
}

static void
windows_private_set_windows_domain(const Repl_Agmt *ra, char *domain)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_windows_domain\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		dp->windows_domain = domain;
		
		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_windows_domain\n" );
	}

/* Returns a copy of the Slapi_DN pointer, no need to free it */
const Slapi_DN* windows_private_get_directory_subtree (const Repl_Agmt *ra)
{
		Dirsync_Private *dp;

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_directory_replarea\n" );

        PR_ASSERT(ra);

		dp = (Dirsync_Private *) agmt_get_priv(ra);
		PR_ASSERT (dp);

		LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_directory_replarea\n" );
	
		return dp->directory_subtree; 
}

/* Takes a copy of the sdn passed in */
void windows_private_set_windows_subtree (const Repl_Agmt *ra,Slapi_DN* sdn )
{

	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_windows_replarea\n" );

	PR_ASSERT(ra);
	PR_ASSERT(sdn);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	slapi_sdn_free(&dp->windows_subtree);
	dp->windows_subtree = sdn;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_windows_replarea\n" );
}

/* Takes a copy of the sdn passed in */
void windows_private_set_directory_subtree (const Repl_Agmt *ra,Slapi_DN* sdn )
{

	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_directory_replarea\n" );

	PR_ASSERT(ra);
	PR_ASSERT(sdn);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);
	
	slapi_sdn_free(&dp->directory_subtree);
	dp->directory_subtree = sdn;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_directory_replarea\n" );
}

PRBool windows_private_create_users(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_create_users\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_create_users\n" );

	return dp->create_users_from_dirsync;

}


void windows_private_set_create_users(const Repl_Agmt *ra, PRBool value)
{
	Dirsync_Private *dp;
	
	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_create_users\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	dp->create_users_from_dirsync = value;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_create_users\n" );

}

PRBool windows_private_create_groups(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_create_groups\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_create_groups\n" );

	return dp->create_groups_from_dirsync;

}


void windows_private_set_create_groups(const Repl_Agmt *ra, PRBool value)
{
	Dirsync_Private *dp;
	
	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_create_groups\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	dp->create_groups_from_dirsync = value;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_create_groups\n" );

}


int windows_private_get_one_way(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_one_way\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_one_way\n" );

	return dp->one_way;
}


void windows_private_set_one_way(const Repl_Agmt *ra, int value)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_one_way\n" );

	PR_ASSERT(ra);
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	dp->one_way = value;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_one_way\n" );
}


/* 
	This function returns the current Dirsync_Private that's inside 
	Repl_Agmt ra as a ldap control.

  */
LDAPControl* windows_private_dirsync_control(const Repl_Agmt *ra)
{

	LDAPControl *control = NULL;
	BerElement *ber;
	Dirsync_Private *dp;
	char iscritical = PR_TRUE;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_dirsync_control\n" );
	
	PR_ASSERT(ra);
	
	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);
	ber = 	ber_alloc();

	ber_printf( ber, "{iio}", dp->dirsync_flags, dp->dirsync_maxattributecount, dp->dirsync_cookie ? dp->dirsync_cookie : "", dp->dirsync_cookie_len );

	/* Use a regular directory server instead of a real AD - for testing */
	if (getenv("WINSYNC_USE_DS")) {
		iscritical = PR_FALSE;
	}
	slapi_build_control( REPL_DIRSYNC_CONTROL_OID, ber, iscritical, &control);

	ber_free(ber,1);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_dirsync_control\n" );


	return control;

}

/* 
	This function scans the array of controls and updates the Repl_Agmt's 
	Dirsync_Private if the dirsync control is found. 

*/
void windows_private_update_dirsync_control(const Repl_Agmt *ra,LDAPControl **controls )
{

	Dirsync_Private *dp;
    int foundDirsyncControl;
	int i;
	LDAPControl *dirsync = NULL;
	BerElement *ber = NULL;
	ber_int_t hasMoreData;
	ber_int_t maxAttributeCount;
	BerValue  *serverCookie = NULL;
#ifdef FOR_DEBUGGING
	int return_value = LDAP_SUCCESS;
#endif

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_update_dirsync_control\n" );

    PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);
	
	if (NULL != controls )
	{
		foundDirsyncControl = 0;
		for ( i = 0; (( controls[i] != NULL ) && ( !foundDirsyncControl )); i++ ) {
			foundDirsyncControl = !strcmp( controls[i]->ldctl_oid, REPL_DIRSYNC_CONTROL_OID );
		}

		if ( !foundDirsyncControl )
		{
#ifdef FOR_DEBUGGING
			return_value = LDAP_CONTROL_NOT_FOUND;
#endif
			goto choke;
		}
		else if (!controls[i-1]->ldctl_value.bv_val) {
#ifdef FOR_DEBUGGING
			return_value = LDAP_CONTROL_NOT_FOUND;
#endif
			goto choke;
		}
		else
		{
			dirsync = slapi_dup_control( controls[i-1]);
		}

		ber = ber_init( &dirsync->ldctl_value ) ;

		if (ber_scanf( ber, "{iiO}", &hasMoreData, &maxAttributeCount, &serverCookie) == LBER_ERROR)
		{
#ifdef FOR_DEBUGGING
			return_value =  LDAP_CONTROL_NOT_FOUND;
#endif
			goto choke;
		}

		slapi_ch_free_string(&dp->dirsync_cookie);
		dp->dirsync_cookie = ( char* ) slapi_ch_malloc(serverCookie->bv_len + 1);

		memcpy(dp->dirsync_cookie, serverCookie->bv_val, serverCookie->bv_len);
		dp->dirsync_cookie_len = (int) serverCookie->bv_len; /* XXX shouldn't cast? */

		/* dp->dirsync_maxattributecount = maxAttributeCount; We don't need to keep this */
		dp->dirsync_cookie_has_more = hasMoreData;

choke:
		ber_bvfree(serverCookie);
		ber_free(ber,1);
		ldap_control_free(dirsync);
	}
	else
	{
#ifdef FOR_DEBUGGING
		return_value = LDAP_CONTROL_NOT_FOUND;
#endif
	}

#ifdef FOR_DEBUGGING
	LDAPDebug1Arg( LDAP_DEBUG_TRACE, "<= windows_private_update_dirsync_control: rc=%d\n", return_value);
#else
	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_update_dirsync_control\n" );
#endif
}

PRBool windows_private_dirsync_has_more(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_dirsync_has_more\n" );

	PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_dirsync_has_more\n" );

	return dp->dirsync_cookie_has_more;

}

void windows_private_null_dirsync_cookie(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_null_dirsync_control\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	dp->dirsync_cookie_len = 0;
	slapi_ch_free_string(&dp->dirsync_cookie);
	dp->dirsync_cookie = NULL;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_null_dirsync_control\n" );
}

static 
Slapi_Mods *windows_private_get_cookie_mod(Dirsync_Private *dp, int modtype)
{
	Slapi_Mods *smods = NULL;
	smods = slapi_mods_new();

	slapi_mods_add( smods, modtype,
	 "nsds7DirsyncCookie", dp->dirsync_cookie_len , dp->dirsync_cookie);	

	return smods;

}


/*  writes the current cookie into dse.ldif under the replication agreement entry 
	returns: ldap result code of the operation. */
int 
windows_private_save_dirsync_cookie(const Repl_Agmt *ra)
{
	Dirsync_Private *dp = NULL;
    Slapi_PBlock *pb = NULL;
	Slapi_DN* sdn = NULL;
	int rc = 0;
	Slapi_Mods *mods = NULL;

    
  
	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_save_dirsync_cookie\n" );
	PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);


	pb = slapi_pblock_new ();
  
    mods = windows_private_get_cookie_mod(dp, LDAP_MOD_REPLACE);
    sdn = slapi_sdn_dup( agmt_get_dn_byref(ra) );

    slapi_modify_internal_set_pb_ext (pb, sdn, 
            slapi_mods_get_ldapmods_byref(mods), NULL, NULL, 
            repl_get_plugin_identity(PLUGIN_MULTIMASTER_REPLICATION), 0);
    slapi_modify_internal_pb (pb);

    slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);

    if (rc == LDAP_NO_SUCH_ATTRIBUTE)
    {	/* try again, but as an add instead */
		slapi_mods_free(&mods);
		mods = windows_private_get_cookie_mod(dp, LDAP_MOD_ADD);
		slapi_modify_internal_set_pb_ext (pb, sdn,
		        slapi_mods_get_ldapmods_byref(mods), NULL, NULL, 
		        repl_get_plugin_identity(PLUGIN_MULTIMASTER_REPLICATION), 0);
		slapi_modify_internal_pb (pb);
	
		slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
    }

	slapi_pblock_destroy (pb);
	slapi_mods_free(&mods);
	slapi_sdn_free(&sdn);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_save_dirsync_cookie\n" );
	return rc;
}

/*  reads the cookie in dse.ldif to the replication agreement entry
	returns: ldap result code of ldap operation, or 
			 LDAP_NO_SUCH_ATTRIBUTE. (this is the equilivent of a null cookie) */
int windows_private_load_dirsync_cookie(const Repl_Agmt *ra)
{
	Dirsync_Private *dp = NULL;
    Slapi_PBlock *pb = NULL;
  
	Slapi_DN* sdn = NULL;
	int rc = 0;
	Slapi_Entry *entry = NULL;
	Slapi_Attr *attr = NULL;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_load_dirsync_cookie\n" );
	PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);


	pb = slapi_pblock_new ();
	sdn = slapi_sdn_dup( agmt_get_dn_byref(ra) );
	

	rc  = slapi_search_internal_get_entry(sdn, NULL, &entry, 
		                                  repl_get_plugin_identity (PLUGIN_MULTIMASTER_REPLICATION));

	if (rc == 0)
	{
		rc= slapi_entry_attr_find( entry, type_nsds7DirsyncCookie, &attr );
		if (attr)
		{
			struct berval **vals;
			rc = slapi_attr_get_bervals_copy(attr, &vals );
		
			if (vals)
			{
				dp->dirsync_cookie_len = (int)  (vals[0])->bv_len;
				slapi_ch_free_string(&dp->dirsync_cookie);

				dp->dirsync_cookie = ( char* ) slapi_ch_malloc(dp->dirsync_cookie_len + 1);
				memcpy(dp->dirsync_cookie,(vals[0]->bv_val), (vals[0])->bv_len+1);

			}

			ber_bvecfree(vals);
			/* we do not free attr */

		}
		else
		{
			rc = LDAP_NO_SUCH_ATTRIBUTE;
		}
	}

	if (entry)
	{
		 slapi_entry_free(entry);
	}
	
	slapi_sdn_free( &sdn);
	slapi_pblock_destroy (pb);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_load_dirsync_cookie\n" );

	return rc;
}

/* get returns a pointer to the structure - do not free */
Slapi_Entry *windows_private_get_raw_entry(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_raw_entry\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_raw_entry\n" );

	return dp->raw_entry;
}

/* this is passin - windows_private owns the pointer, not a copy */
void windows_private_set_raw_entry(const Repl_Agmt *ra, Slapi_Entry *e)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_raw_entry\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	/* If the keep raw entry flag is set, just free the passed
	 * in entry and leave the current raw entry in place. */
	if (windows_private_get_keep_raw_entry(ra)) {
		slapi_entry_free(e);
	} else {
		slapi_entry_free(dp->raw_entry);
		dp->raw_entry = e;
	}

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_raw_entry\n" );
}

/* Setting keep to 1 will cause the current raw entry to remain, even if
 * windows_private_set_raw_entry() is called.  This behavior will persist
 * until this flag is set back to 0. */
void windows_private_set_keep_raw_entry(const Repl_Agmt *ra, int keep)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_keep_raw_entry\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	dp->keep_raw_entry = keep;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_keep_raw_entry\n" );
}

int windows_private_get_keep_raw_entry(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_keep_raw_entry\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_keep_raw_entry\n" );

	return dp->keep_raw_entry;
}

void *windows_private_get_api_cookie(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_api_cookie\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_api_cookie\n" );

	return dp->api_cookie;
}

void windows_private_set_api_cookie(Repl_Agmt *ra, void *api_cookie)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_api_cookie\n" );

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);
	dp->api_cookie = api_cookie;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_api_cookie\n" );
}

time_t
windows_private_get_sync_interval(const Repl_Agmt *ra)
{
	Dirsync_Private *dp;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_get_sync_interval\n" );

	PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_get_sync_interval\n" );

	return dp->sync_interval;	
}

void
windows_private_set_sync_interval(Repl_Agmt *ra, char *str)
{
	Dirsync_Private *dp;
	time_t tmpval = 0;

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "=> windows_private_set_sync_interval\n" );

	PR_ASSERT(ra);

	dp = (Dirsync_Private *) agmt_get_priv(ra);
	PR_ASSERT (dp);

	if (str && (tmpval = (time_t)atol(str))) {
		dp->sync_interval = tmpval;
	}

	LDAPDebug0Args( LDAP_DEBUG_TRACE, "<= windows_private_set_sync_interval\n" );
}

/* an array of function pointers */
static void **_WinSyncAPI = NULL;
static int maxapiidx = WINSYNC_PLUGIN_VERSION_1_END;
#define DECL_WINSYNC_API_FUNC(idx,thetype,thefunc) \
    thetype thefunc = (_WinSyncAPI && (idx <= maxapiidx) && _WinSyncAPI[idx]) ? \
        (thetype)_WinSyncAPI[idx] : NULL;

void
windows_plugin_init(Repl_Agmt *ra)
{
    void *cookie = NULL;
    winsync_plugin_init_cb initfunc = NULL;

	LDAPDebug0Args( LDAP_DEBUG_PLUGIN, "--> windows_plugin_init_start -- begin\n");

    /* if the function pointer array is null, get the functions - we will
       call init once per replication agreement, but will only grab the
       api once */
    if(NULL == _WinSyncAPI) {
        if (slapi_apib_get_interface(WINSYNC_v2_0_GUID, &_WinSyncAPI) ||
            (NULL == _WinSyncAPI))
        {
            LDAPDebug1Arg( LDAP_DEBUG_PLUGIN,
                           "<-- windows_plugin_init_start -- no windows plugin API registered for GUID [%s] -- end\n",
                           WINSYNC_v2_0_GUID);
        } else if (_WinSyncAPI) {
            LDAPDebug1Arg( LDAP_DEBUG_PLUGIN,
                           "<-- windows_plugin_init_start -- found windows plugin API registered for GUID [%s] -- end\n",
                           WINSYNC_v2_0_GUID);
            maxapiidx = WINSYNC_PLUGIN_VERSION_2_END;
        }
    }

    if (NULL == _WinSyncAPI) { /* no v2 interface - look for v1 */
        if (slapi_apib_get_interface(WINSYNC_v1_0_GUID, &_WinSyncAPI) ||
            (NULL == _WinSyncAPI))
        {
            LDAPDebug1Arg( LDAP_DEBUG_PLUGIN,
                           "<-- windows_plugin_init_start -- no windows plugin API registered for GUID [%s] -- end\n",
                           WINSYNC_v1_0_GUID);
            return;
        } else {
            LDAPDebug1Arg( LDAP_DEBUG_PLUGIN,
                           "<-- windows_plugin_init_start -- found windows plugin API registered for GUID [%s] -- end\n",
                           WINSYNC_v1_0_GUID);
        }
	}

    initfunc = (winsync_plugin_init_cb)_WinSyncAPI[WINSYNC_PLUGIN_INIT_CB];
    if (initfunc) {
        cookie = (*initfunc)(windows_private_get_directory_subtree(ra),
                             windows_private_get_windows_subtree(ra));
    }
    windows_private_set_api_cookie(ra, cookie);

	LDAPDebug0Args( LDAP_DEBUG_PLUGIN, "<-- windows_plugin_init_start -- end\n");
    return;
}

void
winsync_plugin_call_dirsync_search_params_cb(const Repl_Agmt *ra, const char *agmt_dn,
                                             char **base, int *scope, char **filter,
                                             char ***attrs, LDAPControl ***serverctrls)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_DIRSYNC_SEARCH_CB,winsync_search_params_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), agmt_dn, base, scope, filter,
               attrs, serverctrls);

    return;
}

void
winsync_plugin_call_pre_ad_search_cb(const Repl_Agmt *ra, const char *agmt_dn,
                                     char **base, int *scope, char **filter,
                                     char ***attrs, LDAPControl ***serverctrls)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_SEARCH_CB,winsync_search_params_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), agmt_dn, base, scope, filter,
               attrs, serverctrls);

    return;
}

void
winsync_plugin_call_pre_ds_search_entry_cb(const Repl_Agmt *ra, const char *agmt_dn,
                                           char **base, int *scope, char **filter,
                                           char ***attrs, LDAPControl ***serverctrls)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_SEARCH_ENTRY_CB,winsync_search_params_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), agmt_dn, base, scope, filter,
               attrs, serverctrls);

    return;
}

void
winsync_plugin_call_pre_ds_search_all_cb(const Repl_Agmt *ra, const char *agmt_dn,
                                         char **base, int *scope, char **filter,
                                         char ***attrs, LDAPControl ***serverctrls)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_SEARCH_ALL_CB,winsync_search_params_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), agmt_dn, base, scope, filter,
               attrs, serverctrls);

    return;
}

void
winsync_plugin_call_pre_ad_mod_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                       Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                       Slapi_Mods *smods, int *do_modify)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_MOD_USER_CB,winsync_pre_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, do_modify);

    return;
}

void
winsync_plugin_call_pre_ad_mod_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                        Slapi_Mods *smods, int *do_modify)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_MOD_GROUP_CB,winsync_pre_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, do_modify);

    return;
}

void
winsync_plugin_call_pre_ds_mod_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                       Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                       Slapi_Mods *smods, int *do_modify)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_MOD_USER_CB,winsync_pre_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, do_modify);

    return;
}

void
winsync_plugin_call_pre_ds_mod_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                        Slapi_Mods *smods, int *do_modify)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_MOD_GROUP_CB,winsync_pre_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, do_modify);

    return;
}

void
winsync_plugin_call_pre_ds_add_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                       Slapi_Entry *ad_entry, Slapi_Entry *ds_entry)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_ADD_USER_CB,winsync_pre_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry);

    return;
}

void
winsync_plugin_call_pre_ds_add_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_DS_ADD_GROUP_CB,winsync_pre_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry);

    return;
}

void
winsync_plugin_call_get_new_ds_user_dn_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                          Slapi_Entry *ad_entry, char **new_dn_string,
                                          const Slapi_DN *ds_suffix, const Slapi_DN *ad_suffix)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_GET_NEW_DS_USER_DN_CB,winsync_get_new_dn_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               new_dn_string, ds_suffix, ad_suffix);

    return;
}

void
winsync_plugin_call_get_new_ds_group_dn_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                           Slapi_Entry *ad_entry, char **new_dn_string,
                                           const Slapi_DN *ds_suffix, const Slapi_DN *ad_suffix)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_GET_NEW_DS_GROUP_DN_CB,winsync_get_new_dn_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               new_dn_string, ds_suffix, ad_suffix);

    return;
}

void
winsync_plugin_call_pre_ad_mod_user_mods_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                            const Slapi_DN *local_dn,
                                            const Slapi_Entry *ds_entry,
                                            LDAPMod * const *origmods,
                                            Slapi_DN *remote_dn, LDAPMod ***modstosend)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_MOD_USER_MODS_CB,winsync_pre_ad_mod_mods_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, local_dn,
               ds_entry, origmods, remote_dn, modstosend);

    return;
}

void
winsync_plugin_call_pre_ad_mod_group_mods_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                             const Slapi_DN *local_dn,
                                             const Slapi_Entry *ds_entry,
                                             LDAPMod * const *origmods,
                                             Slapi_DN *remote_dn, LDAPMod ***modstosend)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_MOD_GROUP_MODS_CB,winsync_pre_ad_mod_mods_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, local_dn,
               ds_entry, origmods, remote_dn, modstosend);

    return;
}

int
winsync_plugin_call_can_add_entry_to_ad_cb(const Repl_Agmt *ra, const Slapi_Entry *local_entry,
                                           const Slapi_DN *remote_dn)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_CAN_ADD_ENTRY_TO_AD_CB,winsync_can_add_to_ad_cb,thefunc);

    if (!thefunc) {
        return 1; /* default is entry can be added to AD */
    }

    return (*thefunc)(windows_private_get_api_cookie(ra), local_entry, remote_dn);
}

void
winsync_plugin_call_begin_update_cb(const Repl_Agmt *ra, const Slapi_DN *ds_subtree,
                                    const Slapi_DN *ad_subtree, int is_total)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_BEGIN_UPDATE_CB,winsync_plugin_update_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ds_subtree, ad_subtree, is_total);

    return;
}

void
winsync_plugin_call_end_update_cb(const Repl_Agmt *ra, const Slapi_DN *ds_subtree,
                                  const Slapi_DN *ad_subtree, int is_total)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_END_UPDATE_CB,winsync_plugin_update_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ds_subtree, ad_subtree, is_total);

    return;
}

void
winsync_plugin_call_destroy_agmt_cb(const Repl_Agmt *ra,
                                    const Slapi_DN *ds_subtree,
                                    const Slapi_DN *ad_subtree)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_DESTROY_AGMT_CB,winsync_plugin_destroy_agmt_cb,thefunc);

    if (thefunc) {
        (*thefunc)(windows_private_get_api_cookie(ra), ds_subtree, ad_subtree);
    }

    return;
}

void
winsync_plugin_call_post_ad_mod_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                        Slapi_Mods *smods, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_MOD_USER_CB,winsync_post_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, result);

    return;
}

void
winsync_plugin_call_post_ad_mod_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                         Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                         Slapi_Mods *smods, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_MOD_GROUP_CB,winsync_post_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, result);

    return;
}

void
winsync_plugin_call_post_ds_mod_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                        Slapi_Mods *smods, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_DS_MOD_USER_CB,winsync_post_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, result);

    return;
}

void
winsync_plugin_call_post_ds_mod_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                         Slapi_Entry *ad_entry, Slapi_Entry *ds_entry,
                                         Slapi_Mods *smods, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_DS_MOD_GROUP_CB,winsync_post_mod_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, smods, result);

    return;
}

void
winsync_plugin_call_post_ds_add_user_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                        Slapi_Entry *ad_entry, Slapi_Entry *ds_entry, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_DS_ADD_USER_CB,winsync_post_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, result);

    return;
}

void
winsync_plugin_call_post_ds_add_group_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                         Slapi_Entry *ad_entry, Slapi_Entry *ds_entry, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_DS_ADD_GROUP_CB,winsync_post_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, ad_entry,
               ds_entry, result);

    return;
}

void
winsync_plugin_call_pre_ad_add_user_cb(const Repl_Agmt *ra, Slapi_Entry *ad_entry,
                                       Slapi_Entry *ds_entry)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_ADD_USER_CB,winsync_pre_ad_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ad_entry, ds_entry);

    return;
}

void
winsync_plugin_call_pre_ad_add_group_cb(const Repl_Agmt *ra, Slapi_Entry *ad_entry,
                                        Slapi_Entry *ds_entry)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_PRE_AD_ADD_GROUP_CB,winsync_pre_ad_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ad_entry, ds_entry);

    return;
}

void
winsync_plugin_call_post_ad_add_user_cb(const Repl_Agmt *ra, Slapi_Entry *ad_entry,
                                        Slapi_Entry *ds_entry, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_ADD_USER_CB,winsync_post_ad_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ad_entry, ds_entry, result);

    return;
}

void
winsync_plugin_call_post_ad_add_group_cb(const Repl_Agmt *ra, Slapi_Entry *ad_entry,
                                         Slapi_Entry *ds_entry, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_ADD_GROUP_CB,winsync_post_ad_add_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), ad_entry, ds_entry, result);

    return;
}

void
winsync_plugin_call_post_ad_mod_user_mods_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                             const Slapi_DN *local_dn,
                                             const Slapi_Entry *ds_entry,
                                             LDAPMod * const *origmods,
                                             Slapi_DN *remote_dn, LDAPMod **modstosend, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_MOD_USER_MODS_CB,winsync_post_ad_mod_mods_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, local_dn,
               ds_entry, origmods, remote_dn, modstosend, result);

    return;
}

void
winsync_plugin_call_post_ad_mod_group_mods_cb(const Repl_Agmt *ra, const Slapi_Entry *rawentry,
                                              const Slapi_DN *local_dn,
                                              const Slapi_Entry *ds_entry,
                                              LDAPMod * const *origmods,
                                              Slapi_DN *remote_dn, LDAPMod **modstosend, int *result)
{
    DECL_WINSYNC_API_FUNC(WINSYNC_PLUGIN_POST_AD_MOD_GROUP_MODS_CB,winsync_post_ad_mod_mods_cb,thefunc);

    if (!thefunc) {
        return;
    }

    (*thefunc)(windows_private_get_api_cookie(ra), rawentry, local_dn,
               ds_entry, origmods, remote_dn, modstosend, result);

    return;
}

/* #define WINSYNC_TEST_IPA */
#ifdef WINSYNC_TEST_IPA

#include "ipa-winsync.c"
#include "ipa-winsync-config.c"

#endif
