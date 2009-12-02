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
 * Copyright (C) 2009 Red Hat, Inc.
 * Copyright (C) 2009 Hewlett-Packard Development Company, L.P.
 * All rights reserved.
 *
 * Contributors:
 *   Hewlett-Packard Development Company, L.P.
 *     Bugfix for bug #193297
 *     Bugfix for bug #201275
 *
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

/* bind.c - decode an ldap bind operation and pass it to a backend db */

/*
 * Copyright (c) 1995 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif
#include "slap.h"
#include "fe.h"

#include "pratom.h"
#include <sasl.h>

static void log_bind_access(
    Slapi_PBlock *pb, 
    const char* dn, 
    int method, 
    int version,
    const char *saslmech,
    const char *msg
);


/* 
 * Function: is_root_dn_pw
 * 
 * Returns: 1 if the password for the root dn is correct.
 *          0 otherwise.
 * dn must be normalized
 *
 */
static int
is_root_dn_pw( const char *dn, const Slapi_Value *cred )
{
	int rv= 0;
	char *rootpw = config_get_rootpw();
	if ( rootpw == NULL || !slapi_dn_isroot( dn ) )
	{
	       rv = 0;
	}
	else
	{
		Slapi_Value rdnpwbv;
		Slapi_Value *rdnpwvals[2];
		slapi_value_init_string(&rdnpwbv,rootpw);
		rdnpwvals[ 0 ] = &rdnpwbv;
		rdnpwvals[ 1 ] = NULL;
		rv = slapi_pw_find_sv( rdnpwvals, cred ) == 0;
		value_done(&rdnpwbv);
	}
	slapi_ch_free_string( &rootpw );
	return rv;
}

