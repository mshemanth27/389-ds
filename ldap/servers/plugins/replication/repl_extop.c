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

 
#include "slapi-plugin.h"
#include "repl.h"
#include "repl5.h"
#include "repl5_prot_private.h"
#include "cl5_api.h"


/*
 * repl_extop.c - there are two types of functions in this file:
 *              - Code that implements an extended operation plugin.
 *                The replication DLL arranges for this code to
 *                be called when a StartNSDS50ReplicationRequest
 *                or an EndNSDS50ReplicationRequest extended operation
 *                is received.
 *              - Code that sends extended operations on an already-
 *                established client connection.
 *
 * The requestValue portion of the StartNSDS50ReplicationRequest
 * looks like this:
 *
 *     requestValue ::= SEQUENCE { 
 *         replProtocolOID LDAPOID, 
 *         replicatedTree LDAPDN, 
           supplierRUV  OCTET STRING
 *         referralURLs SET of LDAPURL OPTIONAL 
 *         csn OCTET STRING OPTIONAL 
 *     } 
 *
 */
static int check_replica_id_uniqueness(Replica *replica, RUV *supplier_ruv);
static multimaster_mtnode_extension *replica_config_get_mtnode_by_dn(const char *dn);

static int 
encode_ruv (BerElement *ber, const RUV *ruv)
{
    int rc = LDAP_SUCCESS;
    struct berval **bvals = NULL;
    
    PR_ASSERT (ber);
    PR_ASSERT (ruv);

    if (ruv_to_bervals(ruv, &bvals) != 0)
    {
        rc = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    if (ber_printf(ber, "[V]", bvals) == -1)
    {
        rc = LDAP_ENCODING_ERROR;
		goto done;
    }
   
    rc = LDAP_SUCCESS;

done:
    if (bvals)
        ber_bvecfree (bvals);    
    
    return rc;
}

/* The data_guid and data parameters should only be set if we
 * are talking with a 9.0 replica. */
static struct berval *
create_ReplicationExtopPayload(const char *protocol_oid,
	const char *repl_root, char **extra_referrals, CSN *csn,
	int send_end, const char *data_guid, const struct berval *data)
{
	struct berval *req_data = NULL;
	BerElement *tmp_bere = NULL;
	int rc = 0;
    Object  *repl_obj = NULL, *ruv_obj = NULL;
	Replica *repl;
    RUV *ruv;
    Slapi_DN *sdn = NULL;

	PR_ASSERT(protocol_oid != NULL || send_end);
	PR_ASSERT(repl_root != NULL);

	/* Create the request data */

	if ((tmp_bere = der_alloc()) == NULL)
	{
		rc = LDAP_ENCODING_ERROR;
		goto loser;
	}
	if (!send_end)
	{
		if (ber_printf(tmp_bere, "{ss", protocol_oid, repl_root) == -1)
		{
			rc = LDAP_ENCODING_ERROR;
			goto loser;
		}
	}
	else
	{
		if (ber_printf(tmp_bere, "{s", repl_root) == -1)
		{
			rc = LDAP_ENCODING_ERROR;
			goto loser;
		}
	}

    sdn = slapi_sdn_new_dn_byref(repl_root);
    repl_obj = replica_get_replica_from_dn (sdn);
    if (repl_obj == NULL)
    {
        rc = LDAP_OPERATIONS_ERROR;
        goto loser;
    }

    repl = (Replica*)object_get_data (repl_obj);
    PR_ASSERT (repl);
    ruv_obj = replica_get_ruv (repl);
    if (ruv_obj == NULL)
    {
        rc = LDAP_OPERATIONS_ERROR;
        goto loser;
    }
	ruv = object_get_data(ruv_obj);
	PR_ASSERT(ruv);

    /* send supplier's ruv so that consumer can build its own referrals. 
       In case of total protocol, it is also used as consumer's ruv once 
       protocol successfully completes */
	/* We need to encode and send each time the local ruv in case we have changed it */
	rc = encode_ruv (tmp_bere, ruv);
	if (rc != 0)
	{
		goto loser;
	}

	if (!send_end)
	{
		char s[CSN_STRSIZE];		
		ReplicaId rid;
		char *local_replica_referral[2] = {0};
		char **referrals_to_send = NULL;
		/* Add the referral URL(s), if present */
		rid = replica_get_rid(repl);
		if (!ruv_contains_replica(ruv, rid))
		{
			/*
			 * In the event that there is no RUV component for this replica (e.g.
			 * if the database was just loaded from LDIF and no local CSNs have been
			 * generated), then we need to explicitly add this server to the list
			 * of referrals, since it wouldn't have been sent with the RUV.
			 */
			local_replica_referral[0] = (char *)multimaster_get_local_purl(); /* XXXggood had to cast away const */
		}
		charray_merge(&referrals_to_send, extra_referrals, 0);
		charray_merge(&referrals_to_send, local_replica_referral, 0);
		if (NULL != referrals_to_send)
		{
			if (ber_printf(tmp_bere, "[v]", referrals_to_send) == -1)
			{
				rc = LDAP_ENCODING_ERROR;
				goto loser;
			}
			slapi_ch_free((void **)&referrals_to_send);
		}
		/* Add the CSN */
		PR_ASSERT(NULL != csn);
		if (ber_printf(tmp_bere, "s", csn_as_string(csn,PR_FALSE,s)) == -1)
		{
			rc = LDAP_ENCODING_ERROR;
			goto loser;
		}
	}

	/* If we have data to send to a 9.0 style replica, set it here. */
	if (data_guid && data) {
		if (ber_printf(tmp_bere, "sO", data_guid, data) == -1)
		{
			rc = LDAP_ENCODING_ERROR;
			goto loser;
		}
	}

	if (ber_printf(tmp_bere, "}") == -1)
	{
		rc = LDAP_ENCODING_ERROR;
		goto loser;
	}

	if (ber_flatten(tmp_bere, &req_data) == -1)
	{
		rc = LDAP_LOCAL_ERROR;
		goto loser;
	}
	/* Success */
	goto done;

loser:
	/* Free stuff we allocated */
	if (NULL != req_data)
	{
		ber_bvfree(req_data); req_data = NULL;
	}

done:
	if (NULL != tmp_bere)
	{
		ber_free(tmp_bere, 1); tmp_bere = NULL;
	}
	if (NULL != sdn)
	{
        slapi_sdn_free (&sdn); /* Put on stack instead of allocating? */
	}
    if (NULL != repl_obj)
	{
        object_release (repl_obj);
	}
    if (NULL != ruv_obj)
	{
        object_release (ruv_obj);
	}
	return req_data;
}


struct berval *
NSDS50StartReplicationRequest_new(const char *protocol_oid,
	const char *repl_root, char **extra_referrals, CSN *csn)
{
	return(create_ReplicationExtopPayload(protocol_oid,
		repl_root, extra_referrals, csn, 0, 0, 0));
}

struct berval *
NSDS90StartReplicationRequest_new(const char *protocol_oid,
        const char *repl_root, char **extra_referrals, CSN *csn,
	const char *data_guid, const struct berval *data)
{
	return(create_ReplicationExtopPayload(protocol_oid,
                repl_root, extra_referrals, csn, 0, data_guid, data));
}

struct berval *
NSDS50EndReplicationRequest_new(char *repl_root)
{
	return(create_ReplicationExtopPayload(NULL, repl_root, NULL, NULL, 1, 0, 0));
}

static int 
decode_ruv (BerElement *ber, RUV **ruv)
{
    int rc = -1;
    struct berval **bvals = NULL;

    PR_ASSERT (ber && ruv);

 	if (ber_scanf(ber, "[V]", &bvals) == LBER_DEFAULT)
	{
		goto done;
	}

    if (ruv_init_from_bervals(bvals, ruv) != 0)
    {
		goto done;
    }

    rc = 0;
done:
    if (bvals)
        ber_bvecfree (bvals);

    return rc;
}

/*
 * Decode an NSDS50 or NSDS90 Start Replication Request extended
 * operation. Returns 0 on success, -1 on decoding error.
 * The caller is responsible for freeing protocol_oid,
 * repl_root, referrals, csn, data_guid, and data.
 */
static int
decode_startrepl_extop(Slapi_PBlock *pb, char **protocol_oid, char **repl_root, 
                       RUV **supplier_ruv, char ***extra_referrals, char **csnstr,
                       char **data_guid, struct berval **data, int *is90)
{
	char *extop_oid = NULL;
	struct berval *extop_value = NULL;
	BerElement *tmp_bere = NULL;
	ber_len_t len;
	int rc = 0;

	PR_ASSERT (pb && protocol_oid && repl_root && supplier_ruv && extra_referrals && csnstr && data_guid && data);
    
	*protocol_oid = NULL;
	*repl_root = NULL;
	*supplier_ruv = NULL;
	*extra_referrals = NULL;
	*csnstr = NULL;
    
	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_OID, &extop_oid);
	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_VALUE, &extop_value);

	if (NULL == extop_oid ||
		((strcmp(extop_oid, REPL_START_NSDS50_REPLICATION_REQUEST_OID) != 0) &&
		(strcmp(extop_oid, REPL_START_NSDS90_REPLICATION_REQUEST_OID) != 0)) ||
		NULL == extop_value || NULL == extop_value->bv_val)
	{
		/* bogus */
		rc = -1;
		goto free_and_return;
	}

	/* Set a flag to let the caller know if this is a 9.0 style start extop */
	if (strcmp(extop_oid, REPL_START_NSDS90_REPLICATION_REQUEST_OID) == 0)
	{
		*is90 = 1;
	}
	else
	{
		*is90 = 0;
	}

	if ((tmp_bere = ber_init(extop_value)) == NULL)
	{
		rc = -1;
		goto free_and_return;
	}
	if (ber_scanf(tmp_bere, "{") == LBER_ERROR)
	{
		rc = -1;
		goto free_and_return;
	}
	/* Get the required protocol OID and root of replicated subtree */
	if (ber_get_stringa(tmp_bere, protocol_oid) == LBER_DEFAULT)
	{
		rc = -1;
		goto free_and_return;
	}
	if (ber_get_stringa(tmp_bere, repl_root) == LBER_DEFAULT)
	{
		rc = -1;
		goto free_and_return;
	}

	/* get supplier's ruv */
	if (decode_ruv (tmp_bere, supplier_ruv) == -1)
	{
		rc = -1;
		goto free_and_return;
	}

	/* Get the optional set of referral URLs */
	if (ber_peek_tag(tmp_bere, &len) == LBER_SET)
	{
		if (ber_scanf(tmp_bere, "[v]", extra_referrals) == LBER_ERROR)
		{
			rc = -1;
			goto free_and_return;
		}
	}
	/* Get the CSN */
	if (ber_get_stringa(tmp_bere, csnstr) == LBER_ERROR)
	{
		rc = -1;
		goto free_and_return;
	}
	/* Get the optional replication session callback data. */
	if (ber_peek_tag(tmp_bere, &len) == LBER_OCTETSTRING)
	{
		if (ber_get_stringa(tmp_bere, data_guid) == LBER_ERROR)
		{
			rc = -1;
			goto free_and_return;
		}
		/* If a data_guid was specified, data must be specified as well. */
		if (ber_peek_tag(tmp_bere, &len) == LBER_OCTETSTRING)
		{
			if (ber_get_stringal(tmp_bere, data) == LBER_ERROR)
			{
				rc = -1;
				goto free_and_return;
			}
		}
		else
		{
			rc = -1;
			goto free_and_return;
		}
	}
	if (ber_scanf(tmp_bere, "}") == LBER_ERROR)
	{
		rc = -1;
		goto free_and_return;
	}