void
do_bind( Slapi_PBlock *pb )
{
    BerElement	*ber = pb->pb_op->o_ber;
    int		err, isroot;
    ber_tag_t 	method = LBER_DEFAULT;
    ber_int_t	version = -1;
    int		auth_response_requested = 0;
    int		pw_response_requested = 0;
    char		*dn = NULL, *saslmech = NULL;
    struct berval	cred = {0};
    Slapi_Backend		*be = NULL;
    ber_tag_t rc;
    Slapi_DN sdn;
    Slapi_Entry *referral;
    char errorbuf[BUFSIZ];
    char **supported, **pmech;
    char authtypebuf[256]; /* >26 (strlen(SLAPD_AUTH_SASL)+SASL_MECHNAMEMAX+1) */
    Slapi_Entry *bind_target_entry = NULL;
    int auto_bind = 0;
    int minssf = 0;

    LDAPDebug( LDAP_DEBUG_TRACE, "do_bind\n", 0, 0, 0 );

    /*
     * Parse the bind request.  It looks like this:
     *
     *	BindRequest ::= SEQUENCE {
     *		version		INTEGER,		 -- version
     *		name		DistinguishedName,	 -- dn
     *		authentication	CHOICE {
     *			simple		[0] OCTET STRING, -- passwd
     *			krbv42ldap	[1] OCTET STRING, -- not used
     *			krbv42dsa	[2] OCTET STRING, -- not used
     *			sasl		[3] SaslCredentials -- v3 only
     *		}
     *	}
     *
     *	Saslcredentials ::= SEQUENCE {
     *		mechanism	LDAPString,
     *		credentials	OCTET STRING
     *	}
     */

    rc = ber_scanf( ber, "{iat", &version, &dn, &method );
    if ( rc == LBER_ERROR ) {
        LDAPDebug( LDAP_DEBUG_ANY,
                   "ber_scanf failed (op=Bind; params=Version,DN,Method)\n",
                   0, 0, 0 );
        log_bind_access (pb, "???", method, version, saslmech, "decoding error");
        send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                          "decoding error", 0, NULL );
        slapi_ch_free_string(&dn);
        return;
    }

    slapi_sdn_init_dn_passin(&sdn,dn);

    LDAPDebug( LDAP_DEBUG_TRACE, "BIND dn=\"%s\" method=%d version=%d\n",
               dn, method, version );

    /* target spec is used to decide which plugins are applicable for the operation */
    operation_set_target_spec (pb->pb_op, &sdn);

    switch ( method ) {
    case LDAP_AUTH_SASL:
        if ( version < LDAP_VERSION3 ) {
            LDAPDebug( LDAP_DEBUG_ANY,
                       "got SASL credentials from LDAPv2 client\n",
                       0, 0, 0 );
            log_bind_access (pb, slapi_sdn_get_dn (&sdn), method, version, saslmech, "SASL credentials only in LDAPv3");
            send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                              "SASL credentials only in LDAPv3", 0, NULL );
            goto free_and_return;
        }
        /* Get the SASL mechanism */
        rc = ber_scanf( ber, "{a", &saslmech );
        /* Get the (optional) SASL credentials */
        if ( rc != LBER_ERROR ) {
            /* Credentials are optional in SASL bind */
            ber_len_t clen;
            if (( ber_peek_tag( ber, &clen )) == LBER_OCTETSTRING ) {
                rc = ber_scanf( ber, "o}}", &cred );
                if (cred.bv_len == 0) {
                    slapi_ch_free_string(&cred.bv_val);
                }
            } else {
                rc = ber_scanf( ber, "}}" );
            }
        }
        break;
    case LDAP_AUTH_KRBV41:
        /* FALLTHROUGH */
    case LDAP_AUTH_KRBV42:
        if ( version >= LDAP_VERSION3 ) {
            static char *kmsg = 
                "LDAPv2-style kerberos authentication received "
                "on LDAPv3 connection.";
            LDAPDebug( LDAP_DEBUG_ANY, kmsg, 0, 0, 0 );
            log_bind_access (pb, slapi_sdn_get_dn (&sdn), method, version, saslmech, kmsg);
            send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                              kmsg, 0, NULL );
            goto free_and_return;
        }
        /* FALLTHROUGH */
    case LDAP_AUTH_SIMPLE:
        rc = ber_scanf( ber, "o}", &cred );
        if (cred.bv_len == 0) {
            slapi_ch_free_string(&cred.bv_val);
        }
        break;
    default:
        log_bind_access (pb, slapi_sdn_get_dn (&sdn), method, version, saslmech, "Unknown bind method");
        send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                          "Unknown bind method", 0, NULL );
        goto free_and_return;
    }
    if ( rc == LBER_ERROR ) {
        LDAPDebug( LDAP_DEBUG_ANY,
                   "ber_scanf failed (op=Bind; params=Credentials)\n",
                   0, 0, 0 );
        log_bind_access (pb, slapi_sdn_get_dn (&sdn), method, version, saslmech, "decoding error");
        send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                          "decoding error", 0, NULL );
        goto free_and_return;
    }

    /*
     * in LDAPv3 there can be optional control extensions on
     * the end of an LDAPMessage. we need to read them in and
     * pass them to the backend.
     * We also check for the presence of an "Authentication Request
     * Control" and set a flag so we know later whether we need to send
     * an "Authentication Response Control" with Success responses.
     */
    {
        LDAPControl	**reqctrls;

        if (( err = get_ldapmessage_controls( pb, ber, &reqctrls ))
            != 0 ) {
            log_bind_access (pb, slapi_sdn_get_dn (&sdn), method,
                             version, saslmech, "failed to parse LDAP controls");
            send_ldap_result( pb, err, NULL, NULL, 0, NULL );
            goto free_and_return;
        }

        auth_response_requested =  slapi_control_present( reqctrls,
                                                          LDAP_CONTROL_AUTH_REQUEST, NULL, NULL );
        slapi_pblock_get (pb, SLAPI_PWPOLICY, &pw_response_requested);
    }

    PR_Lock( pb->pb_conn->c_mutex );

    bind_credentials_clear( pb->pb_conn, PR_FALSE, /* do not lock conn */
                            PR_FALSE /* do not clear external creds. */ );