free_and_return:
	if (-1 == rc)
	{
		/* Free everything when error encountered */
 
		/* slapi_ch_free accepts NULL pointer */
		slapi_ch_free ((void**)protocol_oid);
		slapi_ch_free ((void**)repl_root);
		slapi_ch_array_free (*extra_referrals);
        *extra_referrals = NULL;
		slapi_ch_free ((void**)csnstr);

		if (*supplier_ruv)
		{
			ruv_destroy (supplier_ruv);
		}

	}
	if (NULL != tmp_bere)
	{
		ber_free(tmp_bere, 1);
		tmp_bere = NULL;
	}

	return rc;
}


/*
 * Decode an NSDS50 End Replication Request extended
 * operation. Returns 0 on success, -1 on decoding error.
 * The caller is responsible for freeing repl_root.
 */
static int
decode_endrepl_extop(Slapi_PBlock *pb, char **repl_root)
{
	char *extop_oid = NULL;
	struct berval *extop_value = NULL;
	BerElement *tmp_bere = NULL;
	int rc = 0;

	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_OID, &extop_oid);
	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_VALUE, &extop_value);

	if (NULL == extop_oid ||
		strcmp(extop_oid, REPL_END_NSDS50_REPLICATION_REQUEST_OID) != 0 ||
		NULL == extop_value || NULL == extop_value->bv_val)
	{
		/* bogus */
		rc = -1;
		goto free_and_return;
	}

	if ((tmp_bere = ber_init(extop_value)) == NULL)
	{
		rc = -1;
		goto free_and_return;
	}
	if (ber_scanf(tmp_bere, "{") == LBER_DEFAULT)
	{
		rc = -1;
		goto free_and_return;
	}
	/* Get the required root of replicated subtree */
	if (ber_get_stringa(tmp_bere, repl_root) == LBER_DEFAULT)
	{
		rc = -1;
		goto free_and_return;
	}
	if (ber_scanf(tmp_bere, "}") == LBER_DEFAULT)
	{
		rc = -1;
		goto free_and_return;
	}

free_and_return:
	if (NULL != tmp_bere)
		{
		ber_free(tmp_bere, 1);
		tmp_bere = NULL;
	}

	return rc;
}




/*
 * Decode an NSDS50ReplicationResponse or NSDS90ReplicationResponse
 * extended response. The extended response just contains a sequence
 * that contains:
 * 1) An integer response code
 * 2) An optional array of bervals representing the consumer
 *    replica's update vector
 * 3) An optional data guid and data string if this is a 9.0
 *    style response
 * Returns 0 on success, or -1 if the response could not be parsed.
 */
int
decode_repl_ext_response(struct berval *bvdata, int *response_code,
	struct berval ***ruv_bervals, char **data_guid, struct berval **data)
{
	BerElement *tmp_bere = NULL;
	int return_value = 0;

	PR_ASSERT(NULL != response_code);
	PR_ASSERT(NULL != ruv_bervals);

	if (NULL == bvdata || NULL == response_code || NULL == ruv_bervals ||
		NULL == data_guid || NULL == data || NULL == bvdata->bv_val)
	{
		return_value = -1;
	}
	else
	{
		ber_len_t len;
		ber_int_t temp_response_code = 0;
		*ruv_bervals = NULL;
		if ((tmp_bere = ber_init(bvdata)) == NULL)
		{
			return_value = -1;
		}
		else if (ber_scanf(tmp_bere, "{e", &temp_response_code) == LBER_ERROR)
		{
			return_value = -1;
		}
		else if (ber_peek_tag(tmp_bere, &len) == LBER_SEQUENCE)
		{
			if (ber_scanf(tmp_bere, "{V}", ruv_bervals) == LBER_ERROR)
			{
				return_value = -1;
			}
		}
		/* Check for optional data from replication session callback */
		if (ber_peek_tag(tmp_bere, &len) == LBER_OCTETSTRING)
		{
			if (ber_scanf(tmp_bere, "aO}", data_guid, data) == LBER_ERROR)
			{
				return_value = -1;
			}
		}
		else if (ber_scanf(tmp_bere, "}") == LBER_ERROR)
		{
			return_value = -1;
		}

		*response_code = (int)temp_response_code;
	}
	if (0 != return_value)
	{
		if (NULL != ruv_bervals && NULL != *ruv_bervals)
		{
			ber_bvecfree(*ruv_bervals);
		}
	}
	if (NULL != tmp_bere)
	{
		ber_free(tmp_bere, 1); tmp_bere = NULL;
	}
	return return_value;
}


/*
 * This plugin entry point is called whenever a
 * StartNSDS50ReplicationRequest is received.
 */