#if defined(ENABLE_AUTOBIND)
    /* LDAPI might have auto bind on, binding as anon should
       mean bind as self in this case
     */
    /* You are "bound" when the SSL connection is made, 
       but the client still passes a BIND SASL/EXTERNAL request.
     */
    if((LDAP_AUTH_SASL == method) &&
       (0 == strcasecmp (saslmech, LDAP_SASL_EXTERNAL)) &&
       (0 == dn || 0 == dn[0]) && pb->pb_conn->c_unix_local)
    {
        slapd_bind_local_user(pb->pb_conn);
        if(pb->pb_conn->c_dn)
        {
            auto_bind = 1; /* flag the bind method */
            dn = slapi_ch_strdup(pb->pb_conn->c_dn);
            slapi_sdn_init_dn_passin(&sdn,dn);
        }
    }
#endif /* ENABLE_AUTOBIND */

    /* Clear the password policy flag that forbid operation
     * other than Bind, Modify, Unbind :
     * With a new bind, the flag should be reset so that the new
     * bound user can work properly
     */
    pb->pb_conn->c_needpw = 0;
    PR_Unlock( pb->pb_conn->c_mutex );

    log_bind_access(pb, dn, method, version, saslmech, NULL);

    switch ( version ) {
    case LDAP_VERSION2:
        if (method == LDAP_AUTH_SIMPLE
            && (dn == NULL || *dn == '\0') && cred.bv_len == 0
            && pb->pb_conn->c_external_dn != NULL) {
            /* Treat this like a SASL EXTERNAL Bind: */
            method = LDAP_AUTH_SASL;
            saslmech = slapi_ch_strdup (LDAP_SASL_EXTERNAL);
            /* This enables a client to establish an identity by sending
             * a certificate in the SSL handshake, and also use LDAPv2
             * (by sending this type of Bind request).
             */
        }
        break;
    case LDAP_VERSION3:
        break;
    default:
        LDAPDebug( LDAP_DEBUG_TRACE, "bind: unknown LDAP protocol version %d\n",
                   version, 0, 0 );
        send_ldap_result( pb, LDAP_PROTOCOL_ERROR, NULL,
                          "version not supported", 0, NULL );
        goto free_and_return;
    }

    LDAPDebug( LDAP_DEBUG_TRACE, "do_bind: version %d method 0x%x dn %s\n",
               version, method, dn );
    pb->pb_conn->c_ldapversion = version;

    isroot = slapi_dn_isroot( slapi_sdn_get_ndn(&sdn) );
    slapi_pblock_set( pb, SLAPI_REQUESTOR_ISROOT, &isroot );
    slapi_pblock_set( pb, SLAPI_BIND_TARGET, (void*)slapi_sdn_get_ndn(&sdn) );
    slapi_pblock_set( pb, SLAPI_BIND_METHOD, &method );
    slapi_pblock_set( pb, SLAPI_BIND_SASLMECHANISM, saslmech );
    slapi_pblock_set( pb, SLAPI_BIND_CREDENTIALS, &cred );

    if (method != LDAP_AUTH_SASL) {
        /*
         * RFC2251: client may abort a sasl bind negotiation by sending
         * an authentication choice other than sasl.
         */
        pb->pb_conn->c_flags &= ~CONN_FLAG_SASL_CONTINUE;
    }

    switch ( method ) {
    case LDAP_AUTH_SASL:
        /*
         * All SASL auth methods are categorized as strong binds,
         * although they are not necessarily stronger than simple.
         */
        slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsStrongAuthBinds);
        if ( saslmech == NULL || *saslmech == '\0' ) {
            send_ldap_result( pb, LDAP_AUTH_METHOD_NOT_SUPPORTED, NULL,
                              "SASL mechanism absent", 0, NULL );
            goto free_and_return;
        }

        if (strlen(saslmech) > SASL_MECHNAMEMAX) {
            send_ldap_result( pb, LDAP_AUTH_METHOD_NOT_SUPPORTED, NULL,
                              "SASL mechanism name is too long", 0, NULL );
            goto free_and_return;
        }

        supported = slapi_get_supported_saslmechanisms_copy();
        if ( (pmech = supported) != NULL ) while (1) {
            if (*pmech == NULL) {
		/* As we call the safe function, we receive a strdup'd saslmechanisms
		   charray. Therefore, we need to remove it instead of NULLing it */
		charray_free(supported); 
		pmech = supported = NULL;
                break;
            }
            if (!strcasecmp (saslmech, *pmech)) break;
            ++pmech;
        }
        if (!pmech) {
            /* now check the sasl library */
            /* ids_sasl_check_bind takes care of calling bind
             * pre-op plugins after it knows the target DN */
            ids_sasl_check_bind(pb);
            plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
            goto free_and_return;
        }
        else {
            charray_free(supported); /* Avoid leaking */
        }

        if (!strcasecmp (saslmech, LDAP_SASL_EXTERNAL)) {
            /* call preop plugins */
            if (plugin_call_plugins( pb, SLAPI_PLUGIN_PRE_BIND_FN ) != 0){
                goto free_and_return;
            }

#if defined(ENABLE_AUTOBIND)
            if (1 == auto_bind) {
                /* Already AUTO-BOUND */
                break;
            }
#endif
            /*
             * if this is not an SSL connection, fail and return an
             * inappropriateAuth error.
             */
            if ( 0 == ( pb->pb_conn->c_flags & CONN_FLAG_SSL )) {
                send_ldap_result( pb, LDAP_INAPPROPRIATE_AUTH, NULL,
                                  "SASL EXTERNAL bind requires an SSL connection",
                                  0, NULL );
                /* call postop plugins */
                plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
                goto free_and_return;
            }

            /*
             * if the client sent us a certificate but we could not map it
             * to an LDAP DN, fail and return an invalidCredentials error.
             */
            if ( NULL != pb->pb_conn->c_client_cert &&
                 NULL == pb->pb_conn->c_external_dn ) {
                send_ldap_result( pb, LDAP_INVALID_CREDENTIALS, NULL,
                                  "client certificate mapping failed", 0, NULL );
                /* call postop plugins */
                plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
                goto free_and_return;
            }

            if (!isroot ) {
            /* check if the account is locked */
                bind_target_entry = get_entry(pb, pb->pb_conn->c_external_dn);
                if ( bind_target_entry != NULL && check_account_lock(pb, bind_target_entry,
                     pw_response_requested, 0 /*not account_inactivation_only*/ ) == 1) {
                    /* call postop plugins */
                    plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
                    goto free_and_return;
                }
            }

            /*
             * copy external credentials into connection structure
             */
            bind_credentials_set( pb->pb_conn,
                                  pb->pb_conn->c_external_authtype, 
                                  pb->pb_conn->c_external_dn,
                                  NULL, NULL, NULL , NULL);
            if ( auth_response_requested ) {
                slapi_add_auth_response_control( pb, pb->pb_conn->c_external_dn );
            }
            send_ldap_result( pb, LDAP_SUCCESS, NULL, NULL, 0, NULL );
            /* call postop plugins */
            plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
            goto free_and_return;
        }
        break;
    case LDAP_AUTH_SIMPLE:
        slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsSimpleAuthBinds);

        /* Check if the minimum SSF requirement has been met. */
        minssf = config_get_minssf();
        if ((pb->pb_conn->c_sasl_ssf < minssf) && (pb->pb_conn->c_ssl_ssf < minssf)) {
            send_ldap_result(pb, LDAP_UNWILLING_TO_PERFORM, NULL,
                             "Minimum SSF not met.", 0, NULL);
            /* increment BindSecurityErrorcount */
            slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
            goto free_and_return;
        }

        /* accept null binds */
        if (dn == NULL || *dn == '\0') {
            slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsAnonymousBinds);
            /* by definition anonymous is also unauthenticated so increment 
               that counter */
            slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsUnAuthBinds);

            /* Refuse the operation if anonymous access is disabled. */
            if (!config_get_anon_access_switch()) {
                send_ldap_result(pb, LDAP_INAPPROPRIATE_AUTH, NULL,
                                 "Anonymous access is not allowed", 0, NULL);
                /* increment BindSecurityErrorcount */
                slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                goto free_and_return;
            }

            /* call preop plugins */
            if (plugin_call_plugins( pb, SLAPI_PLUGIN_PRE_BIND_FN ) == 0){
                if ( auth_response_requested ) {
                    slapi_add_auth_response_control( pb, "" );
                }
                send_ldap_result( pb, LDAP_SUCCESS, NULL, NULL, 0, NULL );

                /* call postop plugins */
                plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
            }
            goto free_and_return;
        /* Check if unauthenticated binds are allowed. */
        } else if ( cred.bv_len == 0 ) {
            /* Increment unauthenticated bind counter */
            slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsUnAuthBinds);

            /* Refuse the operation if anonymous access is disabled. */
            if (!config_get_anon_access_switch()) {
                send_ldap_result(pb, LDAP_INAPPROPRIATE_AUTH, NULL,
                                 "Anonymous access is not allowed", 0, NULL);
                /* increment BindSecurityErrorcount */
                slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                goto free_and_return;
            }

            /* Refuse the operation if unauthenticated binds are disabled. */
            if (!config_get_unauth_binds_switch()) {
                /* As stated in RFC 4513, a server SHOULD by default fail
                 * Unauthenticated Bind requests with a resultCode of
                 * unwillingToPerform. */
                send_ldap_result(pb, LDAP_UNWILLING_TO_PERFORM, NULL,
                                 "Unauthenticated binds are not allowed", 0, NULL);
                /* increment BindSecurityErrorcount */
                slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                goto free_and_return;
            }
        /* Check if simple binds are allowed over an insecure channel.  We only check
	 * this for authenticated binds. */
        } else if (config_get_require_secure_binds() == 1) {
                Connection *conn = NULL;
                int sasl_ssf = 0;

                /* Allow simple binds only for SSL/TLS established connections
                 * or connections using SASL privacy layers */
                conn = pb->pb_conn;
                if ( slapi_pblock_get(pb, SLAPI_CONN_SASL_SSF, &sasl_ssf) != 0) {
                    slapi_log_error( SLAPI_LOG_PLUGIN, "do_bind",
                                     "Could not get SASL SSF from connection\n" );
                    sasl_ssf = 0;
                }

                if (((conn->c_flags & CONN_FLAG_SSL) != CONN_FLAG_SSL) &&
                    (sasl_ssf <= 1) ) {
                        send_ldap_result(pb, LDAP_CONFIDENTIALITY_REQUIRED, NULL,
                                         "Operation requires a secure connection",
                                         0, NULL);
                        slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                        goto free_and_return;
                }
        }
        break;
    default:
        break;
    }

    /*
     * handle binds as the manager here, pass others to the backend
     */

    if ( isroot && method == LDAP_AUTH_SIMPLE ) {
        if (cred.bv_len != 0) {
            /* a passwd was supplied -- check it */
            Slapi_Value cv;
            slapi_value_init_berval(&cv,&cred);

            /* right dn and passwd - authorize */
            if ( is_root_dn_pw( slapi_sdn_get_ndn(&sdn), &cv )) {
                bind_credentials_set( pb->pb_conn, SLAPD_AUTH_SIMPLE,
                                      slapi_ch_strdup( slapi_sdn_get_ndn(&sdn) ),
                                      NULL, NULL, NULL , NULL);

            /* right dn, wrong passwd - reject with invalid creds */
            } else {
                send_ldap_result( pb, LDAP_INVALID_CREDENTIALS, NULL,
                                  NULL, 0, NULL );
                /* increment BindSecurityErrorcount */
                slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                value_done(&cv);
                goto free_and_return;
            }
            value_done(&cv);
        }

        /* call preop plugin */
        if (plugin_call_plugins( pb, SLAPI_PLUGIN_PRE_BIND_FN ) == 0){
            if ( auth_response_requested ) {
                slapi_add_auth_response_control( pb,
                                           ( cred.bv_len == 0 ) ? "" :
                                           slapi_sdn_get_ndn(&sdn));
            }
            send_ldap_result( pb, LDAP_SUCCESS, NULL, NULL, 0, NULL );

            /* call postop plugins */
            plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
        }
        goto free_and_return;
    }

    /* We could be serving multiple database backends.  Select the appropriate one */
    if (slapi_mapping_tree_select(pb, &be, &referral, errorbuf) != LDAP_SUCCESS) {
        send_nobackend_ldap_result( pb );
        be = NULL;
        goto free_and_return;
    }

    if (referral)
    {
		send_referrals_from_entry(pb,referral);
		slapi_entry_free(referral);
        goto free_and_return;
    }

    slapi_pblock_set( pb, SLAPI_BACKEND, be );

	/* not root dn - pass to the backend */
    if ( be->be_bind != NULL ) {

        /*
         * call the pre-bind plugins. if they succeed, call
         * the backend bind function. then call the post-bind
         * plugins.
         */
        if ( plugin_call_plugins( pb, SLAPI_PLUGIN_PRE_BIND_FN )
             == 0 )  {
            int	rc = 0;

            /*
             * Is this account locked ?
             *	could be locked through the account inactivation
             *	or by the password policy
             *
             * rc=0: account not locked
             * rc=1: account locked, can not bind, result has been sent
             * rc!=0 and rc!=1: error. Result was not sent, lets be_bind
             * 		deal with it.
             *
             */
			
			/* get the entry now, so that we can give it to check_account_lock and reslimit_update_from_dn */
            if (! slapi_be_is_flag_set(be, SLAPI_BE_FLAG_REMOTE_DATA)) {
				bind_target_entry = get_entry(pb,  slapi_sdn_get_ndn(&sdn));
				rc = check_account_lock ( pb, bind_target_entry, pw_response_requested,0);
            }

            slapi_pblock_set( pb, SLAPI_PLUGIN, be->be_database );
            set_db_default_result_handlers(pb);
            if ( (rc != 1) && (auto_bind || (((rc = (*be->be_bind)( pb ))
                                == SLAPI_BIND_SUCCESS ) || rc
                               == SLAPI_BIND_ANONYMOUS ))) {
                long t;
                {
                    char* authtype = NULL;

                    if(auto_bind)
                        rc = SLAPI_BIND_SUCCESS;

                    switch ( method ) {
                    case LDAP_AUTH_SIMPLE:
                        if (cred.bv_len != 0) {
                            authtype = SLAPD_AUTH_SIMPLE;
                        }
#if defined(ENABLE_AUTOBIND)
                        else if(auto_bind) {
                            authtype = SLAPD_AUTH_OS;
                        }
#endif /* ENABLE_AUTOBIND */
                        break;
                    case LDAP_AUTH_SASL:
                        /* authtype = SLAPD_AUTH_SASL && saslmech: */
                        PR_snprintf(authtypebuf, sizeof(authtypebuf), "%s%s", SLAPD_AUTH_SASL, saslmech);
                        authtype = authtypebuf;
                    break;
                    default: /* ??? */
                        break;
                    }

                    if ( rc == SLAPI_BIND_SUCCESS ) {
			if(!auto_bind)
                            bind_credentials_set( pb->pb_conn,
                                              authtype, slapi_ch_strdup(
                                                  slapi_sdn_get_ndn(&sdn)),
                                              NULL, NULL, NULL, bind_target_entry );
                        if ( auth_response_requested ) {
                            slapi_add_auth_response_control( pb,
                                                       slapi_sdn_get_ndn(&sdn));
                        }
                    } else {	/* anonymous */
                        if ( auth_response_requested ) {
                            slapi_add_auth_response_control( pb,
                                                       "" );
                        }
                    }
                }

                if ( 0 == auto_bind && rc != SLAPI_BIND_ANONYMOUS &&
                     ! slapi_be_is_flag_set(be,
                                            SLAPI_BE_FLAG_REMOTE_DATA)) {
                    /* check if need new password before sending 
                       the bind success result */
                    switch ( need_new_pw (pb, &t, bind_target_entry, pw_response_requested )) {
						
                    case 1:
                        (void)slapi_add_pwd_control ( pb, 
                                                LDAP_CONTROL_PWEXPIRED, 0);
                        break;
						
                    case 2:
                        (void)slapi_add_pwd_control ( pb, 
                                                LDAP_CONTROL_PWEXPIRING, t);
                        break;
                    case -1: 
                        goto free_and_return;
                    default:
                        break;
                    } 
                } /* end if */
            }else{
                
                if(cred.bv_len == 0) {
                    /* its an UnAuthenticated Bind, DN specified but no pw */
                    slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsUnAuthBinds);
                }else{
                    /* password must have been invalid */
                    /* increment BindSecurityError count */
                    slapi_counter_increment(g_get_global_snmp_vars()->ops_tbl.dsBindSecurityErrors);
                }
			
            }

            /*
             * if rc != SLAPI_BIND_SUCCESS and != SLAPI_BIND_ANONYMOUS,
             * the result has already been sent by the backend.  otherwise,
             * we assume it is success and send it here to avoid a race
             * condition where the client could be told by the
             * backend that the bind succeeded before we set the
             * c_dn field in the connection structure here in
             * the front end.
             */
            if ( rc == SLAPI_BIND_SUCCESS || rc == SLAPI_BIND_ANONYMOUS) {
                send_ldap_result( pb, LDAP_SUCCESS, NULL, NULL,
                                  0, NULL );
            }

            slapi_pblock_set( pb, SLAPI_PLUGIN_OPRETURN, &rc );
            plugin_call_plugins( pb, SLAPI_PLUGIN_POST_BIND_FN );
        }
    } else {
        send_ldap_result( pb, LDAP_UNWILLING_TO_PERFORM, NULL,
                          "Function not implemented", 0, NULL );
    }

 free_and_return:;
    if (be)
        slapi_be_Unlock(be);
    slapi_sdn_done(&sdn);
    slapi_ch_free_string( &saslmech );
    slapi_ch_free( (void **)&cred.bv_val );
	if ( bind_target_entry != NULL )
		slapi_entry_free(bind_target_entry);
}