int
multimaster_extop_StartNSDS50ReplicationRequest(Slapi_PBlock *pb)
{
	int return_value = SLAPI_PLUGIN_EXTENDED_NOT_HANDLED;
	ber_int_t response = 0;
	int rc = 0;
	BerElement *resp_bere = NULL;
	struct berval *resp_bval = NULL;
	char *protocol_oid = NULL;
	char *repl_root = NULL;
	Slapi_DN *repl_root_sdn = NULL;
	char **referrals = NULL;
	Object *replica_object = NULL;
	Replica *replica = NULL;
	void *conn;
	consumer_connection_extension *connext = NULL;
	char *replicacsnstr = NULL;
	CSN *replicacsn = NULL;
	int zero = 0;
	int one = 1;
	RUV *ruv = NULL;
	struct berval **ruv_bervals = NULL;
	CSNGen *gen = NULL;
	Object *gen_obj = NULL;
	Slapi_DN *bind_sdn = NULL;
	char *bind_dn = NULL;
	Object *ruv_object = NULL;
	RUV *supplier_ruv = NULL;
	PRUint64 connid = 0;
	int opid = 0;
	PRBool isInc = PR_FALSE; /* true if incremental update */
	char *locking_purl = NULL; /* the supplier contacting us */
	char *current_purl = NULL; /* the supplier which already has exclusive access */
	char locking_session[24];
	char *data_guid = NULL;
	struct berval *data = NULL;
	int is90 = 0;

	/* Decode the extended operation */
	if (decode_startrepl_extop(pb, &protocol_oid, &repl_root, &supplier_ruv,
			&referrals, &replicacsnstr, &data_guid, &data, &is90) == -1)
	{
		response = NSDS50_REPL_DECODING_ERROR;
		goto send_response;
	}
	if (NULL == protocol_oid || NULL == repl_root || NULL == replicacsnstr)
	{
		response = NSDS50_REPL_DECODING_ERROR;
		goto send_response;
	}

	slapi_pblock_get(pb, SLAPI_CONN_ID, &connid);
	slapi_pblock_get(pb, SLAPI_OPERATION_ID, &opid);

	/*
	 * Get a hold of the connection extension object and
	 * make sure it's there.
	 */
	slapi_pblock_get(pb, SLAPI_CONNECTION, &conn);
	connext = (consumer_connection_extension *)repl_con_get_ext(
			REPL_CON_EXT_CONN, conn);
	if (NULL == connext)
	{
		/* Something bad happened. Don't go any further */
		response = NSDS50_REPL_INTERNAL_ERROR;
		goto send_response;
	}

	/* Verify that we know about this replication protocol OID */
	if (strcmp(protocol_oid, REPL_NSDS50_INCREMENTAL_PROTOCOL_OID) == 0)
	{
		if (repl_session_plugin_call_recv_acquire_cb(repl_root, 0 /* is_total == FALSE */,
				data_guid, data))
		{
			slapi_ch_free_string(&data_guid);
			ber_bvfree(data);
			data = NULL;
			response = NSDS50_REPL_BACKOFF;
			goto send_response;
		} else {
			slapi_ch_free_string(&data_guid);
			ber_bvfree(data);
			data = NULL;
		}

		/* Stash info that this is an incremental update session */
		connext->repl_protocol_version = REPL_PROTOCOL_50_INCREMENTAL;
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
			"conn=%" NSPRIu64 " op=%d repl=\"%s\": Begin incremental protocol\n",
			connid, opid, repl_root);
		isInc = PR_TRUE;
	}
	else if (strcmp(protocol_oid, REPL_NSDS50_TOTAL_PROTOCOL_OID) == 0)
	{
		if (repl_session_plugin_call_recv_acquire_cb(repl_root, 1 /* is_total == TRUE */,
				data_guid, data))
		{
			slapi_ch_free_string(&data_guid);
			ber_bvfree(data);
			data = NULL;
			response = NSDS50_REPL_DISABLED;
			goto send_response;
		} else {
			slapi_ch_free_string(&data_guid);
			ber_bvfree(data);
			data = NULL;
		}

		/* Stash info that this is a total update session */
		if (NULL != connext)
		{
			connext->repl_protocol_version = REPL_PROTOCOL_50_TOTALUPDATE;
		}
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
				"conn=%" NSPRIu64 " op=%d repl=\"%s\": Begin total protocol\n",
				connid, opid, repl_root);
		isInc = PR_FALSE;
	}
	else if (strcmp(protocol_oid, REPL_NSDS71_INCREMENTAL_PROTOCOL_OID) == 0)
	{
		/* Stash info that this is an incremental update session */
		connext->repl_protocol_version = REPL_PROTOCOL_50_INCREMENTAL;
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
			"conn=%" NSPRIu64 " op=%d repl=\"%s\": Begin 7.1 incremental protocol\n",
			connid, opid, repl_root);
		isInc = PR_TRUE;
	}
	else if (strcmp(protocol_oid, REPL_NSDS71_TOTAL_PROTOCOL_OID) == 0)
	{
		/* Stash info that this is a total update session */
		if (NULL != connext)
		{
			connext->repl_protocol_version = REPL_PROTOCOL_71_TOTALUPDATE;
		}
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
				"conn=%" NSPRIu64 " op=%d repl=\"%s\": Begin 7.1 total protocol\n",
				connid, opid, repl_root);
		isInc = PR_FALSE;
	}
	else
	{
		/* Unknown replication protocol */
		response = NSDS50_REPL_UNKNOWN_UPDATE_PROTOCOL;
		goto send_response;
	}

	/* Verify that repl_root names a valid replicated area */
	if ((repl_root_sdn = slapi_sdn_new_dn_byval(repl_root)) == NULL)
	{
		response = NSDS50_REPL_INTERNAL_ERROR;
		goto send_response;
	}

	/* see if this replica is being configured and wait for it */
	if (replica_is_being_configured(repl_root))
	{
        slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
				"conn=%" NSPRIu64 " op=%d replica=\"%s\": "
				"Replica is being configured: try again later\n",
				connid, opid, repl_root);
		response = NSDS50_REPL_REPLICA_BUSY;
		goto send_response;
	}

	replica_object = replica_get_replica_from_dn(repl_root_sdn);
	if (NULL != replica_object)
	{
		replica = object_get_data(replica_object);
	}
	if (NULL == replica)
	{
		response = NSDS50_REPL_NO_SUCH_REPLICA;
		goto send_response;
	}

	if (REPL_PROTOCOL_50_TOTALUPDATE == connext->repl_protocol_version)
	{
		/* If total update has been initiated against other replicas or
		 * this replica is already being initialized, we should return
		 * an error immediately. */
		if (replica_is_state_flag_set(replica,
							REPLICA_TOTAL_EXCL_SEND|REPLICA_TOTAL_EXCL_RECV))
		{
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name,
				"%s: total update on is initiated on the replica.  Cannot execute the total update from other master.\n", repl_root);
			response = NSDS50_REPL_REPLICA_BUSY;
			goto send_response;
		}
		else
		{
			replica_set_state_flag (replica, REPLICA_TOTAL_EXCL_RECV, 0);
		}
	}

    /* check that this replica is not a 4.0 consumer */
    if (replica_is_legacy_consumer (replica))
    {
        response = NSDS50_REPL_LEGACY_CONSUMER;
        goto send_response;
    }

	/* Check that bind dn is authorized to supply replication updates */
	slapi_pblock_get(pb, SLAPI_CONN_DN, &bind_dn); /* bind_dn is allocated */
	bind_sdn = slapi_sdn_new_dn_passin(bind_dn);
	if (replica_is_updatedn(replica, bind_sdn) == PR_FALSE)
	{
		response = NSDS50_REPL_PERMISSION_DENIED;
		goto send_response;
	}

	/* Check received CSN for clock skew */
	gen_obj = replica_get_csngen(replica);
	if (NULL != gen_obj)
	{
		gen = object_get_data(gen_obj);
		if (NULL != gen)
		{
			replicacsn = csn_new_by_string(replicacsnstr);
			if (NULL != replicacsn)
			{
				/* ONREPL - we used to manage clock skew here. However, csn generator
				   code already does it. The csngen also manages local skew caused by
				   system clock reset, so to keep it consistent, I removed code from here */
				/* update the state of the csn generator */
				rc = replica_update_csngen_state_ext (replica, supplier_ruv, replicacsn); /* too much skew */
				if (rc == CSN_LIMIT_EXCEEDED)
				{
					response = NSDS50_REPL_EXCESSIVE_CLOCK_SKEW;
					slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name,
									"conn=%" NSPRIu64 " op=%d repl=\"%s\": "
									"Excessive clock skew from supplier RUV\n",
									connid, opid, repl_root);
					goto send_response;
				}
				else if (rc != 0)
				{
					/* Oops, problem csn or ruv format, or memory, or .... */
					response = NSDS50_REPL_INTERNAL_ERROR;
					goto send_response;
				}

			}
			else
			{
				/* Oops, csnstr couldn't be converted */
				response = NSDS50_REPL_INTERNAL_ERROR;
				goto send_response;
			}
		}
		else
		{
			/* Oops, no csn generator */
			response = NSDS50_REPL_INTERNAL_ERROR;
			goto send_response;
		}
	}
	else
	{
		/* Oops, no csn generator object */
		response = NSDS50_REPL_INTERNAL_ERROR;
		goto send_response;
	}

	if (check_replica_id_uniqueness(replica, supplier_ruv) != 0){
        slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
				"conn=%" NSPRIu64 " op=%d repl=\"%s\": "
				"Replica has same replicaID %d as supplier\n",
				connid, opid, repl_root, replica_get_rid(replica));
		response = NSDS50_REPL_REPLICAID_ERROR;
		goto send_response;
	}
	
	/* Attempt to acquire exclusive access to the replicated area */
	/* Since partial URL is always the master, this locking_purl does not
	 * help us to know the true locker when it is a hub. Change to use
	 * the session's conn id and op id to identify the the supplier.
	 */
	/* junkrc = ruv_get_first_id_and_purl(supplier_ruv, &junkrid, &locking_purl); */
	PR_snprintf(locking_session, sizeof(locking_session), "conn=%" NSPRIu64 " id=%d", connid, opid);
	locking_purl = &locking_session[0];
	if (replica_get_exclusive_access(replica, &isInc, connid, opid,
									 locking_purl,
									 &current_purl) == PR_FALSE)
	{
		locking_purl = NULL; /* no dangling pointers */
		response = NSDS50_REPL_REPLICA_BUSY;
		goto send_response;
	}
	else
	{
		locking_purl = NULL; /* no dangling pointers */
		/* Stick the replica object pointer in the connection extension */
		connext->replica_acquired = (void *)replica_object;
        replica_object = NULL;
	}

	/* If this is incremental protocol get replica's ruv to return to the supplier */
    if (connext->repl_protocol_version == REPL_PROTOCOL_50_INCREMENTAL)
    {
	    ruv_object = replica_get_ruv(replica);
	    if (NULL != ruv_object)
	    {
		    ruv = object_get_data(ruv_object);
		    (void)ruv_to_bervals(ruv, &ruv_bervals);
		    object_release(ruv_object);
	    }
    }

	/*
	 * Save the supplier ruv in the connection extension so it can
	 * either (a) be installed upon successful initialization (if this
	 * is a total update session) or used to update referral information
	 * for new replicas that show up in the supplier's RUV.
	 */
	/*
	 * the supplier_ruv may have been set before, so free it here
	 * (in ruv_copy_and_destroy)
	 */
	ruv_copy_and_destroy(&supplier_ruv, (RUV **)&connext->supplier_ruv);

    if (connext->repl_protocol_version == REPL_PROTOCOL_50_INCREMENTAL)
    {
		/* The supplier ruv may have changed, so let's update the referrals */
		consumer5_set_mapping_tree_state_for_replica(replica, connext->supplier_ruv);
    }
    else /* full protocol */
    {
		char *mtnstate = slapi_mtn_get_state(repl_root_sdn);
		char **mtnreferral = slapi_mtn_get_referral(repl_root_sdn);

		/* richm 20041118 - we do not want to reap tombstones while there is
		   a total update in progress, so shut it down */
		replica_set_tombstone_reap_stop(replica, PR_TRUE);

		/* richm 20010831 - set the mapping tree to the referral state *before*
		   we invoke slapi_start_bulk_import - see bug 556992 -
		   slapi_start_bulk_import sets the database offline, if an operation comes
		   in while the database is offline but the mapping tree is not referring yet,
		   the server gets confused
		*/
		/* During a total update we refer *all* operations */
		repl_set_mtn_state_and_referrals(repl_root_sdn, STATE_REFERRAL,
										 connext->supplier_ruv, NULL, referrals);
		/* LPREPL - check the return code. 
		 * But what do we do if mapping tree could not be updated ? */

		/* start the bulk import */
        slapi_pblock_set (pb, SLAPI_TARGET_SDN, repl_root_sdn);
        rc = slapi_start_bulk_import (pb);
        if (rc != LDAP_SUCCESS)
        {
            response = NSDS50_REPL_INTERNAL_ERROR;
			/* reset the mapping tree state to what it was before
			   we tried to do the bulk import if mtnstate exists */
			if (mtnstate) {
				repl_set_mtn_state_and_referrals(repl_root_sdn, mtnstate,
											 NULL, NULL, mtnreferral);
				slapi_ch_free_string(&mtnstate);
			}
			charray_free(mtnreferral);
			mtnreferral = NULL;
			
		    goto send_response;    
        }
		slapi_ch_free_string(&mtnstate);
		charray_free(mtnreferral);
		mtnreferral = NULL;
    }

	response = NSDS50_REPL_REPLICA_READY;
	/* Set the "is replication session" flag in the connection extension */
	slapi_pblock_set( pb, SLAPI_CONN_IS_REPLICATION_SESSION, &one );
	connext->isreplicationsession = 1;
	/* Save away the connection */
	slapi_pblock_get(pb, SLAPI_CONNECTION, &connext->connection);

send_response:
	if (connext && replica &&
		(REPL_PROTOCOL_50_TOTALUPDATE == connext->repl_protocol_version))
	{
		replica_set_state_flag (replica, REPLICA_TOTAL_EXCL_RECV, 1);
	}
    if (response != NSDS50_REPL_REPLICA_READY)
    {
		int resp_log_level = SLAPI_LOG_FATAL;
		char purlstr[1024] = {0};
		if (current_purl)
			PR_snprintf(purlstr, sizeof(purlstr), " locked by %s for %s update", current_purl,
					isInc ? "incremental" : "total");

		/* Don't log replica busy as errors - these are almost always not
		   errors - use the replication monitoring tools to determine if
		   a replica is not converging, then look for pathological replica
		   busy errors by turning on the replication log level.  We also
		   don't want to log replica backoff as an error, as that response
                   is only used when a replication session hook wants a master to
                   go into incremental backoff mode. */
		if ((response == NSDS50_REPL_REPLICA_BUSY) || (response == NSDS50_REPL_BACKOFF)) {
			resp_log_level = SLAPI_LOG_REPL;
		}

		slapi_log_error (resp_log_level, repl_plugin_name,
			"conn=%" NSPRIu64 " op=%d replica=\"%s\": "
			"Unable to acquire replica: error: %s%s\n",
			connid, opid,
			(replica ? slapi_sdn_get_dn(replica_get_root(replica)) : "unknown"),
			protocol_response2string (response), purlstr);

		/* enable tombstone reap again since the total update failed */
		replica_set_tombstone_reap_stop(replica, PR_FALSE);
	}

	/* Call any registered replica session reply callback.  We
	 * want to reject the updates if the return value is non-0. */
	if (repl_session_plugin_call_reply_acquire_cb(replica ?
		slapi_sdn_get_ndn(replica_get_root(replica)) : "",
		((isInc == PR_TRUE) ? 0 : 1), &data_guid, &data))
	{
		slapi_ch_free_string(&data_guid);
		ber_bvfree(data);
		data = NULL;
		response = NSDS50_REPL_BACKOFF;
	}

	/* Send the response */
	if ((resp_bere = der_alloc()) == NULL)
	{
	    /* ONREPL - not sure what we suppose to do here */
	}
	ber_printf(resp_bere, "{e", response);
	if (NULL != ruv_bervals)
	{
		ber_printf(resp_bere, "{V}", ruv_bervals);
	}
	/* Add extra data from replication session callback if necessary */
	if (is90 && data_guid && data)
	{
		ber_printf(resp_bere, "sO", data_guid, data);
	}

	ber_printf(resp_bere, "}");
	ber_flatten(resp_bere, &resp_bval);

	if (is90)
	{
		slapi_pblock_set(pb, SLAPI_EXT_OP_RET_OID, REPL_NSDS90_REPLICATION_RESPONSE_OID);
	}
	else
	{
		slapi_pblock_set(pb, SLAPI_EXT_OP_RET_OID, REPL_NSDS50_REPLICATION_RESPONSE_OID);
	}

	slapi_pblock_set(pb, SLAPI_EXT_OP_RET_VALUE, resp_bval);
	slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name,
					"conn=%" NSPRIu64 " op=%d repl=\"%s\": "
					"%s: response=%d rc=%d\n",
					connid, opid, repl_root,
					is90 ? "StartNSDS90ReplicationRequest" :
					"StartNSDS50ReplicationRequest", response, rc);
	slapi_send_ldap_result(pb, LDAP_SUCCESS, NULL, NULL, 0, NULL);

	return_value = SLAPI_PLUGIN_EXTENDED_SENT_RESULT;

	/* Free any data allocated by the replication
	 * session reply callback. */
	slapi_ch_free_string(&data_guid);
	ber_bvfree(data);
	data = NULL;

	slapi_ch_free_string(&current_purl);

	/* protocol_oid */
	/* slapi_ch_free accepts NULL pointer */
	slapi_ch_free((void **)&protocol_oid);

	/* repl_root */
	slapi_ch_free((void **)&repl_root);

	/* supplier's ruv */
	if (supplier_ruv)
	{
		ruv_destroy (&supplier_ruv);
	}
	/* referrals (char **) */
	slapi_ch_array_free(referrals);

	/* replicacsnstr */
	slapi_ch_free((void **)&replicacsnstr);

	/* repl_root_sdn */
	slapi_sdn_free(&repl_root_sdn);

	if (NSDS50_REPL_REPLICA_READY != response)
	{
		/*
		 * Something went wrong, and we never told the other end that the
		 * replica had been acquired, so we'd better release it.
		 */
		if (NULL != connext && NULL != connext->replica_acquired)
		{
            Object *r_obj = (Object*)connext->replica_acquired;
			replica_relinquish_exclusive_access((Replica*)object_get_data (r_obj),
												connid, opid);
		}
		/* Remove any flags that would indicate repl session in progress */
		if (NULL != connext)
		{
			connext->repl_protocol_version = REPL_PROTOCOL_UNKNOWN;
			connext->isreplicationsession = 0;
		}
		slapi_pblock_set( pb, SLAPI_CONN_IS_REPLICATION_SESSION, &zero );
	}
	/* Release reference to replica_object */
	if (NULL != replica_object)
	{
		object_release(replica_object);
	}
	/* bind_sdn */
	if (NULL != bind_sdn)
	{
		slapi_sdn_free(&bind_sdn);
	}
	/* Release reference to gen_obj */
	if (NULL != gen_obj)
	{
		object_release(gen_obj);
	}
	/* replicacsn */
	if (NULL != replicacsn)
	{
		csn_free(&replicacsn);
	}
	/* resp_bere */
	if (NULL != resp_bere)
	{
		ber_free(resp_bere, 1);
	}
	/* resp_bval */
	if (NULL != resp_bval)
	{
		ber_bvfree(resp_bval);
	}
	/* ruv_bervals */
	if (NULL != ruv_bervals)
	{
		ber_bvecfree(ruv_bervals);
	}

	return return_value;
}