/*
 * register all of the LDAPv3 SASL mechanisms we know about.
 */
void
init_saslmechanisms( void )
{
    ids_sasl_init();
    slapi_register_supported_saslmechanism( LDAP_SASL_EXTERNAL );
}

static void
log_bind_access (
    Slapi_PBlock *pb, 
    const char* dn, 
    int method, 
    int version,
    const char *saslmech,
    const char *msg
)
{
    char ebuf[ BUFSIZ ];
    const char *edn;

    edn = escape_string( dn, ebuf );

    if (method == LDAP_AUTH_SASL && saslmech && msg) {
        slapi_log_access( LDAP_DEBUG_STATS, 
                          "conn=%" NSPRIu64 " op=%d BIND dn=\"%s\" "
                          "method=sasl version=%d mech=%s, %s\n",
                          pb->pb_conn->c_connid, pb->pb_op->o_opid, edn, 
                          version, saslmech, msg );
    } else if (method == LDAP_AUTH_SASL && saslmech) {
        slapi_log_access( LDAP_DEBUG_STATS, 
                          "conn=%" NSPRIu64 " op=%d BIND dn=\"%s\" "
                          "method=sasl version=%d mech=%s\n",
                          pb->pb_conn->c_connid, pb->pb_op->o_opid, edn, 
                          version, saslmech );
    } else if (msg) {
        slapi_log_access( LDAP_DEBUG_STATS, 
                          "conn=%" NSPRIu64 " op=%d BIND dn=\"%s\" "
                          "method=%d version=%d, %s\n",
                          pb->pb_conn->c_connid, pb->pb_op->o_opid, edn, 
                          method, version, msg );
    } else {
        slapi_log_access( LDAP_DEBUG_STATS, 
                          "conn=%" NSPRIu64 " op=%d BIND dn=\"%s\" "
                          "method=%d version=%d\n",
                          pb->pb_conn->c_connid, pb->pb_op->o_opid, edn, 
                          method, version );
    }
}