/*
 * This plugin entry point is called whenever an
 * EndNSDS50ReplicationRequest is received.
 * XXXggood this code is not finished.
 */
int
multimaster_extop_EndNSDS50ReplicationRequest(Slapi_PBlock *pb)
{
	int return_value = SLAPI_PLUGIN_EXTENDED_NOT_HANDLED;
	char *repl_root = NULL;
	Slapi_DN *repl_root_sdn = NULL;
	BerElement *resp_bere = NULL;
	struct berval *resp_bval = NULL;
	ber_int_t response;
	void *conn;
	consumer_connection_extension *connext = NULL;
	PRUint64 connid = 0;
	int opid=-1;

	/* Decode the extended operation */
	if (decode_endrepl_extop(pb, &repl_root) == -1)
	{
		response = NSDS50_REPL_DECODING_ERROR;
	}
	else
	{

		/* First, verify that the current connection is a replication session */
		/* XXXggood - do we need to wait around for any pending updates to complete?
		   I suppose it's possible that the end request may arrive asynchronously, before
		   we're really done processing all the updates.
		 */
		/* Get a hold of the connection extension object */
		slapi_pblock_get(pb, SLAPI_CONNECTION, &conn);
		connext = (consumer_connection_extension *)repl_con_get_ext(
				REPL_CON_EXT_CONN, conn);
		if (NULL != connext && NULL != connext->replica_acquired)
		{
			int zero= 0;
			Replica *r = (Replica*)object_get_data ((Object*)connext->replica_acquired);

			/* if this is total protocol we need to install suppliers ruv for the replica */
			if (connext->repl_protocol_version == REPL_PROTOCOL_50_TOTALUPDATE)
			{
				/* We no longer need to refer all operations... 
				 * and update the referrals on the mapping tree node
				 */
				consumer5_set_mapping_tree_state_for_replica(r, NULL);

				/* LPREPL - First we clear the total in progress flag
				   Like this we know it's a normal termination of import. This is required by
				   the replication function that responds to backend state change.
				   If the flag is not clear, the callback knows that replication should not be
				   enabled again */
				replica_set_state_flag(r, REPLICA_TOTAL_IN_PROGRESS, PR_TRUE /* clear  flag */);

                /* slapi_pblock_set (pb, SLAPI_TARGET_DN, repl_root); */
                /* Verify that repl_root names a valid replicated area */
                if ((repl_root_sdn = slapi_sdn_new_dn_byref(repl_root)) == NULL)
                {
                    response = NSDS50_REPL_INTERNAL_ERROR;
                    goto send_response;
                }
                slapi_pblock_set (pb, SLAPI_TARGET_SDN, repl_root_sdn);

                slapi_stop_bulk_import (pb); 

                /* ONREPL - this is a bit of a hack. Once bulk import is finished,
                   the replication function that responds to backend state change 
                   will be called. That function normally do all ruv and changelog 
                   processing. However, in the case of replica initalization, it
                   will not do the right thing because supplier does not send its
                   ruv tombstone to the consumer. So that's why we need to do the
                   second processing here.
                   The supplier does not send its RUV entry because it could be
                   more up to date then the data send to the consumer.
                   The best solution I think, would be to "fake" on the supplier
                   an entry that corresponds to the ruv sent to the consumer and then
                   send it as part of the data */

                if (cl5GetState () == CL5_STATE_OPEN)
                {
                    cl5DeleteDBSync (connext->replica_acquired);
                }

				replica_set_ruv (r, connext->supplier_ruv);
				connext->supplier_ruv = NULL;

                /* if changelog is enabled, we need to log a dummy change for the
                   smallest csn in the new ruv, so that this replica ca supply
                   other servers.
                */      
                if (cl5GetState () == CL5_STATE_OPEN)
                {                    
                    replica_log_ruv_elements (r);
                }

			    /* ONREPL code that dealt with new RUV, etc was moved into the code
                   that enables replication when a backend comes back online. This
                   code is called once the bulk import is finished */

				/* allow reaping again */
				replica_set_tombstone_reap_stop(r, PR_FALSE);

			}
			else if (connext->repl_protocol_version == REPL_PROTOCOL_50_INCREMENTAL)
			{
				/* The ruv from the supplier may have changed. Report the change on the
					consumer side */
				replica_update_ruv_consumer(r, connext->supplier_ruv);
			}

			/* Relinquish control of the replica */
			slapi_pblock_get (pb, SLAPI_OPERATION_ID, &opid);
			if (opid) slapi_pblock_get (pb, SLAPI_CONN_ID, &connid);
			replica_relinquish_exclusive_access(r, connid, opid);
			object_release ((Object*)connext->replica_acquired);
			connext->replica_acquired = NULL;
			connext->isreplicationsession= 0;
			slapi_pblock_set( pb, SLAPI_CONN_IS_REPLICATION_SESSION, &zero );
			response = NSDS50_REPL_REPLICA_RELEASE_SUCCEEDED;
			/* Outbound replication agreements need to all be restarted now */
			/* XXXGGOOD RESTART REEPL AGREEMENTS */
		} else {
			/* Unless bail out, we return uninitialized response */
			goto free_and_return; 
		}
	}
send_response:
	/* Send the response code */
	if ((resp_bere = der_alloc()) == NULL)
	{
		goto free_and_return;
	}
	ber_printf(resp_bere, "{e}", response);
	ber_flatten(resp_bere, &resp_bval);
	slapi_pblock_set(pb, SLAPI_EXT_OP_RET_OID, REPL_NSDS50_REPLICATION_RESPONSE_OID);
	slapi_pblock_set(pb, SLAPI_EXT_OP_RET_VALUE, resp_bval);
	slapi_send_ldap_result(pb, LDAP_SUCCESS, NULL, NULL, 0, NULL);

	return_value = SLAPI_PLUGIN_EXTENDED_SENT_RESULT;

free_and_return:
	/* repl_root */
	slapi_ch_free((void **)&repl_root);

	slapi_sdn_free(&repl_root_sdn);

	/* BerElement */
	if (NULL != resp_bere)
	{
		ber_free(resp_bere, 1);
	}
	/* response */
	if (NULL != resp_bval)
	{
		ber_bvfree(resp_bval);
	}

	return return_value;
}

/*
 *  Return the mtnode extension of the dn
 */
static multimaster_mtnode_extension *
replica_config_get_mtnode_by_dn(const char *dn)
{
	Slapi_DN *sdn;
	mapping_tree_node *mtnode;
	multimaster_mtnode_extension *ext = NULL;

	sdn = slapi_sdn_new_dn_byval(dn);
	mtnode = slapi_get_mapping_tree_node_by_dn (sdn);
	if (mtnode)	{
		/* check if the replica object already exists in the subtree */
		ext = (multimaster_mtnode_extension *)repl_con_get_ext (REPL_CON_EXT_MTNODE, mtnode);
	}
	slapi_sdn_free (&sdn);

	return ext;
}

/*
 * Decode the ber element passed to us by the cleanAllRUV task
 */
static int
decode_cleanruv_payload(struct berval *extop_value, char **payload)
{
	BerElement *tmp_bere = NULL;
	int rc = 0;

	if ((tmp_bere = ber_init(extop_value)) == NULL){
		rc = -1;
		goto free_and_return;
	}
	if (ber_scanf(tmp_bere, "{") == LBER_ERROR){
		rc = -1;
		goto free_and_return;
	}
	if (ber_get_stringa(tmp_bere, payload) == LBER_DEFAULT){
		rc = -1;
		goto free_and_return;
	}

	if (ber_scanf(tmp_bere, "}") == LBER_ERROR){
		rc = -1;
		goto free_and_return;
	}

free_and_return:
	if (-1 == rc){
		slapi_ch_free_string(payload);
	}
	if (NULL != tmp_bere){
		ber_free(tmp_bere, 1);
		tmp_bere = NULL;
	}
	return rc;
}

/*
 *  Process the REPL_CLEANRUV_OID extended operation.
 *
 *  The payload consists of the replica ID, and the repl root dn.  Since this is
 *  basically a replication operation, it could of originated here and bounced
 *  back from another master.  So check the rid against the "cleaned_rid".  If
 *  it's a match, then we were already here, and we can just return success.
 *
 *  Otherwise, we the set the cleaned_rid from the payload, fire off extended ops
 *  to all the replica agreements on this replica.  Then perform the actual
 *  cleanruv_task on this replica.
 */
int
multimaster_extop_cleanruv(Slapi_PBlock *pb){
	multimaster_mtnode_extension *mtnode_ext;
	PRThread *thread = NULL;
	Repl_Connection *conn;
	const Slapi_DN *dn;
	Replica *r = NULL;
	Object *agmt_obj;
	Repl_Agmt *agmt;
	ConnResult crc;
	cleanruv_data *data = NULL;
	struct berval *extop_value;
	char *extop_oid;
	char *repl_root;
	char *payload = NULL;
	char *iter;
	int send_msgid = 0;
	int agmt_count = 0;
	int rid = 0;
	int rc = 0;

	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_OID, &extop_oid);
	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_VALUE, &extop_value);

	if (NULL == extop_oid || strcmp(extop_oid, REPL_CLEANRUV_OID) != 0 ||
	    NULL == extop_value || NULL == extop_value->bv_val){
		/* something is wrong, error out */
		return -1;
	}
	/*
	 *  Extract the rid and repl_root from the payload
	 */
	if(decode_cleanruv_payload(extop_value, &payload)){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to decode payload.  Aborting ext op\n");
		return -1;
	}
	rid = atoi(ldap_utf8strtok_r(payload, ":", &iter));
	repl_root = ldap_utf8strtok_r(iter, ":", &iter);

	/*
	 *  If we already cleaned this server, just return success
	 */
	if(is_cleaned_rid(rid)){
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name, "cleanAllRUV_extop: rid (%d) has already been cleaned, skipping\n",rid);
		return rc;
	} else {
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: cleaning rid (%d)...\n", rid);
		set_cleaned_rid(rid);
	}

	/*
	 *  Get the node, so we can get the replica and its agreements
	 */
	if((mtnode_ext = replica_config_get_mtnode_by_dn(repl_root)) == NULL){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to get replication node "
			"from (%s), aborting operation\n", repl_root);
		return -1;
	}
	if (mtnode_ext->replica)
		object_acquire (mtnode_ext->replica);
	if (mtnode_ext->replica == NULL){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: replica is missing from (%s), "
			"aborting operation\n",repl_root);
		rc = LDAP_OPERATIONS_ERROR;
		goto free_and_return;
	}
	r = (Replica*)object_get_data (mtnode_ext->replica);
	/*
	 * Send out extended ops to each repl agreement
	 */
	agmt_obj = agmtlist_get_first_agreement_for_replica (r);
	while (agmt_obj)
	{
		agmt = (Repl_Agmt*)object_get_data (agmt_obj);
		if(!agmt_is_enabled(agmt)){
			agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
			continue;
		}
		dn = agmt_get_dn_byref(agmt);
		conn = (Repl_Connection *)agmt_get_connection(agmt);
		if(conn == NULL){
			/* no connection for this agreement, move on to the next agmt */
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: the replica (%s), is "
				"missing the connection.  This replica will not be cleaned.\n", slapi_sdn_get_dn(dn));
			agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
			continue;
		}
		crc = conn_connect(conn);
		if (CONN_OPERATION_FAILED == crc ){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to connect "
				"to repl agreement connection (%s), error %d\n",slapi_sdn_get_dn(dn), ACQUIRE_TRANSIENT_ERROR);
			rc = LDAP_OPERATIONS_ERROR;
		} else if (CONN_SSL_NOT_ENABLED == crc){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to acquire "
				"repl agmt connection (%s), error %d\n",slapi_sdn_get_dn(dn), ACQUIRE_FATAL_ERROR);
			rc = LDAP_OPERATIONS_ERROR;
		} else {
			conn_cancel_linger(conn);
			crc = conn_send_extended_operation(conn, REPL_CLEANRUV_OID, extop_value, NULL, &send_msgid);
			if (CONN_OPERATION_SUCCESS != crc){
				slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to send "
					"clean_ruv extended op to repl agmt (%s), error %d\n", slapi_sdn_get_dn(dn), crc);
				rc = LDAP_OPERATIONS_ERROR;
			} else {
				/* success */
				slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: successfully sent "
					"extended op to (%s)\n",slapi_sdn_get_dn(dn) );
				agmt_count++;
			}
			conn_start_linger(conn);
		}
		if(crc != CONN_OPERATION_SUCCESS){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: replica (%s) has not "
					        "been cleaned.  You will need to rerun the CLEANALLRUV task on this replica\n",
					        slapi_sdn_get_dn(dn) );
			rc = LDAP_OPERATIONS_ERROR;
		}
		agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
	}

	/* now clean the ruv */
	replica_execute_cleanruv_task_ext(mtnode_ext->replica, rid);

free_and_return:

	if(rc == 0 && agmt_count > 0){
		/*
		 *  Launch the cleanruv monitoring thread.  Once all the replicas are cleaned it will release the rid
		 */
		data = (cleanruv_data*)slapi_ch_calloc(1, sizeof(cleanruv_data));
		if (data == NULL) {
			slapi_log_error( SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to allocate "
				"cleanruv_Data\n");
			return -1;
		}
		data->repl_obj = mtnode_ext->replica;
		data->rid = rid;

		thread = PR_CreateThread(PR_USER_THREAD, replica_cleanallruv_monitor_thread,
				(void *)data, PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD,
				PR_UNJOINABLE_THREAD, SLAPD_DEFAULT_THREAD_STACKSIZE);
		if (thread == NULL) {
			slapi_log_error( SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: unable to create cleanAllRUV "
				"monitoring thread.  Aborting task.\n");
		}
	} else if (rc == 0){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: Successfully Finished.\n");
	} else {
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanALLRUV_extop: failed to clean rid (%d), error (%d)\n",rid, rc);
	}

	if (mtnode_ext->replica)
		object_release (mtnode_ext->replica);

	return rc;
}