void
slapi_add_auth_response_control( Slapi_PBlock *pb, const char *binddn )
{
    LDAPControl		arctrl;
	char			dnbuf_fixedsize[ 512 ], *dnbuf, *dnbuf_dynamic = NULL;
	size_t			dnlen;

	if ( NULL == binddn ) {
		binddn = "";
	}
	dnlen = strlen( binddn );

	/*
	 * According to draft-weltman-ldapv3-auth-response-03.txt section
	 * 4 (Authentication Response Control):
	 *
	 *   The controlType is "2.16.840.1.113730.3.4.15". If the bind request   
	 *   succeeded and resulted in an identity (not anonymous), the 
	 *   controlValue contains the authorization identity [AUTH] granted to 
	 *   the requestor. If the bind request resulted in anonymous 
	 *   authentication, the controlValue field is a string of zero length. 
	 *
	 * [AUTH] is a reference to RFC 2829, which in section 9 defines
	 * authorization identity as:
	 *
	 *
	 *   The authorization identity is a string in the UTF-8 character set,
	 *   corresponding to the following ABNF [7]:
	 *
	 *   ; Specific predefined authorization (authz) id schemes are
	 *   ; defined below -- new schemes may be defined in the future.
	 *
	 *   authzId    = dnAuthzId / uAuthzId
	 *
	 *   ; distinguished-name-based authz id.
	 *   dnAuthzId  = "dn:" dn
	 *   dn         = utf8string    ; with syntax defined in RFC 2253
	 *
	 *   ; unspecified userid, UTF-8 encoded.
	 *   uAuthzId   = "u:" userid
	 *   userid     = utf8string    ; syntax unspecified
	 *
	 *   A utf8string is defined to be the UTF-8 encoding of one or more ISO
	 *   10646 characters.
	 *
	 * We always map identities to DNs, so we always use the dnAuthzId form.
	 */
	arctrl.ldctl_oid = LDAP_CONTROL_AUTH_RESPONSE;
	arctrl.ldctl_iscritical = 0;

	if ( dnlen == 0 ) {		/* anonymous -- return zero length value */
		arctrl.ldctl_value.bv_val = "";
		arctrl.ldctl_value.bv_len = 0;
	} else {				/* mapped to a DN -- return "dn:<DN>" */
		if ( 3 + dnlen < sizeof( dnbuf_fixedsize )) {
			dnbuf = dnbuf_fixedsize;
		} else {
			dnbuf = dnbuf_dynamic = slapi_ch_malloc( 4 + dnlen );
		}
		strcpy( dnbuf, "dn:" );
		strcpy( dnbuf + 3, binddn );
		arctrl.ldctl_value.bv_val = dnbuf;
		arctrl.ldctl_value.bv_len = 3 + dnlen;
	}
	
	if ( slapi_pblock_set( pb, SLAPI_ADD_RESCONTROL, &arctrl ) != 0 ) {
		slapi_log_error( SLAPI_LOG_FATAL, "bind",
				"unable to add authentication response control" );
	}

	if ( NULL != dnbuf_dynamic ) {
		slapi_ch_free_string( &dnbuf_dynamic );
	}
}