/*
 *  Process the REPL_RELEASERUV_OID extended operation
 *
 *  Once, all the replicas in the replication farm have been cleaned, then
 *  we need to "release" the cleaned_rid, or else we will reject all updates
 *  that come from that rid until we restart the server.
 *
 *  We set the cleaned_ruv to zero(invalid rid), and then fire off extended
 *  operations to all of the replicas
 */
int
multimaster_extop_releaseruv(Slapi_PBlock *pb){
	multimaster_mtnode_extension *mtnode_ext;
	Repl_Connection *conn;
	const Slapi_DN *dn;
	Replica *r = NULL;
	Object *agmt_obj;
	Repl_Agmt *agmt;
	ConnResult crc;
	struct berval *extop_value;
	char *payload = NULL;
	char *extop_oid;
	char *repl_root;
	char *iter;
	int send_msgid = 0;
	int rid = 0;
	int rc = 0;

	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_OID, &extop_oid);
	slapi_pblock_get(pb, SLAPI_EXT_OP_REQ_VALUE, &extop_value);

	if (NULL == extop_oid || strcmp(extop_oid, REPL_RELEASERUV_OID) != 0 ||
		NULL == extop_value || NULL == extop_value->bv_val){
		/* something is wrong, error out */
		return -1;
	}

	if(decode_cleanruv_payload(extop_value, &payload)){
		slapi_log_error(SLAPI_LOG_FATAL,repl_plugin_name, "releaseRUV_extop: failed to decode payload, aborting ext op.\n");
		return -1;
	}
	rid = atoi(ldap_utf8strtok_r(payload, ":", &iter));
	repl_root = ldap_utf8strtok_r(iter, ":", &iter);

	/*
	 *  If we already released this ruv, just return.
	 */
	if(is_released_rid(rid) || is_already_released_rid()){
		slapi_log_error(SLAPI_LOG_REPL, repl_plugin_name, "cleanAllRUV_extop: rid (%d) has already been released, skipping.\n",rid);
		return 0;
	} else {
		/* set the released rid, and trigger trimming */
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: releasing rid (%d)...\n", rid);
		set_released_rid((int)rid);
		trigger_cl_trimming();
	}

	if((mtnode_ext = replica_config_get_mtnode_by_dn(repl_root)) == NULL){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "releaseRUV_extop: failed to get node "
			"from replication root dn(%s), aborting operation.\n", repl_root);
		return -1;
	}
	if (mtnode_ext->replica)
		object_acquire (mtnode_ext->replica);
	if (mtnode_ext->replica == NULL){
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "releaseRUV_extop: replica is missing from (%s), "
			"aborting operation.\n", repl_root);
		rc = LDAP_OPERATIONS_ERROR;
		goto free_and_return;
	}
	r = (Replica*)object_get_data (mtnode_ext->replica);
	/*
	 *  Loop over the agreements, and send out extended ops
	 */
	agmt_obj = agmtlist_get_first_agreement_for_replica (r);
	while (agmt_obj)
	{
		agmt = (Repl_Agmt*)object_get_data (agmt_obj);
		if(!agmt_is_enabled(agmt)){
			agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
			continue;
		}
		dn = agmt_get_dn_byref(agmt);
		conn = (Repl_Connection *)agmt_get_connection(agmt);
		if(conn == NULL){
			/* no connection for this agreement, log error, and move on */
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: the replica (%s), is "
				"missing the connection.  This replica will not be released.\n", slapi_sdn_get_dn(dn));
			agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
			continue;
		}
		crc = conn_connect(conn);
		if (CONN_OPERATION_FAILED == crc ){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "releaseRUV_extop: failed to connect "
				"to repl agreement connection (%s), error %d\n",slapi_sdn_get_dn(dn), ACQUIRE_TRANSIENT_ERROR);
			rc = LDAP_OPERATIONS_ERROR;
		} else if (CONN_SSL_NOT_ENABLED == crc){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "releaseRUV_extop: failed to acquire "
				"repl agmt connection (%s), error %d\n",slapi_sdn_get_dn(dn), ACQUIRE_FATAL_ERROR);
			rc = LDAP_OPERATIONS_ERROR;
		} else {
			conn_cancel_linger(conn);
			crc = conn_send_extended_operation(conn, REPL_RELEASERUV_OID, extop_value, NULL, &send_msgid);
			if (CONN_OPERATION_SUCCESS != crc){
				slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: failed to send "
					"releaseRUV extended op to repl agmt (%s), error %d\n",	slapi_sdn_get_dn(dn), crc);
				rc = LDAP_OPERATIONS_ERROR;
			} else {
				/* success */
				slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: successfully sent "
					"releaseRUV extended op to (%s)\n",slapi_sdn_get_dn(dn) );
				rc = 0;
			}
			conn_start_linger(conn);
		}
		if(crc){
			slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: replica (%s) has not "
				"been released.  You will need to rerun the task.\n",
				slapi_sdn_get_dn(dn) );
		}
		agmt_obj = agmtlist_get_next_agreement_for_replica (r, agmt_obj);
	}

free_and_return:
	/*
	 *  Set the rid as "ALREADY_RELEASED, and remove the cleaned ruv
	 */
	if(rc == 0){
		set_released_rid(ALREADY_RELEASED);
		delete_cleaned_rid();
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: Successfully released rid (%d)\n", rid);
	} else {
		slapi_log_error(SLAPI_LOG_FATAL, repl_plugin_name, "cleanAllRUV_extop: Failed to release rid(%d), error (%d), "
			"please retry the task.\n",rid, rc);
	}

	if(mtnode_ext->replica)
		object_release (mtnode_ext->replica);

	return rc;
}

/*
 * This plugin entry point is a noop entry
 * point. It's used when registering extops that
 * are only used as responses. We'll never receive
 * one of those, unsolicited, but we still want to
 * register them so they appear in the
 * supportedextension attribute in the root DSE.
 */
int
extop_noop(Slapi_PBlock *pb)
{
	return SLAPI_PLUGIN_EXTENDED_NOT_HANDLED;
}


static int
check_replica_id_uniqueness(Replica *replica, RUV *supplier_ruv)
{
	ReplicaId local_rid = replica_get_rid(replica);
	ReplicaId sup_rid = 0;
	char *sup_purl = NULL;

	if (ruv_get_first_id_and_purl(supplier_ruv, &sup_rid, &sup_purl) == RUV_SUCCESS) {
		/* ReplicaID Uniqueness is checked only on Masters */
		if ((replica_get_type(replica) == REPLICA_TYPE_UPDATABLE) &&
			 (sup_rid == local_rid)) {
			return 1;
		}
	}
	return 0;
}

	
	
