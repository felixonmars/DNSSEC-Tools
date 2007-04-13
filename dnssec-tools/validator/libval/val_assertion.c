/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */
#include "validator-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#ifndef VAL_NO_THREADS
#include <pthread.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <arpa/nameser.h>
#ifdef HAVE_ARPA_NAMESER_COMPAT_H
#include <arpa/nameser_compat.h>
#else
#include "arpa/header.h"
#endif

#include <validator/resolver.h>
#include <validator/validator.h>
#include <resolv.h>
#include "val_resquery.h"
#include "val_support.h"
#include "val_cache.h"
#include "val_verify.h"
#include "val_policy.h"
#include "val_crypto.h"
#include "val_context.h"
#include "val_assertion.h"

#define STRIP_LABEL(name, newname) do {\
    int label_len;\
    label_len = name[0];\
    if (label_len != 0) {\
        newname = name + label_len + 1;\
    }\
} while(0)

#define ADD_LABEL(q_zonecut_n, curzone_n, qname) do {\
    u_int8_t *endptr = (qname == NULL)? curzone_n : qname;\
    u_int8_t *stptr = q_zonecut_n;\
    u_int8_t *prevptr = NULL;\
    while (!namecmp(stptr, endptr) && (prevptr != stptr)) {\
        prevptr = stptr;\
        STRIP_LABEL(prevptr, stptr);\
    }\
    if (prevptr)\
        qname = prevptr;\
    else\
        qname = stptr;\
} while (0)

/*
 * Identify if the type is present in the bitmap
 * The encoding of the bitmap is a sequence of <block#, len, bitmap> tuples
 */
static int
is_type_set(u_int8_t * field, int field_len, u_int16_t type)
{
    int             block, blen;

    /** The type will be present in the following block */
    int             t_block = type / 256;
    /** within the bitmap, the type will be present in the following byte */
    int             t_bm_offset = type / 8;

    int             cnt = 0;

    /*
     * need at least two bytes 
     */
    while (field_len > cnt + 2) {

        block = field[cnt];
        blen = field[cnt + 1];
        cnt += 2;

        if (block == t_block) {
            /*
             * see if we have space 
             */
            if ((blen >= t_bm_offset) && (field_len >= cnt + blen)) {
                /*
                 * see if the bit is set 
                 */
                if (field[cnt + t_bm_offset] & (1 << (7 - (type % 8))))
                    return 1;
            }
            return 0;
        }
        cnt += blen;
    }
    return 0;
}

#define CLONE_NAME_LEN(oldb, oldlen, newb, newlen) do {\
        if (oldlen) {                                                   \
            newb =	(u_int8_t *) MALLOC (oldlen * sizeof(u_int8_t)); \
            if (newb == NULL) {                                         \
                retval = VAL_OUT_OF_MEMORY;                             \
                goto err;                                               \
            }                                                           \
            memcpy(newb, oldb, oldlen);                                 \
        }                                                               \
        else                                                            \
            newb = NULL;                                                \
        newlen = oldlen;                                                \
    } while (0)


#ifdef LIBVAL_NSEC3
#define CHECK_RANGE(range1, range1len, range2, range2len, hash, hashlen) \
            ((nsec3_order_cmp(range2, range2len, hash, hashlen) != 0) &&\
                ((nsec3_order_cmp(range2, range2len, range1, range1len) > 0)?\
                    ((nsec3_order_cmp(hash, hashlen, range1, range1len) > 0) && \
					(nsec3_order_cmp(hash, hashlen, range2, range2len) < 0)) :\
                    ((nsec3_order_cmp(hash, hashlen, range2, range2len) < 0)||\
                     (nsec3_order_cmp(hash, hashlen, range1, range1len) > 0))))
#endif

    
void
free_val_rrset_members(struct val_rrset *r)
{
    if (r == NULL)
        return;

    if (r->val_msg_header)
        FREE(r->val_msg_header);
    if (r->val_rrset_name_n)
        FREE(r->val_rrset_name_n);
    if (r->val_rrset_server)
        FREE(r->val_rrset_server);
    if (r->val_rrset_data != NULL)
        res_sq_free_rr_recs(&r->val_rrset_data);
    if (r->val_rrset_sig != NULL)
        res_sq_free_rr_recs(&r->val_rrset_sig);
            
}

/*
 * Create a "result" list whose elements point to assertions and also have their
 * validated result 
 */

void
val_free_result_chain(struct val_result_chain *results)
{
    struct val_result_chain *prev;
    struct val_authentication_chain *trust;
    int             i;

    while (NULL != (prev = results)) {
        results = results->val_rc_next;

        /*
         * free the chain of trust 
         */
        while (NULL != (trust = prev->val_rc_answer)) {

            prev->val_rc_answer = trust->val_ac_trust;

            if (trust->val_ac_rrset != NULL) {
                free_val_rrset_members(trust->val_ac_rrset);
                FREE(trust->val_ac_rrset);
            }

            FREE(trust);
        }

        for (i = 0; i < prev->val_rc_proof_count; i++) {

            if (prev->val_rc_proofs[i] == NULL)
                break;

            while (NULL != (trust = prev->val_rc_proofs[i])) {
                prev->val_rc_proofs[i] = trust->val_ac_trust;
                if (trust->val_ac_rrset != NULL) {
                    if (trust->val_ac_rrset->val_msg_header)
                        FREE(trust->val_ac_rrset->val_msg_header);
                    if (trust->val_ac_rrset->val_rrset_name_n)
                        FREE(trust->val_ac_rrset->val_rrset_name_n);
                    if (trust->val_ac_rrset->val_rrset_server)
                        FREE(trust->val_ac_rrset->val_rrset_server);
                    if (trust->val_ac_rrset->val_rrset_data != NULL)
                        res_sq_free_rr_recs(&trust->val_ac_rrset->
                                            val_rrset_data);
                    if (trust->val_ac_rrset->val_rrset_sig != NULL)
                        res_sq_free_rr_recs(&trust->val_ac_rrset->
                                            val_rrset_sig);

                    FREE(trust->val_ac_rrset);
                }
                FREE(trust);
            }
        }

        FREE(prev);
    }
}

void reset_query_chain_node(struct val_query_chain *q)
{
    if (q == NULL)
        return;

    q->qc_ttl_x = 0;
    q->qc_bad = 0;
    q->qc_zonecut_n = NULL;
    q->qc_ans = NULL;
    q->qc_proof = NULL;
    q->qc_ns_list = NULL;
    q->qc_respondent_server = NULL;
    q->qc_referral = NULL;
    q->qc_state = Q_INIT;
    q->qc_trans_id = -1;
}


/*
 * Add {domain_name, type, class} to the list of queries currently active
 * for validating a response. 
 *
 * Returns:
 * VAL_NO_ERROR                 Operation succeeded
 * VAL_BAD_ARGUMENT     Bad argument (e.g. NULL ptr)
 * VAL_OUT_OF_MEMORY    Could not allocate enough memory for operation
 */
int
add_to_query_chain(struct val_query_chain **queries, u_char * name_n,
                   const u_int16_t type_h, const u_int16_t class_h, 
                   const u_int8_t flags, struct val_query_chain **added_q)
{
    struct val_query_chain *temp, *prev;
    /* use only those flags that affect caching */
    const u_int8_t caching_flag = flags & VAL_MASK_AFFECTS_CACHING;

    /*
     * sanity checks 
     */
    if ((NULL == queries) || (NULL == name_n) || (added_q == NULL))
        return VAL_BAD_ARGUMENT;

    *added_q = NULL;

    /*
     * Check if query already exists 
     */
    temp = *queries;
    prev = temp;
    while (temp) {
        if ((namecmp(temp->qc_original_name, name_n) == 0)
            && (temp->qc_type_h == type_h)
            && (temp->qc_class_h == class_h)
            && (caching_flag == (temp->qc_flags & VAL_MASK_AFFECTS_CACHING)))
            break;
        prev = temp;
        temp = temp->qc_next;
    }
    if (temp != NULL) {
        *added_q = temp;
        return VAL_NO_ERROR;
    }

    temp =
        (struct val_query_chain *) MALLOC(sizeof(struct val_query_chain));
    if (temp == NULL)
        return VAL_OUT_OF_MEMORY;

#ifndef VAL_NO_THREADS
    if (0 != pthread_rwlock_init(&temp->qc_rwlock, NULL)) {
        FREE(temp);
        return VAL_INTERNAL_ERROR;
    } 
#endif

    memcpy(temp->qc_name_n, name_n, wire_name_length(name_n));
    memcpy(temp->qc_original_name, name_n, wire_name_length(name_n));
    temp->qc_type_h = type_h;
    temp->qc_class_h = class_h;
    temp->qc_flags = flags;

    reset_query_chain_node(temp);
    
    temp->qc_next = *queries;
    *queries = temp;
    *added_q = temp;

    return VAL_NO_ERROR;
}

static void 
free_query_chain_structure(struct val_query_chain *queries)
{
    if (queries->qc_zonecut_n != NULL) {
        FREE(queries->qc_zonecut_n);
        queries->qc_zonecut_n = NULL;
    }

    if (queries->qc_referral != NULL) {
        free_referral_members(queries->qc_referral);
        FREE(queries->qc_referral);
        queries->qc_referral = NULL;
    }

    if (queries->qc_ns_list != NULL) {
        free_name_servers(&(queries->qc_ns_list));
        queries->qc_ns_list = NULL;
    }

    if (queries->qc_respondent_server != NULL) {
        free_name_server(&(queries->qc_respondent_server));
        queries->qc_respondent_server = NULL;
    }
}


/*
 * Free up the query chain.
 */
void
free_query_chain(struct val_query_chain *queries)
{
    if (queries == NULL)
        return;

    if (queries->qc_next)
        free_query_chain(queries->qc_next);

    free_query_chain_structure(queries);
    FREE(queries);
}

void
free_authentication_chain_structure(struct val_digested_auth_chain *assertions)
{
    if (assertions && assertions->_as.ac_data)
        res_sq_free_rrset_recs(&(assertions->_as.ac_data));
}

void
delete_authentication_chain_element(val_context_t *ctx,
                                    struct val_digested_auth_chain *as)
{
    struct val_digested_auth_chain *t_as, *t_as_prev;
    
    if (ctx == NULL)
        return; 

    if(as == NULL)
        return; 

    if(as->_as.val_ac_rrset_next) 
        delete_authentication_chain_element(ctx, as->_as.val_ac_rrset_next); 
    
    t_as_prev = NULL;
    for(t_as=ctx->a_list; t_as; t_as=t_as->_as.val_ac_next) {
        if (t_as == as) {
            if (t_as_prev) {
                t_as_prev->_as.val_ac_next = t_as->_as.val_ac_next;
            } else {
                ctx->a_list = t_as->_as.val_ac_next;
            }
            t_as->_as.val_ac_next = NULL;
            free_authentication_chain_structure(t_as);
            FREE(t_as);
            return;
        } 
        t_as_prev = t_as;
    }
    return; 
}

void zap_query(val_context_t *context, struct val_query_chain *added_q) 
{
    if(context == NULL || added_q == NULL)
        return;
    
    delete_authentication_chain_element(context, added_q->qc_ans);
    delete_authentication_chain_element(context, added_q->qc_proof);
    free_query_chain_structure(added_q);
    reset_query_chain_node(added_q);
}

int
add_to_qfq_chain(val_context_t *context, struct queries_for_query **queries, 
                 u_char * name_n, const u_int16_t type_h, const u_int16_t class_h, 
                 const u_int8_t flags, struct queries_for_query **added_qfq)
{
    struct queries_for_query *temp, *prev;
    /* use only those flags that affect caching */
    const u_int8_t caching_flag = flags & VAL_MASK_AFFECTS_CACHING;
    struct val_query_chain *added_q = NULL;
    struct timeval  tv;
    int retval;
    
    /*
     * sanity checks 
     */
    if ((NULL == context) || (NULL == queries) || (NULL == name_n) || (added_qfq == NULL))
        return VAL_BAD_ARGUMENT;

    *added_qfq = NULL;

    /*
     * Check if query already exists 
     */
    temp = *queries;
    prev = temp;

    while (temp) {
        if ((namecmp(temp->qfq_query->qc_original_name, name_n) == 0)
            && (temp->qfq_query->qc_type_h == type_h)
            && (temp->qfq_query->qc_class_h == class_h)
            && (caching_flag == (temp->qfq_query->qc_flags & VAL_MASK_AFFECTS_CACHING)))
            break;
        prev = temp;
        temp = temp->qfq_next;
    }

    if (temp == NULL) {
        /*
         * Add to the cache and to the qfq chain 
         */
        if (VAL_NO_ERROR !=
            (retval =
             add_to_query_chain(&context->q_list, name_n, type_h, class_h,
                                flags, &added_q)))
            return retval;

        temp = (struct queries_for_query *) MALLOC (sizeof(struct queries_for_query));
        if (temp == NULL) {
            return VAL_OUT_OF_MEMORY;
        }
        temp->qfq_query = added_q;
        temp->qfq_next = *queries;
        gettimeofday(&tv, NULL);
        if (added_q->qc_bad > 0 || 
            (added_q->qc_ttl_x > 0 && 
            tv.tv_sec > added_q->qc_ttl_x)) {
            /* try to get an exclusive lock on this query */
            if(LOCK_QC_TRY_EX(added_q)) {
                if (added_q->qc_bad > 0 && 
                        !(flags & VAL_FLAGS_DONT_VALIDATE)) {
                    /* Invoke bad-cache logic only if validation is requested */
                    if (++added_q->qc_bad > QUERY_BAD_CACHE_THRESHOLD) {
                        added_q->qc_bad = QUERY_BAD_CACHE_THRESHOLD;
                        SET_MIN_TTL(added_q->qc_ttl_x, tv.tv_sec + QUERY_BAD_CACHE_TTL);
                    } else {
                        added_q->qc_ttl_x = 0; 
                    }
                }
               
                if (tv.tv_sec > added_q->qc_ttl_x) { 
                    /* flush data for this query and start again */
                    char name_p[NS_MAXDNAME];
                    if (-1 == ns_name_ntop(added_q->qc_name_n, name_p, sizeof(name_p)))
                        snprintf(name_p, sizeof(name_p), "unknown/error");
                    val_log(context, LOG_DEBUG, "Data in cache timed out: {%s %d %d}", 
                                name_p, added_q->qc_class_h, added_q->qc_type_h);
                    zap_query(context, added_q);
                    
                }

                UNLOCK_QC(added_q);
            }
        }
                
        LOCK_QC_SH(added_q);
        *queries = temp;
    } 
    
    *added_qfq = temp;
       
    return VAL_NO_ERROR; 
}

int free_qfq_chain(struct queries_for_query *queries)
{
    if (queries == NULL)
        return VAL_NO_ERROR; 

    if (queries->qfq_next)
        free_qfq_chain(queries->qfq_next);

    UNLOCK_QC(queries->qfq_query);
    FREE(queries);
    /* 
     * The val_query_chain that this qfq element points to 
     * is part of the context cache and will be freed when the
     * context is free'd or the TTL times out
     */
    return VAL_NO_ERROR;
}



int
get_zse(val_context_t * ctx, u_int8_t * name_n, u_int16_t *status, u_int32_t *ttl_x)
{
    policy_entry_t *zse_pol, *zse_cur;
    int             name_len;
    u_int8_t       *p;
    char            name_p[NS_MAXDNAME];

    /*
     * sanity checks 
     */
    if (NULL == name_n)
        return VAL_BAD_ARGUMENT;

    name_len = wire_name_length(name_n);

    /*
     * Check if the zone is trusted 
     */
    
    RETRIEVE_POLICY(ctx, P_ZONE_SECURITY_EXPECTATION, zse_pol);
    if (zse_pol != NULL) {
        for (zse_cur = zse_pol;
             zse_cur && (wire_name_length(zse_cur->zone_n) > name_len);
             zse_cur = zse_cur->next);

        /*
         * for all zones which are shorter or as long, do a strstr 
         */
        /*
         * Because of the ordering, the longest match is found first 
         */
        for (; zse_cur; zse_cur = zse_cur->next) {
            int             root_zone = 0;
            if (!namecmp(zse_cur->zone_n, (const u_int8_t *) ""))
                root_zone = 1;
            else {
                /*
                 * Find the last occurrence of zse_cur->zone_n in name_n 
                 */
                p = name_n;
                while (p && (*p != '\0')) {
                    if (!namecmp(p, zse_cur->zone_n))
                        break;
                    p = p + *p + 1;
                }
            }

            if ((root_zone || (!namecmp(p, zse_cur->zone_n))) && zse_cur->pol) {
                struct zone_se_policy *pol = 
                    (struct zone_se_policy *)(zse_cur->pol);
                    
                if (zse_cur->exp_ttl > 0)
                    *ttl_x = zse_cur->exp_ttl;
                
                if (-1 == ns_name_ntop(name_n, name_p, sizeof(name_p)))
                    snprintf(name_p, sizeof(name_p), "unknown/error");
                if (pol->trusted == ZONE_SE_UNTRUSTED) {
                    val_log(ctx, LOG_DEBUG, "zone %s is not trusted",
                            name_p);
                    *status = VAL_AC_UNTRUSTED_ZONE;
                    return VAL_NO_ERROR;
                } else if (pol->trusted == ZONE_SE_TRUSTED) {
                    val_log(ctx, LOG_DEBUG, "zone %s is trusted", name_p);
                    *status = VAL_AC_TRUSTED_ZONE;
                    return VAL_NO_ERROR;
                } else if (pol->trusted == ZONE_SE_DO_VAL) {
                    val_log(ctx, LOG_DEBUG, "%s requires DNSSEC", name_p);
                    *status = VAL_AC_WAIT_FOR_TRUST;
                    return VAL_NO_ERROR;
                } else {
                    /** ZONE_SE_IGNORE */
                    val_log(ctx, LOG_DEBUG, "Ignoring DNSSEC for  %s",
                            name_p);
                    *status = VAL_AC_IGNORE_VALIDATION;
                    return VAL_NO_ERROR;
                }
            }
        }
    }
    if (-1 == ns_name_ntop(name_n, name_p, sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
    val_log(ctx, LOG_DEBUG, "%s requires DNSSEC", name_p);

    *status = VAL_AC_WAIT_FOR_TRUST;
    return VAL_NO_ERROR;
}

int
find_trust_point(val_context_t * ctx, u_int8_t * zone_n,
                 u_int8_t ** matched_zone, u_int32_t *ttl_x)
{

    policy_entry_t *ta_pol, *ta_cur, *ta_tmphead;
    int             name_len;
    u_int8_t       *zp = zone_n;

    /*
     * This function should never be called with a NULL zone_n, but still... 
     */
    if ((zone_n == NULL) || (matched_zone == NULL))
        return VAL_BAD_ARGUMENT;

    *matched_zone = NULL;

    name_len = wire_name_length(zp);
    RETRIEVE_POLICY(ctx, P_TRUST_ANCHOR, ta_pol);
    if (ta_pol == NULL) {
        return VAL_NO_ERROR;
    }

    /*
     * skip longer names 
     */
    for (ta_cur = ta_pol;
         ta_cur && (wire_name_length(ta_cur->zone_n) > name_len);
         ta_cur = ta_cur->next);

    /*
     * for the remaining nodes, if the length of the zones are 
     * the same, look for an exact match 
     */
    for (; ta_cur &&
         (wire_name_length(ta_cur->zone_n) == name_len);
         ta_cur = ta_cur->next) {

        if (!namecmp(ta_cur->zone_n, zp)) {
            *matched_zone =
                (u_int8_t *) MALLOC(wire_name_length(zp) *
                                    sizeof(u_int8_t));
            if (*matched_zone == NULL) {
                return VAL_OUT_OF_MEMORY;
            }
            if (ta_cur->exp_ttl > 0)
                *ttl_x = ta_cur->exp_ttl;

            memcpy(*matched_zone, zp, wire_name_length(zp));
            return VAL_NO_ERROR;
        }
    }

    /*
     * for the remaining nodes, see if there is any hope 
     */
    ta_tmphead = ta_cur;
    while ((zp != NULL) && zp[0]) {
        /*
         * trim the top label from our candidate zone 
         */
        zp += (int) zp[0] + 1;
        for (ta_cur = ta_tmphead; ta_cur; ta_cur = ta_cur->next) {
            if (wire_name_length(zp) < wire_name_length(ta_cur->zone_n))
                /** next time look from this point */
                ta_tmphead = ta_cur->next;

            if (namecmp(ta_cur->zone_n, zp) == 0) {
                /** We have hope */
                *matched_zone =
                    (u_int8_t *) MALLOC(wire_name_length(zp) *
                                        sizeof(u_int8_t));
                if (*matched_zone == NULL) {
                    return VAL_OUT_OF_MEMORY;
                }
                memcpy(*matched_zone, zp, wire_name_length(zp));
                return VAL_NO_ERROR;
            }
        }
    }

    return VAL_NO_ERROR;
}

static int
is_trusted_key(val_context_t * ctx, u_int8_t * zone_n, struct rr_rec *key,
               val_astatus_t * status, u_int32_t *ttl_x)
{
    policy_entry_t *ta_pol, *ta_cur, *ta_tmphead;
    int             name_len;
    u_int8_t       *zp = zone_n;
    val_dnskey_rdata_t dnskey;
    struct rr_rec  *curkey;

    /*
     * This function should never be called with a NULL zone_n, but still... 
     */
    if ((zone_n == NULL) || (status == NULL))
        return VAL_BAD_ARGUMENT;

    /*
     * Default value, will change 
     */
    *status = VAL_AC_NO_TRUST_ANCHOR;

    name_len = wire_name_length(zp);
    RETRIEVE_POLICY(ctx, P_TRUST_ANCHOR, ta_pol);
    if (ta_pol == NULL) {
        *status = VAL_AC_NO_TRUST_ANCHOR;
        return VAL_NO_ERROR;
    }

    /*
     * skip longer names 
     */
    for (ta_cur = ta_pol;
         ta_cur && (wire_name_length(ta_cur->zone_n) > name_len);
         ta_cur = ta_cur->next);

    /*
     * for the remaining nodes, if the length of the zones are 
     * the same, look for an exact match 
     */
    for (; ta_cur &&
         (wire_name_length(ta_cur->zone_n) == name_len);
         ta_cur = ta_cur->next) {

        if (!namecmp(ta_cur->zone_n, zp)) {

            for (curkey = key; curkey; curkey = curkey->rr_next) {
                /** clear any data from previous iterations */
                memset(&dnskey, 0x00, sizeof(dnskey));

                /*
                 * parse key and compare
                 */
                val_parse_dnskey_rdata(curkey->rr_rdata,
                                       curkey->rr_rdata_length_h, &dnskey);

                if (ta_cur->pol) {
                    struct trust_anchor_policy *pol = 
                        (struct trust_anchor_policy *)(ta_cur->pol);

                    if (!dnskey_compare(&dnskey, pol->publickey)) {
                        char            name_p[NS_MAXDNAME];
                        if (-1 == ns_name_ntop(zp, name_p, sizeof(name_p)))
                            snprintf(name_p, sizeof(name_p), "unknown/error");
                        if (dnskey.public_key != NULL)
                            FREE(dnskey.public_key);
                        curkey->rr_status = VAL_AC_VERIFIED_LINK;
                        if (ta_cur->exp_ttl > 0)
                            *ttl_x = ta_cur->exp_ttl;
                        val_log(ctx, LOG_DEBUG, "key %s is trusted", name_p);
                        *status = VAL_AC_TRUST_KEY;
                        return VAL_NO_ERROR;
                    }
                    if (dnskey.public_key != NULL)
                        FREE(dnskey.public_key);
                }
            }
        }
    }

    /*
     * for the remaining nodes, see if there is any hope 
     */
    ta_tmphead = ta_cur;
    while ((zp != NULL) && zp[0]) {
        /*
         * trim the top label from our candidate zone 
         */
        zp += (int) zp[0] + 1;
        for (ta_cur = ta_tmphead; ta_cur; ta_cur = ta_cur->next) {
            if (wire_name_length(zp) < wire_name_length(ta_cur->zone_n))
                /** next time look from this point */
                ta_tmphead = ta_cur->next;

            if (namecmp(ta_cur->zone_n, zp) == 0) {
                *status = VAL_AC_WAIT_FOR_TRUST;
                return VAL_NO_ERROR;
            }
        }
    }

    val_log(ctx, LOG_DEBUG,
            "Cannot find a good trust anchor for the chain of trust above %s",
            zp);
    *status = VAL_AC_NO_TRUST_ANCHOR;
    return VAL_NO_ERROR;
}


static int
set_ans_kind(u_int8_t * qname_n,
             const u_int16_t q_type_h,
             const u_int16_t q_class_h,
             struct rrset_rec *the_set, u_int16_t * status)
{
    if ((NULL == the_set) || (NULL == status))
        return VAL_BAD_ARGUMENT;

    /*
     * Referals won't make it this far, they are handled in digest_response 
     */

    if ((the_set->rrs.val_rrset_data == NULL)
        && (the_set->rrs.val_rrset_sig != NULL)) {
        the_set->rrs_ans_kind = SR_ANS_BARE_RRSIG;
        return VAL_NO_ERROR;
    }

    /*
     * Answer is a NACK_NSEC if... 
     */
    if (the_set->rrs.val_rrset_type_h == ns_t_nsec) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_nsec))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_NSEC;

        return VAL_NO_ERROR;
    }
#ifdef LIBVAL_NSEC3
    /*
     * Answer is a NACK_NSEC3 if... 
     */
    if (the_set->rrs.val_rrset_type_h == ns_t_nsec3) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_nsec3))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_NSEC3;

        return VAL_NO_ERROR;
    }
#endif

    /*
     * Answer is a NACK_SOA if... 
     */

    if (the_set->rrs.val_rrset_type_h == ns_t_soa) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_soa))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_SOA;

        return VAL_NO_ERROR;
    }

    /*
     * Answer is a CNAME if... 
     */

    if (the_set->rrs.val_rrset_type_h == ns_t_cname) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_cname))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_CNAME;

        return VAL_NO_ERROR;
    }

    /*
     * Answer is a DNAME if... 
     */

    if (the_set->rrs.val_rrset_type_h == ns_t_dname) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_dname))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_DNAME;

        return VAL_NO_ERROR;
    }


    /*
     * Answer is an ANSWER if... 
     */
    if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) == 0 &&
        (q_type_h == ns_t_any
         || q_type_h == the_set->rrs.val_rrset_type_h)) {
        /*
         * We asked for it 
         */
        the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        return VAL_NO_ERROR;
    }

    the_set->rrs_ans_kind = SR_ANS_UNSET;
    *status = VAL_AC_DNS_ERROR_BASE + SR_WRONG_ANSWER;

    return VAL_NO_ERROR;
}

#define TOP_OF_QNAMES   0
#define MID_OF_QNAMES   1
#define NOT_IN_QNAMES   2

static int
name_in_q_names(struct qname_chain *q_names_n, u_int8_t *name_n)
{
    struct qname_chain *temp_qc;

    if ((name_n == NULL) || (q_names_n == NULL))
        return NOT_IN_QNAMES;

    if (namecmp(name_n, q_names_n->qnc_name_n) == 0)
        return TOP_OF_QNAMES;

    temp_qc = q_names_n->qnc_next;

    while (temp_qc) {
        if (namecmp(name_n, temp_qc->qnc_name_n) ==
            0)
            return MID_OF_QNAMES;
        temp_qc = temp_qc->qnc_next;
    }

    return NOT_IN_QNAMES;
}

static int
fails_to_answer_query(struct qname_chain *q_names_n,
                      const u_int16_t q_type_h,
                      const u_int16_t q_class_h,
                      struct rrset_rec *the_set, u_int16_t * status)
{
    int             name_present;
    int             type_match;
    int             class_match;
    int             data_present;

    if ((NULL == the_set) || (NULL == q_names_n) || (NULL == status)) {
        *status = VAL_AC_DNS_ERROR_BASE + SR_WRONG_ANSWER;
        return TRUE;
    }

    /*
     * If this is already a wrong answer return 
     */
    if (*status == (VAL_AC_DNS_ERROR_BASE + SR_WRONG_ANSWER))
        return TRUE;

    name_present = name_in_q_names(q_names_n, the_set->rrs.val_rrset_name_n);
    type_match = (the_set->rrs.val_rrset_type_h == q_type_h)
        || ((q_type_h == ns_t_any) && (name_present == TOP_OF_QNAMES));
    class_match = (the_set->rrs.val_rrset_class_h == q_class_h)
        || (q_class_h == ns_c_any);

    if (q_type_h != ns_t_rrsig) {
        data_present = the_set->rrs.val_rrset_data != NULL;
    } else {
        data_present = the_set->rrs.val_rrset_sig != NULL;
    }

    if (!data_present) {
        *status = VAL_AC_DNS_ERROR_BASE + SR_WRONG_ANSWER;
        return TRUE;
    }

    if (!class_match ||
        (!type_match && the_set->rrs_ans_kind == SR_ANS_STRAIGHT) ||
        (type_match && 
            the_set->rrs_ans_kind != SR_ANS_STRAIGHT && 
            the_set->rrs_ans_kind != SR_ANS_NACK_SOA &&
#ifdef LIBVAL_NSEC3
            the_set->rrs_ans_kind != SR_ANS_NACK_NSEC3 && 
#endif
            the_set->rrs_ans_kind != SR_ANS_NACK_NSEC) ||
        (name_present != TOP_OF_QNAMES && type_match &&
         the_set->rrs_ans_kind == SR_ANS_STRAIGHT) ||
        (name_present != MID_OF_QNAMES && !type_match &&
         the_set->rrs_ans_kind == SR_ANS_CNAME) ||
        (name_present != MID_OF_QNAMES && !type_match &&
         the_set->rrs_ans_kind == SR_ANS_DNAME) ||
        (name_present == MID_OF_QNAMES && !type_match &&
         (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC ||
#ifdef LIBVAL_NSEC3
          the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3 ||
#endif
          the_set->rrs_ans_kind == SR_ANS_NACK_SOA))
        ) {

        if (the_set->rrs_ans_kind == SR_ANS_CNAME &&
            type_match &&
            class_match &&
            the_set->rrs.val_rrset_data &&
            name_in_q_names(q_names_n, the_set->rrs.val_rrset_data->rr_rdata) == MID_OF_QNAMES) {
            /* synthesized CNAME */
            *status = VAL_AC_IGNORE_VALIDATION;
            return FALSE;
        }

        
        *status = VAL_AC_DNS_ERROR_BASE + SR_WRONG_ANSWER;
        return TRUE;
    }

    return FALSE;
}


/*
 * Add a new assertion for the response data 
 *
 * Returns:
 * VAL_NO_ERROR                 Operation succeeded
 * VAL_OUT_OF_MEMORY    Could not allocate enough memory for operation
 * VAL_BAD_ARGUMENT     Bad argument (eg NULL ptr)
 */
static int
add_to_authentication_chain(struct val_digested_auth_chain **assertions,
                            struct val_query_chain *matched_q,
                            struct rrset_rec *rrset)
{
    struct val_digested_auth_chain *new_as, *first_as, *last_as;
    struct rrset_rec *next_rr;

    if (NULL == assertions || matched_q == NULL)
        return VAL_BAD_ARGUMENT;

    first_as = NULL;
    last_as = NULL;

    next_rr = rrset;
    while (next_rr) {

        new_as = (struct val_digested_auth_chain *)
            MALLOC(sizeof(struct val_digested_auth_chain));

        new_as->_as.ac_data = copy_rrset_rec(next_rr);

        new_as->_as.val_ac_rrset_next = NULL;
        new_as->_as.val_ac_next = NULL;
        new_as->val_ac_status = VAL_AC_INIT;
        new_as->val_ac_query = matched_q;

        SET_MIN_TTL(matched_q->qc_ttl_x, next_rr->rrs.val_rrset_ttl_x);

        if (last_as != NULL) {
            last_as->_as.val_ac_rrset_next = new_as;
            last_as->_as.val_ac_next = new_as;
        } else {
            first_as = new_as;
        }
        last_as = new_as;
        next_rr = next_rr->rrs_next;
    }
    if (first_as) {
        last_as->_as.val_ac_next = *assertions;
        *assertions = first_as;
    }

    return VAL_NO_ERROR;
}

/*
 * Free up the authentication chain.
 */
void
free_authentication_chain(struct val_digested_auth_chain *assertions)
{

    if (assertions == NULL)
        return;

    if (assertions->_as.val_ac_next)
        free_authentication_chain(assertions->_as.val_ac_next);

    free_authentication_chain_structure(assertions);

    FREE(assertions);
}

/*
 * For a given assertion identify its pending queries
 */
static int
build_pending_query(val_context_t *context,
                    struct queries_for_query **queries,
                    struct val_digested_auth_chain *as,
                    struct queries_for_query **added_q)
{
    u_int8_t       *signby_name_n;
    u_int16_t       tzonestatus;
    int             retval;
    struct rr_rec  *cur_rr;
    u_int8_t flags;
    u_int32_t ttl_x = 0;

    if ((context == NULL) || (NULL == queries) || 
        (NULL == as) || (NULL == as->val_ac_query) || 
        (NULL == added_q))
        return VAL_BAD_ARGUMENT;

    flags = as->val_ac_query->qc_flags;
    
    if (as->_as.ac_data == NULL) {
        as->val_ac_status = VAL_AC_DATA_MISSING;
        return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs_ans_kind == SR_ANS_BARE_RRSIG) {
        as->val_ac_status = VAL_AC_BARE_RRSIG;
        return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs.val_rrset_data == NULL) {
        as->val_ac_status = VAL_AC_DATA_MISSING;
        return VAL_NO_ERROR;
    }

    /*
     * Check if this zone is locally trusted/untrusted 
     */
    if (VAL_NO_ERROR != (retval = 
        get_zse(context, as->_as.ac_data->rrs.val_rrset_name_n, &tzonestatus, &ttl_x))) {
        return retval;
    }
    SET_MIN_TTL(as->val_ac_query->qc_ttl_x, ttl_x);

    if (tzonestatus != VAL_AC_WAIT_FOR_TRUST) {
        as->val_ac_status = tzonestatus;
        return VAL_NO_ERROR;
    }

    /*
     * Check if this is a DNSKEY and it is trusted
     */
    if (as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey) {
        if (VAL_NO_ERROR !=
            (retval =
             is_trusted_key(context, as->_as.ac_data->rrs.val_rrset_name_n,
                            as->_as.ac_data->rrs.val_rrset_data,
                            &as->val_ac_status, &ttl_x))) {
            return retval;
        }
        SET_MIN_TTL(as->val_ac_query->qc_ttl_x, ttl_x);
        if (as->val_ac_status != VAL_AC_WAIT_FOR_TRUST)
            return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs.val_rrset_sig == NULL) {
        as->val_ac_status = VAL_AC_WAIT_FOR_RRSIG;
        /*
         * create a query and link it as the pending query for this assertion 
         */
        if (VAL_NO_ERROR != (retval = add_to_qfq_chain(context,
                                                       queries,
                                                       as->_as.ac_data->
                                                       rrs.
                                                       val_rrset_name_n,
                                                       ns_t_rrsig,
                                                       as->_as.ac_data->
                                                       rrs.
                                                       val_rrset_class_h,
                                                       flags,
                                                       added_q)))
            return retval;

        return VAL_NO_ERROR;
    }
    
    cur_rr = as->_as.ac_data->rrs.val_rrset_sig;
    while (cur_rr) {
        /*
         * Identify the DNSKEY that created the RRSIG:
         */
        if (cur_rr->rr_rdata == NULL) {
            cur_rr->rr_status = VAL_AC_DATA_MISSING;
        } else { 
            /*
             * First identify the signer name from the RRSIG 
             */
            signby_name_n = &cur_rr->rr_rdata[SIGNBY];
            /* The signer name has to be within the zone */
            if (namename(as->_as.ac_data->rrs.val_rrset_name_n, 
                 signby_name_n) == NULL) {
                cur_rr->rr_status = VAL_AC_INVALID_RRSIG;
            } else {
                /* set the zonecut in the assertion */
                if (as->_as.ac_data->rrs_zonecut_n == NULL) {
                    int len = wire_name_length(signby_name_n);
                    as->_as.ac_data->rrs_zonecut_n = (u_int8_t *) MALLOC (len * sizeof(u_int8_t));
                    if (as->_as.ac_data->rrs_zonecut_n == NULL)
                        return VAL_OUT_OF_MEMORY;
                    memcpy(as->_as.ac_data->rrs_zonecut_n, signby_name_n, len);
                }
                break;
            }
        }
        cur_rr = cur_rr->rr_next;
    }

    if (!cur_rr) {
        as->val_ac_status = VAL_AC_RRSIG_MISSING;
        return VAL_NO_ERROR;
    }

    /*
     * Then look for  {signby_name_n, DNSKEY/DS, type} 
     */
    if (as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey) {

        /*
         * Create a query for missing data 
         */
        if (VAL_NO_ERROR !=
            (retval =
             add_to_qfq_chain(context, queries, signby_name_n, ns_t_ds,
                              as->_as.ac_data->rrs.val_rrset_class_h, 
                              flags, added_q)))
            return retval;

    } else {
        /*
         * look for DNSKEY records 
         */
        if (VAL_NO_ERROR !=
            (retval =
             add_to_qfq_chain(context, queries, signby_name_n, ns_t_dnskey,
                              as->_as.ac_data->rrs.val_rrset_class_h, 
                              flags, added_q)))
            return retval;
    }

    as->val_ac_status = VAL_AC_WAIT_FOR_TRUST;
    return VAL_NO_ERROR;
}

static int
check_conflicting_answers(val_context_t * context,
                          struct val_digested_auth_chain *as,
                          struct queries_for_query **queries,
                          struct val_query_chain *matched_q,
                          struct qname_chain *q_names_n,
                          u_int16_t type_h, u_int16_t class_h)
{
    int             retval;
    u_int8_t        kind = SR_ANS_UNSET;
    struct queries_for_query *added_q = NULL;

    /*
     * Identify the state for each of the assertions obtained 
     */
    for (; as; as = as->_as.val_ac_rrset_next) {

        /*
         * Cover error conditions first 
         * SOA checks will appear during sanity checks later on 
         */
        if ((set_ans_kind(q_names_n->qnc_name_n, type_h, class_h,
                          as->_as.ac_data,
                          &as->val_ac_status) != VAL_NO_ERROR)
            || fails_to_answer_query(q_names_n, type_h, class_h,
                                     as->_as.ac_data,
                                     &as->val_ac_status)) {

            continue;
        }

        if (kind == SR_ANS_UNSET)
            kind = as->_as.ac_data->rrs_ans_kind;
        else {
            switch (kind) {
                /*
                 * STRAIGHT and CNAME/DNAME are OK 
                 */
            case SR_ANS_STRAIGHT:
            case SR_ANS_CNAME:
            case SR_ANS_DNAME:
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_STRAIGHT) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_CNAME) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_DNAME)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * Only bare RRSIGs together 
                 */
            case SR_ANS_BARE_RRSIG:
                if (as->_as.ac_data->rrs_ans_kind != SR_ANS_BARE_RRSIG) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * NACK_NXT and NACK_SOA are OK 
                 */
            case SR_ANS_NACK_NSEC:
#ifdef LIBVAL_NSEC3
            case SR_ANS_NACK_NSEC3:
#endif
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC) &&
#ifdef LIBVAL_NSEC3
                    /*
                     * check if there is a mix of NSEC and NSEC3 later in the proof 
                     */
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC3) &&
#endif
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_SOA)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

            case SR_ANS_NACK_SOA:
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC) &&
#ifdef LIBVAL_NSEC3
                    /*
                     * check if there is a mix of NSEC and NSEC3 later in the proof 
                     */
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC3) &&
#endif
                    (1 == 1)) {

                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * Never Reached 
                 */
            default:
                matched_q->qc_state =
                    Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
            }
        }

        if (matched_q->qc_flags & VAL_FLAGS_DONT_VALIDATE)
            as->val_ac_status = VAL_AC_IGNORE_VALIDATION;

        if (as->val_ac_status < VAL_AC_DONT_GO_FURTHER) { 

            if (VAL_NO_ERROR !=
                (retval = build_pending_query(context, queries, as, &added_q)))
                return retval;
        }
    }
    return VAL_NO_ERROR;
}

/*
 * Read the response that came in and create assertions from it. Set the state
 * of the assertion based on what data is available and whether validation
 * can proceed.
 * 
 * Returns:
 * VAL_NO_ERROR                 Operation completed successfully
 *
 */
static int
assimilate_answers(val_context_t * context,
                   struct queries_for_query **queries,
                   struct domain_info *response,
                   struct val_query_chain *matched_q)
{
    int             retval;
    u_int16_t       type_h;
    u_int16_t       class_h;
    struct val_digested_auth_chain **assertions;

    if (matched_q == NULL)
        return VAL_NO_ERROR;

    if ((NULL == context) ||
        (NULL == queries) || 
        (NULL == response) || 
        ((NULL == response->di_qnames)) || 
        (matched_q == NULL))
        return VAL_BAD_ARGUMENT;

    type_h = response->di_requested_type_h;
    class_h = response->di_requested_class_h;
    assertions = &context->a_list;

    if ((matched_q->qc_ans != NULL) || (matched_q->qc_proof != NULL)) {
        /*
         * We already had an assertion for this query 
         */
        // XXX What about FLOOD_ATTACKS ?
        return VAL_NO_ERROR;
    }

    if ((response->di_answers == NULL)
        && (response->di_proofs == NULL)) {
        matched_q->qc_state = Q_ERROR_BASE + SR_NO_ANSWER;
        return VAL_NO_ERROR;
    }

    /*
     * Create assertion for the response answers and proof 
     */

    if (response->di_answers) {
        if (VAL_NO_ERROR !=
            (retval =
             add_to_authentication_chain(assertions,
                                         matched_q,
                                         response->di_answers)))
            return retval;
        /*
         * Link the assertion to the query
         */
        matched_q->qc_ans = *assertions;
        if (VAL_NO_ERROR != (retval =
                             check_conflicting_answers(context,
                                                       *assertions,
                                                       queries, matched_q,
                                                       response->di_qnames,
                                                       type_h, class_h))) {
            return retval;
        }
    }

    if (response->di_proofs) {
        if (VAL_NO_ERROR !=
            (retval =
             add_to_authentication_chain(assertions, matched_q, response->di_proofs)))
            return retval;

        /*
         * Link the assertion to the query
         */
        matched_q->qc_proof = *assertions;
        if (VAL_NO_ERROR != (retval =
                             check_conflicting_answers(context,
                                                       *assertions,
                                                       queries, matched_q,
                                                       response->di_qnames,
                                                       type_h, class_h))) {
            return retval;
        }
    }
    return VAL_NO_ERROR;
}

static int
clone_val_rrset(struct val_rrset *old_rrset, struct val_rrset *new_rrset)
{
    int             retval;

    if (new_rrset == NULL)
        return VAL_BAD_ARGUMENT;

    memset(new_rrset, 0, sizeof(struct val_rrset));

    if (old_rrset != NULL) {
        int             len;

        CLONE_NAME_LEN(old_rrset->val_msg_header,
                       old_rrset->val_msg_headerlen,
                       new_rrset->val_msg_header,
                       new_rrset->val_msg_headerlen);

        len = wire_name_length(old_rrset->val_rrset_name_n);
        new_rrset->val_rrset_name_n =
            (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
        if (new_rrset->val_rrset_name_n == NULL) {
            retval = VAL_OUT_OF_MEMORY;
            goto err;
        }

        memcpy(new_rrset->val_rrset_name_n,
               old_rrset->val_rrset_name_n, len);

        new_rrset->val_rrset_class_h = old_rrset->val_rrset_class_h;
        new_rrset->val_rrset_type_h = old_rrset->val_rrset_type_h;
        new_rrset->val_rrset_ttl_h = old_rrset->val_rrset_ttl_h;
        new_rrset->val_rrset_ttl_x = old_rrset->val_rrset_ttl_x;
        new_rrset->val_rrset_section = old_rrset->val_rrset_section;
        new_rrset->val_rrset_data =
            copy_rr_rec_list(new_rrset->val_rrset_type_h,
                             old_rrset->val_rrset_data, 0);
        new_rrset->val_rrset_sig =
            copy_rr_rec_list(new_rrset->val_rrset_type_h,
                             old_rrset->val_rrset_sig, 0);

        if (old_rrset->val_rrset_server) {
            new_rrset->val_rrset_server =
                (struct sockaddr *) MALLOC(sizeof(struct sockaddr_storage));
            if (new_rrset->val_rrset_server == NULL) {
                retval = VAL_OUT_OF_MEMORY;
                goto err;
            }
            memcpy(new_rrset->val_rrset_server,
                   old_rrset->val_rrset_server,
                   sizeof(struct sockaddr_storage));
        } else {
            new_rrset->val_rrset_server = NULL;
        }
    }

    return VAL_NO_ERROR;

  err:
    free_val_rrset_members(new_rrset);
    memset(new_rrset, 0, sizeof(struct val_rrset));
    return retval;
}

struct val_digested_auth_chain *
get_ac_trust(val_context_t *context, 
             struct val_digested_auth_chain *next_as, 
             struct queries_for_query **queries)
{
    struct queries_for_query *added_q = NULL;

    if (!next_as ||
        !next_as->_as.ac_data ||
        !next_as->_as.ac_data->rrs_zonecut_n) {

        return NULL;
    }

    if (next_as->val_ac_status >= VAL_AC_DONT_GO_FURTHER &&
        next_as->val_ac_status <= VAL_AC_LAST_STATE)
        return NULL;
    
    /*
     * Then look for  {zonecut, DNSKEY/DS, type} 
     */
    if (next_as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey) {

        /*
         * Create a query for missing data 
         */
        if (VAL_NO_ERROR !=
             add_to_qfq_chain(context, queries, 
                              next_as->_as.ac_data->rrs_zonecut_n, 
                              ns_t_ds,
                              next_as->_as.ac_data->rrs.val_rrset_class_h, 
                              next_as->val_ac_query->qc_flags, &added_q))
            return NULL;

    } else {
        /*
         * look for DNSKEY records 
         */
        if (VAL_NO_ERROR !=
             add_to_qfq_chain(context, queries, 
                              next_as->_as.ac_data->rrs_zonecut_n, 
                              ns_t_dnskey,
                              next_as->_as.ac_data->rrs.val_rrset_class_h, 
                              next_as->val_ac_query->qc_flags, &added_q))
            return NULL;
    }

    if (added_q->qfq_query->qc_state < Q_ANSWERED)
        next_as->val_ac_status = VAL_AC_WAIT_FOR_TRUST;
    
    if (added_q->qfq_query->qc_ans)
        return added_q->qfq_query->qc_ans;
    else
        return added_q->qfq_query->qc_proof;
}


static int
transform_authentication_chain(val_context_t *context,
                               struct val_digested_auth_chain *top_as,
                               struct queries_for_query **queries,
                               struct val_authentication_chain **a_chain)
{
    struct val_authentication_chain *n_ac, *prev_ac;
    struct val_digested_auth_chain *o_ac;
    int             retval;

    if (a_chain == NULL)
        return VAL_BAD_ARGUMENT;

    (*a_chain) = NULL;
    prev_ac = NULL;
    o_ac = top_as;
    while(o_ac) {

        n_ac = (struct val_authentication_chain *)
            MALLOC(sizeof(struct val_authentication_chain));
        if (n_ac == NULL) {
            retval = VAL_OUT_OF_MEMORY;
            goto err;
        }
        memset(n_ac, 0, sizeof(struct val_authentication_chain));
        n_ac->val_ac_status = o_ac->val_ac_status;
        n_ac->val_ac_trust = NULL;
        n_ac->val_ac_rrset = (struct val_rrset *) MALLOC(sizeof(struct val_rrset));
        if (n_ac->val_ac_rrset == NULL) {
            FREE(n_ac);
            retval = VAL_OUT_OF_MEMORY;
            goto err;
        }

        if (VAL_NO_ERROR !=
            (retval =
             clone_val_rrset(o_ac->val_ac_rrset, n_ac->val_ac_rrset))) {
            FREE(n_ac->val_ac_rrset);
            FREE(n_ac);
            goto err;
        }

        if ((*a_chain) == NULL) {
            (*a_chain) = n_ac;
        } else {
            prev_ac->val_ac_trust = n_ac;
        }
        prev_ac = n_ac;

        if (prev_ac->val_ac_status == VAL_AC_NEGATIVE_PROOF ||
            prev_ac->val_ac_status == VAL_AC_PROVABLY_UNSECURE) { 

            break;
        }

        o_ac = get_ac_trust(context, o_ac, queries); 
    }

    return VAL_NO_ERROR;

  err:
    /*
     * clean up a_chain 
     */
    while (*a_chain) {
        n_ac = *a_chain;
        *a_chain = (*a_chain)->val_ac_trust;
        if (n_ac->val_ac_rrset) {
            free_val_rrset_members(n_ac->val_ac_rrset);
            FREE(n_ac->val_ac_rrset);
        }
        FREE(n_ac);
    }
    return retval;

}

#define CREATE_RESULT_BLOCK(new_res, prev_res, head_res) do {\
    new_res = (struct val_result_chain *) MALLOC (sizeof(struct val_result_chain));\
    if (new_res == NULL) {\
        return VAL_OUT_OF_MEMORY;\
    } \
    (new_res)->val_rc_answer = NULL;\
    memset((new_res)->val_rc_proofs, 0, sizeof((new_res)->val_rc_proofs));\
    (new_res)->val_rc_proof_count = 0;\
    (new_res)->val_rc_next = NULL;\
    if (prev_res == NULL) {\
        head_res = new_res;\
    } else {\
        prev_res->val_rc_next = new_res;\
    }\
    prev_res = new_res;\
} while(0)

/*
 * If proof_res is not NULL, if w_res is of type proof, store it in proof_res
 * else create a new val_result_chain structure for w_res, add add it to the
 * end of results. The new result (if created) or proof_res (if this was used)
 * is returned in *mod_res 
 */
static int
transform_single_result(val_context_t *context,
                        struct val_internal_result *w_res,
                        struct queries_for_query **queries,
                        struct val_result_chain **results,
                        struct val_result_chain *proof_res,
                        struct val_result_chain **mod_res)
{
    struct val_authentication_chain **aptr;
    struct val_result_chain *prev_res;

    if ((results == NULL) || (mod_res == NULL))
        return VAL_BAD_ARGUMENT;

    /*
     * get a pointer to the last result 
     */
    prev_res = *results;
    while (prev_res && prev_res->val_rc_next) {
        prev_res = prev_res->val_rc_next;
    }

    *mod_res = NULL;
    aptr = NULL;
    if (w_res && w_res->val_rc_is_proof) {
        if (proof_res) {
            if (proof_res->val_rc_proof_count == MAX_PROOFS) {
                proof_res->val_rc_status = VAL_BOGUS_PROOF;
                *mod_res = proof_res;
                return VAL_NO_ERROR;
            } else {
                aptr =
                    &proof_res->val_rc_proofs[proof_res->
                                              val_rc_proof_count];
            }
        } else {
            CREATE_RESULT_BLOCK(proof_res, prev_res, *results);
            aptr = &proof_res->val_rc_proofs[0];
        }
        proof_res->val_rc_proof_count++;
        *mod_res = proof_res;
    } else {
        CREATE_RESULT_BLOCK(proof_res, prev_res, *results);
        aptr = &proof_res->val_rc_answer;
        *mod_res = proof_res;
    }
    *aptr = NULL;
    if (w_res) {
        w_res->val_rc_consumed = 1;
        return transform_authentication_chain(context, w_res->val_rc_rrset, queries, aptr);
    }

    return VAL_NO_ERROR;
}

/*
 * Transform the val_internal_result structure
 * into the results structure. If proofs exist, they are placed
 * together in a single val_result_chain structure.
 */
static int
transform_outstanding_results(val_context_t *context,
                              struct val_internal_result *w_results,
                              struct queries_for_query **queries,
                              struct val_result_chain **results,
                              struct val_result_chain *proof_res,
                              val_status_t proof_status)
{
    struct val_internal_result *w_res;
    struct val_result_chain *new_res;
    int             retval;

    if (results == NULL)
        return VAL_BAD_ARGUMENT;

    w_res = w_results;
    /*
     * for each remaining internal result 
     */
    while (w_res) {

        if (!w_res->val_rc_consumed) {
            if (VAL_NO_ERROR !=
                (retval =
                 transform_single_result(context, w_res, queries, results, proof_res,
                                         &new_res))) {
                goto err;
            }

            if (w_res->val_rc_is_proof) {
                proof_res = new_res;
                proof_res->val_rc_status = proof_status;
            } else {
                /*
                 * Update the result 
                 */
                new_res->val_rc_status = w_res->val_rc_status;
            }
        }

        w_res = w_res->val_rc_next;
    }
    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    return retval;
}


static void
prove_nsec_wildcard_check(val_context_t * ctx,
                          u_int16_t qc_type_h,
                          struct rrset_rec *wcard_proof,
                          u_int8_t * closest_encounter,
                          val_status_t * status)
{
    /*
     * Check the wild card proof 
     */
    /*
     * prefix "*" to the closest encounter, and check if that 
     * name falls within the range given in wcard_proof
     */
    u_int8_t       *nxtname;
    u_char          domain_name_n[NS_MAXCDNAME];

    /* Input params have been NULL-checked in the calling function */
    
    if ((NULL == wcard_proof) || (NULL == wcard_proof->rrs.val_rrset_data))
        nxtname = NULL;
    else
        nxtname = wcard_proof->rrs.val_rrset_data->rr_rdata;

    if (NS_MAXCDNAME < wire_name_length(closest_encounter) + 2) {
        val_log(ctx, LOG_DEBUG,
                "NSEC Error: label length with wildcard exceeds bounds");
        *status = VAL_BOGUS_PROOF;
        return;
    }

    domain_name_n[0] = 0x01;
    domain_name_n[1] = 0x2a;    /* for the '*' character */
    if (closest_encounter)
        memcpy(&domain_name_n[2], closest_encounter,
               wire_name_length(closest_encounter));
    /*
     *  either we should be able to prove that wild card does not exist, 
     *  or that type is not present at that wild card 
     */
    if ((nxtname != NULL) &&
        (!namecmp(domain_name_n, wcard_proof->rrs.val_rrset_name_n))) {

        int             nsec_bit_field;
        nsec_bit_field =
            wire_name_length(wcard_proof->rrs.val_rrset_data->rr_rdata);
        if (is_type_set
            ((&
              (wcard_proof->rrs.val_rrset_data->rr_rdata[nsec_bit_field])),
             wcard_proof->rrs.val_rrset_data->rr_rdata_length_h -
             nsec_bit_field, qc_type_h)) {
            val_log(ctx, LOG_DEBUG, "NSEC error: type exists at wildcard");
            *status = VAL_BOGUS_PROOF;
        } else if (is_type_set
                   ((&
                     (wcard_proof->rrs.val_rrset_data->
                      rr_rdata[nsec_bit_field])),
                    wcard_proof->rrs.val_rrset_data->rr_rdata_length_h -
                    nsec_bit_field, ns_t_cname)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: CNAME exists at wildcard");
            *status = VAL_BOGUS_PROOF;
        } else if (is_type_set
                   ((&
                     (wcard_proof->rrs.val_rrset_data->
                      rr_rdata[nsec_bit_field])),
                    wcard_proof->rrs.val_rrset_data->rr_rdata_length_h -
                    nsec_bit_field, ns_t_dname)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: DNAME exists at wildcard");
            *status = VAL_BOGUS_PROOF;
        }

    } else if ((nxtname == NULL) ||
               (namecmp(domain_name_n, wcard_proof->rrs.val_rrset_name_n) <
                0) || (namecmp(nxtname, domain_name_n) < 0)) {
        val_log(ctx, LOG_DEBUG,
                "NSEC error: Incorrect span for wildcard proof");
        *status = VAL_BOGUS_PROOF;
    }
}

static void
prove_nsec_span_chk(val_context_t * ctx,
                    struct rrset_rec *the_set, u_int8_t * qname_n,
                    u_int16_t qtype_h, u_int8_t * soa_name_n,
                    int *span_chk, int *wcard_chk,
                    u_int8_t ** closest_encounter, val_status_t * status)
{

    int             nsec_bit_field;

    /* Input variables have been NULL-checked in the calling function */
    
    if (!namecmp(the_set->rrs.val_rrset_name_n, qname_n)) {
        struct rr_rec  *sig;

        /*
         * NSEC owner = query name & q_type not in list 
         */
        nsec_bit_field =
            wire_name_length(the_set->rrs.val_rrset_data->rr_rdata);
        if (is_type_set
            ((&(the_set->rrs.val_rrset_data->rr_rdata[nsec_bit_field])),
             the_set->rrs.val_rrset_data->rr_rdata_length_h -
             nsec_bit_field, qtype_h)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: Type exists at NSEC record");
            *status = VAL_BOGUS_PROOF;
            return;
        } else if (is_type_set
                   ((&
                     (the_set->rrs.val_rrset_data->
                      rr_rdata[nsec_bit_field])),
                    the_set->rrs.val_rrset_data->rr_rdata_length_h -
                    nsec_bit_field, ns_t_cname)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: CNAME exists at NSEC record, but was not checked");
            *status = VAL_BOGUS_PROOF;
            return;
        } else if (is_type_set
                   ((&
                     (the_set->rrs.val_rrset_data->
                      rr_rdata[nsec_bit_field])),
                    the_set->rrs.val_rrset_data->rr_rdata_length_h -
                    nsec_bit_field, ns_t_dname)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: DNAME exists at NSEC record, but was not checked");
            *status = VAL_BOGUS_PROOF;
            return;
        }

        *span_chk = 1;
        *status = VAL_NONEXISTENT_TYPE;

        /*
         * if the label count in the RRSIG equals the labels
         * in the nsec owner name, wildcard absence is also proved
         * If a wildcard was used, the status would be 
         * VAL_AC_WCARD_VERIFIED instead of VAL_AC_RRSIG_VERIFIED
         * proofs should not be expanded from wildcards
         */
        for (sig = the_set->rrs.val_rrset_sig; sig; sig = sig->rr_next) {
            if (sig->rr_status == VAL_AC_RRSIG_VERIFIED ||
                sig->rr_status == VAL_AC_RRSIG_VERIFIED_SKEW) {
                *wcard_chk = 1;
                return;
            }
        }
    } else if (namecmp(the_set->rrs.val_rrset_name_n, qname_n) > 0) {
        /*
         * query name comes after the NSEC owner 
         */
        return;
    }

    /*
     * Find the next name 
     */
    u_int8_t       *nxtname = the_set->rrs.val_rrset_data ?
        the_set->rrs.val_rrset_data->rr_rdata : NULL;

    if (namecmp(qname_n, nxtname) >= 0) {
        /*
         * check if the next name wraps around 
         */
        if (namecmp(nxtname, soa_name_n) != 0) {
            /*
             * if no, check if this is the proof for no wild-card present 
             * i.e the proof must tell us that "*" does not exist 
             */
            return;
        }
    }

    /*
     * else 
     */
    *status = VAL_NONEXISTENT_NAME; /** This can change later on if wildcard checks fail **/
    *span_chk = 1;

    /*
     * The closest encounter is the longest label match between 
     * this NSEC's owner name and the query name
     */
    int             maxoffset = wire_name_length(qname_n);
    int             offset = qname_n[0] + 1;
    while (offset < maxoffset) {
        u_int8_t       *cur_name_n = &qname_n[offset];
        int             cmp;
        if ((cmp =
             namecmp(cur_name_n, the_set->rrs.val_rrset_name_n)) == 0) {
            *closest_encounter = cur_name_n;
            break;
        } else if (cmp < 0) {
            /*
             * strip off one label from the NSEC owner name 
             */
            *closest_encounter = the_set->rrs.val_rrset_name_n ?
                &the_set->rrs.val_rrset_name_n[the_set->rrs.
                                               val_rrset_name_n[0] +
                                               1] : NULL;
            break;
        }
        offset += cur_name_n[0] + 1;
    }

    return;
}


#ifdef LIBVAL_NSEC3
u_int8_t       *
compute_nsec3_hash(val_context_t * ctx, u_int8_t * qname_n,
                   u_int8_t * soa_name_n, u_int8_t alg, u_int16_t iter,
                   u_int8_t saltlen, u_int8_t * salt,
                   u_int8_t * b32_hashlen, u_int8_t ** b32_hash, u_int32_t *ttl_x)
{
    int             name_len;
    policy_entry_t *pol, *cur;
    u_int8_t       *p;
    char            name_p[NS_MAXDNAME];
    u_int8_t        hashlen;
    u_int8_t       *hash;

    if (alg != ALG_NSEC3_HASH_SHA1)
        return NULL;

    pol = NULL;

    if (soa_name_n != NULL) {
        name_len = wire_name_length(soa_name_n);
        RETRIEVE_POLICY(ctx, P_NSEC3_MAX_ITER, pol);

        if (pol != NULL) {
            /*
             * go past longer names 
             */
            for (cur = pol;
                 cur && (wire_name_length(cur->zone_n) > name_len);
                 cur = cur->next);
    
            /*
             * for all zones which are shorter or as long, do a strstr 
             */
            /*
             * Because of the ordering, the longest match is found first 
             */
            for (; cur; cur = cur->next) {
                int             root_zone = 0;
                if (!namecmp(cur->zone_n, (const u_int8_t *) ""))
                    root_zone = 1;
                else {
                    /*
                     * Find the last occurrence of cur->zone_n in soa_name_n 
                     */
                    p = soa_name_n;
                    while (p && (*p != '\0')) {
                        if (!namecmp(p, cur->zone_n))
                            break;
                        p = p + *p + 1;
                    }
                }
    
                if (root_zone || !namecmp(p, cur->zone_n)) {
                    if (-1 == ns_name_ntop(soa_name_n, name_p, sizeof(name_p)))
                        snprintf(name_p, sizeof(name_p), "unknown/error");
    
                    if (cur->pol != NULL) {
                        if (cur->exp_ttl > 0)
                            *ttl_x = cur->exp_ttl;
                        if (((struct nsec3_max_iter_policy *)(cur->pol))->iter < iter)
                            return NULL;
                    }
                    break;
                }
            }
        }
    }

    if (NULL ==
        nsec3_sha_hash_compute(qname_n, salt, saltlen, iter, &hash,
                               &hashlen))
        return NULL;

    base32hex_encode(hash, hashlen, b32_hash, b32_hashlen);
    FREE(hash);
    return *b32_hash;
}


static int
prove_nsec3_span_chk( val_context_t * ctx, struct val_internal_result *w_results,
                      struct queries_for_query **queries,
                      struct val_result_chain **proof_res,
                      struct val_result_chain **results,
                      u_int8_t * qname_n, u_int16_t qtype_h,
                      u_int8_t * soa_name_n, val_status_t * status, u_int32_t *ttl_x,
                      u_int8_t       **ncn,
                      u_int8_t       **cpe,
                      int *optout) 
{
    struct val_result_chain *new_res;
    struct val_internal_result *res;
    u_int8_t       *cp = NULL;
    val_nsec3_rdata_t nd;
    u_int8_t        nsec3_hashlen;
    u_int8_t       *nsec3_hash = NULL;
    u_int8_t        hashlen;
    u_int8_t       *hash = NULL;
    struct val_internal_result *ncn_res = NULL;
    struct val_internal_result *cpe_res = NULL;
    int             retval;

    /* Input params have been NULL checked in calling function */
    
    cp = qname_n;
    *optout = 0;
    
    while ((namecmp(cp, soa_name_n) >= 0) && !*cpe) {

        /*
         * we have all the data we're looking for 
         */
        if (*ncn && ((*ncn == *cpe) || (*cpe == (*ncn + *ncn[0] + 1)))) {
            break;
        }

        for (res = w_results; res; res = res->val_rc_next) {
            struct rrset_rec *the_set;

            if (!res->val_rc_is_proof) {
                continue;
            }

            the_set = res->val_rc_rrset->_as.ac_data;
            if (the_set->rrs_ans_kind != SR_ANS_NACK_NSEC3)
                continue;

            nsec3_hashlen = the_set->rrs.val_rrset_name_n[0];
            nsec3_hash =
                (nsec3_hashlen ==
                 0) ? NULL : the_set->rrs.val_rrset_name_n + 1;

            if (NULL ==
                val_parse_nsec3_rdata(the_set->rrs.val_rrset_data->
                                      rr_rdata,
                                      the_set->rrs.val_rrset_data->
                                      rr_rdata_length_h, &nd)) {
                val_log(ctx, LOG_DEBUG, "Cannot parse NSEC3 rdata");
                *status = VAL_BOGUS_PROOF;
                return VAL_NO_ERROR;
            }

            /*
             * hash name according to nsec3 parameters 
             */
            if (NULL ==
                compute_nsec3_hash(ctx, cp, soa_name_n, nd.alg,
                                   nd.iterations, nd.saltlen, nd.salt,
                                   &hashlen, &hash, ttl_x)) {
                val_log(ctx, LOG_DEBUG,
                        "Cannot compute NSEC3 hash with given params");
                *status = VAL_BOGUS_PROOF;
                FREE(nd.nexthash);
                return VAL_NO_ERROR;
            }
            
            /*
             * Check if there is an exact match 
             */
            if ((nsec3_hashlen == hashlen)
                && !memcmp(hash, nsec3_hash, hashlen)) {
                struct rr_rec  *sig;
                int             nsec3_bm_len =
                    the_set->rrs.val_rrset_data->rr_rdata_length_h -
                    nd.bit_field;

                /*
                 * This is the closest provable encounter 
                 */
                *cpe = cp;
                cpe_res = res;
                /*
                 * NS can only be set if the SOA bit is set 
                 */
                if (qtype_h == ns_t_ds && 
                    (is_type_set
                     ((&
                       (the_set->rrs.val_rrset_data->
                        rr_rdata[nd.bit_field])), nsec3_bm_len, ns_t_ns)) &&
                    (!is_type_set
                     ((&
                       (the_set->rrs.val_rrset_data->
                        rr_rdata[nd.bit_field])), nsec3_bm_len,
                      ns_t_soa))) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: NS can only be set if the SOA bit is set");
                    *status = VAL_BOGUS_PROOF;
                    FREE(nd.nexthash);
                    FREE(hash);
                    return VAL_NO_ERROR;
                }
                /*
                 * hashes match 
                 */
                if (cp == qname_n) {
                    /*
                     * this is the query name 
                     * make sure that type is missing 
                     */
                    if (is_type_set
                        ((&
                          (the_set->rrs.val_rrset_data->
                           rr_rdata[nd.bit_field])), nsec3_bm_len,
                         qtype_h)) {
                        val_log(ctx, LOG_DEBUG,
                                "NSEC3 error: Type exists at NSEC3 record");
                        *status = VAL_BOGUS_PROOF;
                        FREE(nd.nexthash);
                        FREE(hash);
                        return VAL_NO_ERROR;
                    } else if (is_type_set
                               ((&
                                 (the_set->rrs.val_rrset_data->
                                  rr_rdata[nd.bit_field])), nsec3_bm_len,
                                ns_t_cname)) {
                        val_log(ctx, LOG_DEBUG,
                                "NSEC3 error: CNAME exists at NSEC3 record, but was not checked");
                        *status = VAL_BOGUS_PROOF;
                        FREE(nd.nexthash);
                        FREE(hash);
                        return VAL_NO_ERROR;
                    } else if (is_type_set
                               ((&
                                 (the_set->rrs.val_rrset_data->
                                  rr_rdata[nd.bit_field])), nsec3_bm_len,
                                ns_t_dname)) {
                        val_log(ctx, LOG_DEBUG,
                                "NSEC3 error: DNAME exists at NSEC3 record, but was not checked");
                        *status = VAL_BOGUS_PROOF;
                        FREE(nd.nexthash);
                        FREE(hash);
                        return VAL_NO_ERROR;
                    }

                    /*
                     * This proof is relevant 
                     */
                    if (VAL_NO_ERROR !=
                        (retval =
                         transform_single_result(ctx, res, queries, results, 
                                                 *proof_res, &new_res))) {
                        goto err;
                    }
                    *proof_res = new_res;

                    /*
                     * if the label count in the RRSIG equals the labels
                     * in the nsec owner name, wildcard absence is also proved
                     * If a wildcard was used, the status would be 
                     * VAL_AC_WCARD_VERIFIED instead of VAL_AC_RRSIG_VERIFIED
                     * Proofs sould not be expanded from wildcards
                     */
                    for (sig = the_set->rrs.val_rrset_sig; sig;
                         sig = sig->rr_next) {
                        if (sig->rr_status == VAL_AC_RRSIG_VERIFIED ||
                            sig->rr_status == VAL_AC_RRSIG_VERIFIED_SKEW) {
                            /*
                             * proof complete 
                             */
                            *status = VAL_NONEXISTENT_TYPE;
                            (*proof_res)->val_rc_status = *status;
                            FREE(nd.nexthash);
                            FREE(hash);
                            return VAL_NO_ERROR;
                        }
                    }

                    *status = VAL_BOGUS_PROOF;
                    (*proof_res)->val_rc_status = *status;
                    FREE(nd.nexthash);
                    FREE(hash);
                    return VAL_NO_ERROR;
                }
            }

            /*
             * Check if NSEC3 covers the hash 
             */
            if (CHECK_RANGE
                (nsec3_hash, nsec3_hashlen, nd.nexthash, nd.nexthashlen,
                 hash, hashlen)) {
                *ncn = cp;
                ncn_res = res;
                if (nd.flags & NSEC3_FLAG_OPTOUT) {
                    *optout = 1;
                } else {
                    *optout = 0;
                }
            }

            FREE(nd.nexthash);
            FREE(hash);
        }

        /*
         * strip leading label 
         */
        cp += cp[0] + 1;
    }

    if (ncn_res) {
        /*
         * This proof is relevant 
         */
        if (VAL_NO_ERROR !=
            (retval =
             transform_single_result(ctx, ncn_res, queries, results, 
                                     *proof_res, &new_res))) {
            goto err;
        }
        *proof_res = new_res;
    }
    if (cpe_res && (cpe_res != ncn_res)) {
        /*
         * This proof is relevant 
         */
        if (VAL_NO_ERROR !=
            (retval =
             transform_single_result(ctx, cpe_res, queries, results, 
                                     *proof_res, &new_res))) {
            goto err;
        }
        *proof_res = new_res;
    }

    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    *proof_res = NULL;
    return retval;
}

static int
prove_nsec3_wildcard_check(val_context_t * ctx, struct val_internal_result *w_results,
                           struct queries_for_query **queries,
                           u_int16_t qtype_h,
                           struct val_result_chain **proof_res,
                           struct val_result_chain **results,
                           u_int8_t * soa_name_n, val_status_t * status, u_int32_t *ttl_x,
                           u_int8_t *cpe, int optout)
{
    u_char          wc_n[NS_MAXCDNAME];
    struct val_internal_result *res;
    val_nsec3_rdata_t nd;
    u_int8_t        nsec3_hashlen;
    u_int8_t       *nsec3_hash = NULL;
    u_int8_t        hashlen;
    u_int8_t       *hash = NULL;
    struct val_result_chain *new_res;
    int retval;

    /* Input variables have been NULL checked in calling function */
    
    /*
     * Check for wildcard 
     */
    /*
     * Create a the name *.cpe 
     */
    memset(wc_n, 0, sizeof(wc_n));
    wc_n[0] = 0x01;
    wc_n[1] = 0x2a;             /* for the '*' character */
    memcpy(&wc_n[2], cpe, wire_name_length(cpe));

    for (res = w_results; res; res = res->val_rc_next) {
        if (!res->val_rc_rrset) {
            continue;
        }
        struct rrset_rec *the_set = res->val_rc_rrset->_as.ac_data;
        if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3) {

            nsec3_hashlen = the_set->rrs.val_rrset_name_n[0];
            nsec3_hash =
                (nsec3_hashlen ==
                 0) ? NULL : the_set->rrs.val_rrset_name_n + 1;

            if (NULL ==
                val_parse_nsec3_rdata(the_set->rrs.val_rrset_data->
                                      rr_rdata,
                                      the_set->rrs.val_rrset_data->
                                      rr_rdata_length_h, &nd)) {
                val_log(ctx, LOG_DEBUG,
                        "NSEC3 error: Cannot parse NSEC3 rdata");
                *status = VAL_BOGUS_PROOF;
                return VAL_NO_ERROR;
            }

            /*
             * hash name according to nsec3 parameters 
             */
            if (NULL ==
                compute_nsec3_hash(ctx, wc_n, soa_name_n, nd.alg,
                                   nd.iterations, nd.saltlen, nd.salt,
                                   &hashlen, &hash, ttl_x)) {
                val_log(ctx, LOG_DEBUG,
                        "NSEC3 error: Cannot compute hash with given params");
                FREE(nd.nexthash);
                *status = VAL_BOGUS_PROOF;
                return VAL_NO_ERROR;
            }

            if (!nsec3_order_cmp(nsec3_hash, nsec3_hashlen, hash, hashlen)) {
                /*
                 * if type is set, that's a problem 
                 */
                if (is_type_set
                    ((&
                      (the_set->rrs.val_rrset_data->
                       rr_rdata[nd.bit_field])),
                     the_set->rrs.val_rrset_data->rr_rdata_length_h -
                     nd.bit_field, qtype_h)) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: wildcard proof does not prove non-existence");
                    *status = VAL_BOGUS_PROOF;
                } else if (is_type_set
                           ((&
                             (the_set->rrs.val_rrset_data->
                              rr_rdata[nd.bit_field])),
                            the_set->rrs.val_rrset_data->
                            rr_rdata_length_h - nd.bit_field,
                            ns_t_cname)) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: wildcard proof has CNAME");
                    *status = VAL_BOGUS_PROOF;
                } else if (is_type_set
                           ((&
                             (the_set->rrs.val_rrset_data->
                              rr_rdata[nd.bit_field])),
                            the_set->rrs.val_rrset_data->
                            rr_rdata_length_h - nd.bit_field,
                            ns_t_dname)) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: wildcard proof has DNAME");
                    *status = VAL_BOGUS_PROOF;
                } else
                    *status = VAL_NONEXISTENT_TYPE;
                FREE(nd.nexthash);
                FREE(hash);
                return VAL_NO_ERROR;
            } else
                if (CHECK_RANGE
                    (nsec3_hash, nsec3_hashlen, nd.nexthash,
                     nd.nexthashlen, hash, hashlen)) {
                /*
                 * proved 
                 */

                /*
                 * This proof is relevant 
                 */
                if (VAL_NO_ERROR !=
                    (retval =
                     transform_single_result(ctx, res, queries, results, 
                                             *proof_res, &new_res))) {
                    goto err;
                }
                *proof_res = new_res;

                FREE(nd.nexthash);
                FREE(hash);
                if (optout) {
                    *status = VAL_NONEXISTENT_NAME_OPTOUT;
                } else {
                    *status = VAL_NONEXISTENT_NAME;
                }
                return VAL_NO_ERROR;
            }

            FREE(nd.nexthash);
            FREE(hash);
        }
    }

    val_log(ctx, LOG_DEBUG, "NSEC3 error: wildcard proof does not exist");
    /*
     * Could not find a proof covering the wildcard 
     */
    *status = VAL_BOGUS_PROOF;
    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    *proof_res = NULL;
    return retval;
}

static int
nsec3_proof_chk(val_context_t * ctx, struct val_internal_result *w_results,
                struct queries_for_query **queries,
                int only_span_chk,
                struct val_result_chain **proof_res,
                struct val_result_chain **results,
                u_int8_t * qname_n, u_int16_t qtype_h,
                u_int8_t * soa_name_n, val_status_t * status, u_int32_t *ttl_x)
{

    int retval;
    u_int8_t       *ncn = NULL;
    u_int8_t       *cpe = NULL;
    int optout = 0;

    if (ctx == NULL || queries == NULL || proof_res == NULL || results == NULL ||
        qname_n == NULL || soa_name_n == NULL || status == NULL || 
        ttl_x == NULL) {

        return VAL_BAD_ARGUMENT;
    }

    if (VAL_NO_ERROR != (retval = 
            prove_nsec3_span_chk(ctx, w_results, queries, proof_res, 
                    results, qname_n, qtype_h, soa_name_n, status, ttl_x,
                    &ncn, &cpe, &optout))) {

        *status = VAL_BOGUS_PROOF;
        return retval;
    }
  
    if (!ncn) {
        val_log(ctx, LOG_DEBUG, "NSEC3 error: NCN was not found");
        *status = VAL_INCOMPLETE_PROOF;
        return VAL_NO_ERROR;
    }
    
    if (only_span_chk) {
        return VAL_NO_ERROR;
    }
    
    if (!cpe) {
        val_log(ctx, LOG_DEBUG, "NSEC3 error: CPE was not found");
        *status = VAL_INCOMPLETE_PROOF;
        return VAL_NO_ERROR;
    }

    /*
     * if ncn is not one label greater than cpe then we have a problem 
     */
    if ((ncn != cpe) && (cpe != (ncn + ncn[0] + 1))) {
        val_log(ctx, LOG_DEBUG,
                "NSEC3 error: NCN is not one label greater than CPE");
        *status = VAL_BOGUS_PROOF;
        return VAL_NO_ERROR;
    }

    if (NS_MAXCDNAME < wire_name_length(cpe) + 2) {
        val_log(ctx, LOG_DEBUG,
                "NSEC3 Error: label length with wildcard exceeds bounds");
        *status = VAL_BOGUS_PROOF;
        return VAL_NO_ERROR;
    }

    if (VAL_NO_ERROR != (retval = 
                prove_nsec3_wildcard_check(ctx, w_results, queries, qtype_h, 
                                           proof_res, results, soa_name_n,
                                           status, ttl_x, cpe, optout))) {
        *status = VAL_BOGUS_PROOF;
        return retval;
    }
    
    return VAL_NO_ERROR;

}
#endif



static int
nsec_proof_chk(val_context_t * ctx, struct val_internal_result *w_results,
               struct queries_for_query **queries,
               int only_span_chk,
               struct val_result_chain **proof_res,
               struct val_result_chain **results,
               u_int8_t * qname_n, u_int16_t qtype_h,
               u_int8_t * soa_name_n, val_status_t * status)
{
    struct val_internal_result *res;
    int             wcard_chk = 0;
    int             span_chk = 0;
    u_int8_t       *closest_encounter = NULL;
    struct rrset_rec *wcard_proof = NULL;
    struct val_result_chain *new_res;
    int             retval;

    if (ctx == NULL || queries == NULL || proof_res == NULL ||
        proof_res == NULL || results == NULL ||
        qname_n == NULL || soa_name_n == NULL || status == NULL) {

        return VAL_BAD_ARGUMENT;
    }
    
    for (res = w_results; res; res = res->val_rc_next) {

        if (!res->val_rc_is_proof)
            continue;

        struct rrset_rec *the_set = res->val_rc_rrset->_as.ac_data;
        if (the_set->rrs_ans_kind != SR_ANS_NACK_NSEC)
            continue;

        if (!span_chk) {
            prove_nsec_span_chk(ctx, the_set, qname_n,
                            qtype_h, soa_name_n, &span_chk,
                            &wcard_chk, &closest_encounter, status);
            if (*status != VAL_DONT_KNOW) {
                /*
                 * This proof is relevant 
                 */
                if (VAL_NO_ERROR !=
                    (retval = transform_single_result(ctx, res, queries, results,
                                                  *proof_res, &new_res))) {
                    goto err;
                }
                *proof_res = new_res;
            }
            if (wcard_chk) {
                wcard_proof = the_set;
                break;
            }

            if (wcard_proof == NULL)
                wcard_proof = the_set;
        } else { 
            wcard_proof = the_set;
            if (VAL_NO_ERROR !=
                    (retval = transform_single_result(ctx, res, queries, results,
                                                  *proof_res, &new_res))) {
                goto err;
            }
            *proof_res = new_res;
            break;
        }
    }

    if (!span_chk)
        *status = VAL_INCOMPLETE_PROOF;
    else if (only_span_chk == 0 && !wcard_chk) {
        if (!closest_encounter)
            *status = VAL_INCOMPLETE_PROOF;
        else {
            prove_nsec_wildcard_check(ctx, qtype_h,
                                      wcard_proof,
                                      closest_encounter, status);
        }
    }
    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    *proof_res = NULL;
    return retval;
}

#define GET_HEADER_STATUS_CODE(qc_proof, status_code) do {\
    if (qc_proof &&\
        qc_proof->val_ac_rrset &&\
        qc_proof->val_ac_rrset->val_msg_header) {\
        HEADER *hp = (HEADER *) qc_proof->val_ac_rrset->val_msg_header;\
        if (hp->rcode == ns_r_noerror) {\
            status_code = VAL_NONEXISTENT_TYPE_NOCHAIN;\
        } else if (hp->rcode == ns_r_nxdomain) {\
            status_code = VAL_NONEXISTENT_NAME_NOCHAIN;\
        } else\
            status_code = VAL_ERROR;\
    } else { \
        status_code = VAL_ERROR;\
    }\
}while (0)


static int
prove_nonexistence(val_context_t * ctx,
                   struct val_internal_result *w_results,
                   struct queries_for_query **queries,
                   struct val_result_chain **proof_res,
                   struct val_result_chain **results,
                   u_char * qname_n,
                   u_int16_t qtype_h,
                   u_int16_t qc_class_h,
                   int only_span_chk,
                   u_int8_t *soa_name_n_in,
                   struct val_digested_auth_chain *qc_proof,
                   val_status_t * status,
                   u_int32_t *soa_ttl_x)
{
    struct val_internal_result *res;
    u_int8_t       *soa_name_n = NULL;
    char            name_p[NS_MAXDNAME];
    int             retval;

    int             nsec = 0;
    int             proof_seen = 0;
#ifdef LIBVAL_NSEC3
    int             nsec3 = 0;
    u_int32_t       ttl_x = 0;
#endif

    if (proof_res == NULL)
        return VAL_BAD_ARGUMENT;

    *status = VAL_DONT_KNOW;

    if (-1 == ns_name_ntop(qname_n, name_p, sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
    val_log(ctx, LOG_DEBUG, "proving non-existence for {%s, %d, %d}",
            name_p, qc_class_h, qtype_h);

    /*
     * Check if this is the whole proof and nothing but the proof
     * At this point these records should already be in the TRUSTED state.
     */

    /*
     * inspect the SOA record first 
     */
    if (soa_name_n_in != NULL) {
        soa_name_n = soa_name_n_in;
    } else {
        for (res = w_results; res; res = res->val_rc_next) {
            struct rrset_rec *the_set = res->val_rc_rrset->_as.ac_data;
            int offset;
            if ((the_set) && (the_set->rrs_ans_kind == SR_ANS_NACK_SOA)) {
                struct val_result_chain *new_res;
                /*
                 * This proof is relevant 
                 */
                if (VAL_NO_ERROR != (retval =
                                     transform_single_result(ctx, res, queries, results,
                                                             *proof_res,
                                                             &new_res))) {
                    goto err;
                }
                *proof_res = new_res;
    
                /* Use the SOA minimum time */
                soa_name_n = the_set->rrs.val_rrset_name_n;
                if (the_set->rrs.val_rrset_data &&
                    the_set->rrs.val_rrset_data->rr_rdata &&
                    (offset = the_set->rrs.val_rrset_data->rr_rdata_length_h - sizeof(u_int32_t)) > 0) {
                    u_int32_t t_ttl;
                    memcpy(&t_ttl, &the_set->rrs.val_rrset_data[offset], sizeof(u_int32_t));
                    *soa_ttl_x = ntohl(t_ttl); 
                } else {
                    *soa_ttl_x = the_set->rrs.val_rrset_ttl_x;
                }
                break;
            }
        }
    
        if (res == NULL)
            *status = VAL_INCOMPLETE_PROOF;
        /*
         * check if trusted and complete 
         */
        else if (val_istrusted(res->val_rc_status) &&
                 !val_isvalidated(res->val_rc_status)) {
            /*
             * use the error code as status 
             */
            GET_HEADER_STATUS_CODE(qc_proof, *status);
            /*
             * Collect all other proofs 
             */
            retval =
                transform_outstanding_results(ctx, w_results, queries, results, *proof_res,
                                              *status);
    
            return VAL_NO_ERROR;
        }
    }

    /*
     * Perform general sanity check of proofs
     */
    for (res = w_results; res; res = res->val_rc_next) {
        if (!res->val_rc_is_proof)
            continue;

        struct rrset_rec *the_set = res->val_rc_rrset->_as.ac_data;
        if ((!the_set) || (!the_set->rrs.val_rrset_data)) {
            *status = VAL_BOGUS_PROOF;
            return VAL_NO_ERROR;
        }

        if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC) {
            nsec = 1;
        }
#ifdef LIBVAL_NSEC3
        else if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3) {
            nsec3 = 1;
        }
#endif
    }

    /*
     * Check if we received NSEC and NSEC3 proofs 
     */
    proof_seen = nsec ? 1 : 0;
#ifdef LIBVAL_NSEC3
    proof_seen = nsec3 ? (proof_seen == 0) : proof_seen;
#endif
    if (!proof_seen)
        *status = VAL_BOGUS_PROOF;
    else if (nsec) {
        /*
         * only nsec records 
         */
        if (VAL_NO_ERROR !=
            (retval =
             nsec_proof_chk(ctx, w_results, queries, only_span_chk, 
                            proof_res, results, qname_n,
                            qtype_h, soa_name_n, status)))
            goto err;
    }
#ifdef LIBVAL_NSEC3
    else if (nsec3) {
        /*
         * only nsec3 records 
         */
        if (VAL_NO_ERROR !=
            (retval =
             nsec3_proof_chk(ctx, w_results, queries, only_span_chk,
                             proof_res, results, qname_n,
                             qtype_h, soa_name_n, status, &ttl_x)))
            goto err;
        
        if (qc_proof) {
            SET_MIN_TTL(qc_proof->val_ac_query->qc_ttl_x, ttl_x);
        }
        
        /*
         * If this is opt-out use the error code as status 
         */
        if (*status == VAL_NONEXISTENT_NAME_OPTOUT) {
            GET_HEADER_STATUS_CODE(qc_proof, *status);
        }
    }
#endif

    /*
     * passed all tests 
     */
    if (*status == VAL_DONT_KNOW)
        *status = VAL_NONEXISTENT_NAME;

    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    *proof_res = NULL;
    return retval;
}

/*
 * find the soa for this name and type 
 */
static int
find_next_soa(val_context_t * context, struct queries_for_query **queries,
              u_int8_t * qname, int *done, u_int8_t ** name_n)
{
    int             retval;
    struct val_result_chain *results = NULL;
    struct val_rrset *soa_rrset = NULL;

    if (context == NULL || queries == NULL || name_n == NULL || done == NULL)
        return VAL_BAD_ARGUMENT;

    *name_n = NULL;
    *done = 1;

    if (qname == NULL)
        return VAL_NO_ERROR;

    if (VAL_NO_ERROR !=
            (retval = try_chase_query(context, qname, ns_c_in,
                                      ns_t_soa, VAL_FLAGS_DONT_VALIDATE,
                                      queries, &results, done)))
        return retval;

    if (*done) {

        struct val_result_chain *res;
        for (res = results; res; res = res->val_rc_next) {
            int             i;
            if ((res->val_rc_answer == NULL)
                || (res->val_rc_answer->val_ac_rrset == NULL)) {
                if (res->val_rc_proof_count == 0)
                    continue;
                for (i = 0; i < res->val_rc_proof_count; i++) {
                    if (res->val_rc_proofs[i]->val_ac_rrset->
                        val_rrset_type_h == ns_t_soa) {
                        break;
                    }
                }
                if (i == res->val_rc_proof_count)
                    continue;
                soa_rrset = res->val_rc_proofs[i]->val_ac_rrset;
            } else if (res->val_rc_answer->val_ac_rrset->
                       val_rrset_type_h == ns_t_soa) {
                soa_rrset = res->val_rc_answer->val_ac_rrset;
            }
            if (soa_rrset) {
                int len = wire_name_length(soa_rrset->val_rrset_name_n);
                *name_n = (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
                if (*name_n == NULL) {
                    return VAL_OUT_OF_MEMORY;
                }
                memcpy(*name_n, soa_rrset->val_rrset_name_n, len);
                break;
            }
        }
    }

    val_free_result_chain(results);
    return retval;
}


/*
 * find the zonecut for this name and type 
 */
static int
find_next_zonecut(val_context_t * context, struct queries_for_query **queries,
                  struct rrset_rec *rrset, int *done, u_int8_t ** name_n)
{
    u_int8_t       *qname;
    int             retval;
    int             len;

    if (context == NULL || queries == NULL || name_n == NULL || done == NULL)
        return VAL_BAD_ARGUMENT;

    *name_n = NULL;
    qname = NULL;
    *done = 1;

    if (rrset != NULL) {
        qname = rrset->rrs.val_rrset_name_n;

        if ((rrset->rrs.val_rrset_type_h == ns_t_soa)
            || (rrset->rrs.val_rrset_type_h == ns_t_dnskey)) {
            len = wire_name_length(rrset->rrs.val_rrset_name_n);
            *name_n = (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
            if (*name_n == NULL)
                return VAL_OUT_OF_MEMORY;
            memcpy(*name_n, rrset->rrs.val_rrset_name_n, len);
            return VAL_NO_ERROR;
        } else if (rrset->rrs.val_rrset_sig != NULL) {
            /*
             * the signer name is the zone cut 
             */
            u_int8_t       *signby_name_n;
            signby_name_n = &rrset->rrs.val_rrset_sig->rr_rdata[SIGNBY];
            len = wire_name_length(signby_name_n);
            *name_n = (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
            if (*name_n == NULL)
                return VAL_OUT_OF_MEMORY;
            memcpy(*name_n, signby_name_n, len);
            return VAL_NO_ERROR;
        } else if (rrset->rrs.val_rrset_type_h == ns_t_ds) {
            /* advance qname by one label */
            STRIP_LABEL(rrset->rrs.val_rrset_name_n, qname);
        }

        if (rrset->rrs_zonecut_n != NULL) {
            if ((rrset->rrs.val_rrset_type_h == ns_t_ds) &&
                (!namecmp(rrset->rrs_zonecut_n,
                          rrset->rrs.val_rrset_name_n))) {
                /*
                 * for the DS, zonecut cannot be the same as the qname
                 * Obviously some name server returned something bad
                 */
                FREE(rrset->rrs_zonecut_n);
                rrset->rrs_zonecut_n = NULL;
                STRIP_LABEL(rrset->rrs.val_rrset_name_n, qname);
            } else {
                len = wire_name_length(rrset->rrs_zonecut_n);
                *name_n = (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
                if (*name_n == NULL)
                    return VAL_OUT_OF_MEMORY;
                memcpy(*name_n, rrset->rrs_zonecut_n, len);
                return VAL_NO_ERROR;
            }
        }
    }

    if ((VAL_NO_ERROR != (retval = find_next_soa(context, queries, qname, done, name_n))
         || (*done && *name_n == NULL))) {
        return retval;
    }

    if (rrset) {
        if (rrset->rrs_zonecut_n) {
            FREE(rrset->rrs_zonecut_n);
        }
        rrset->rrs_zonecut_n = *name_n; 
        len = wire_name_length(*name_n);
        *name_n = (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
        if (*name_n == NULL) {
            return VAL_OUT_OF_MEMORY;
        }
        memcpy(*name_n, rrset->rrs_zonecut_n, len);
    }

    return VAL_NO_ERROR;
}

#if 0
static int
prove_existence(val_context_t * context,
                u_int8_t * qname_n,
                u_int16_t qtype_h,
                u_int8_t * soa_name_n,
                struct val_internal_result *w_results,
                struct queries_for_query **queries,
                struct val_result_chain **proof_res,
                struct val_result_chain **results, val_status_t * status,
                u_int32_t *ttl_x)
{
    struct val_internal_result *res;
    int             nsec_bit_field;
#ifdef LIBVAL_NSEC3
    u_int8_t        nsec3_hashlen;
    val_nsec3_rdata_t nd;
    u_int8_t        hashlen;
    u_int8_t       *hash;
    u_int8_t       *cp = NULL;
    u_int8_t       *nsec3_hash = NULL;
#endif
    int             retval;

    for (res = w_results; res; res = res->val_rc_next) {
        if (!res->val_rc_is_proof)
            continue;

        struct rrset_rec *the_set = res->val_rc_rrset->_as.ac_data;
        if ((!the_set) || (!the_set->rrs.val_rrset_data)) {
            continue;
        }

        if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC) {

            if (!namecmp(the_set->rrs.val_rrset_name_n, qname_n)) {
                /*
                 * NSEC owner = query name & q_type not in list 
                 */
                nsec_bit_field =
                    wire_name_length(the_set->rrs.val_rrset_data->
                                     rr_rdata);
                if (is_type_set
                    ((&
                      (the_set->rrs.val_rrset_data->
                       rr_rdata[nsec_bit_field])),
                     the_set->rrs.val_rrset_data->rr_rdata_length_h -
                     nsec_bit_field, qtype_h)) {
                    val_log(context, LOG_DEBUG,
                            "Wildcard expansion: Type exists at NSEC record");
                    *status = VAL_SUCCESS;
                    break;
                }
            }
        }
#ifdef LIBVAL_NSEC3
        else if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3) {

            nsec3_hashlen = the_set->rrs.val_rrset_name_n[0];
            nsec3_hash =
                (nsec3_hashlen ==
                 0) ? NULL : the_set->rrs.val_rrset_name_n + 1;

            if (NULL ==
                val_parse_nsec3_rdata(the_set->rrs.val_rrset_data->
                                      rr_rdata,
                                      the_set->rrs.val_rrset_data->
                                      rr_rdata_length_h, &nd)) {
                val_log(context, LOG_DEBUG, "Cannot parse NSEC3 rdata");
                *status = VAL_BOGUS_PROOF;
                return VAL_NO_ERROR;
            }

            /*
             * hash name according to nsec3 parameters 
             */
            if (NULL ==
                compute_nsec3_hash(context, cp, soa_name_n, nd.alg,
                                   nd.iterations, nd.saltlen, nd.salt,
                                   &hashlen, &hash, ttl_x)) {
                val_log(context, LOG_DEBUG,
                        "Cannot compute NSEC3 hash with given params");
                *status = VAL_BOGUS_PROOF;
                FREE(nd.nexthash);
                return VAL_NO_ERROR;
            }

            /*
             * Check if there is an exact match 
             */
            if ((nsec3_hashlen == hashlen)
                && !memcmp(hash, nsec3_hash, hashlen)) {
                int             nsec3_bm_len =
                    the_set->rrs.val_rrset_data->rr_rdata_length_h -
                    nd.bit_field;

                if (is_type_set
                    ((&(the_set->rrs.val_rrset_data->
                        rr_rdata[nd.bit_field])), nsec3_bm_len, qtype_h)) {
                    val_log(context, LOG_DEBUG,
                            "Wildcard expansion: Type exists at NSEC3 record");
                    *status = VAL_SUCCESS;
                    FREE(nd.nexthash);
                    FREE(hash);
                    break;
                }
            }
        }
#endif
    }

    if (res) {

        struct val_result_chain *new_res;
        /*
         * This proof is relevant 
         */
        if (VAL_NO_ERROR != (retval = transform_single_result(context, res, queries,
                                                              results,
                                                              *proof_res,
                                                              &new_res))) {
            goto err;
        }
        *proof_res = new_res;
        (*proof_res)->val_rc_status = VAL_SUCCESS;
        return VAL_NO_ERROR;
    }

    *status = VAL_BOGUS_PROOF;
    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    *proof_res = NULL;
    return retval;
}
#endif

/*
 * This function does the provably unsecure check in a
 * top-down fashion
 */
static int
verify_provably_unsecure(val_context_t * context,
                         struct queries_for_query **queries,
                         struct val_digested_auth_chain *as,
                         int *done,
                         int *is_punsecure)
{
    struct val_result_chain *results = NULL;
    char            name_p[NS_MAXDNAME];
    char            tempname_p[NS_MAXDNAME];

    u_int8_t       *curzone_n = NULL;
    u_int8_t       *q_zonecut_n = NULL;
    u_int8_t       *zonecut_n = NULL;
    u_int8_t       *qname = NULL;
    u_int32_t      ttl_x = 0;

    struct rrset_rec *rrset;
    int             retval;
    u_int8_t flags;
    struct val_query_chain *top_q = NULL;

    if ((NULL == as) || (NULL == as->_as.ac_data) || 
        (as->val_ac_query == NULL) || (queries == NULL) || 
        (done == NULL) || (is_punsecure == NULL)) {

        return VAL_BAD_ARGUMENT;
    }

    top_q = as->val_ac_query;
    rrset = as->_as.ac_data;
    *done = 1;
    flags = top_q->qc_flags;
    retval = VAL_NO_ERROR;
    
    if (-1 == ns_name_ntop(rrset->rrs.val_rrset_name_n, name_p, 
                sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
       
    if (VAL_NO_ERROR != (find_trust_point(context, rrset->rrs.val_rrset_name_n,
                                          &curzone_n, &ttl_x))) {
        val_log(context, LOG_DEBUG, "Cannot find trust anchor for %s", name_p);
        goto err;
    }
    SET_MIN_TTL(top_q->qc_ttl_x, ttl_x);
    
    /* find the zonecut for the query */
    if ((curzone_n == NULL) ||
        (VAL_NO_ERROR != find_next_zonecut(context, queries, rrset, done, &q_zonecut_n))
                || (*done && q_zonecut_n == NULL)) {

        val_log(context, LOG_DEBUG, "Cannot find zone cut for %s", name_p);
        goto err;
    }

    if (*done == 0) {
        /* Need more data */
        *is_punsecure = 0;
        goto donefornow;
    }

    qname = NULL;
    /* while  we've not reached the zonecut for the query */
    while(namecmp(q_zonecut_n, curzone_n)) {

        /* Add another label to curzone_n */
        ADD_LABEL(q_zonecut_n, curzone_n, qname);

        /* find next zone cut going down from the trust anchor */
        if ((VAL_NO_ERROR !=
                find_next_soa(context, queries, qname, done, &zonecut_n))
                || (*done && zonecut_n == NULL)) {

            if ((curzone_n == NULL) ||
                    (-1 == ns_name_ntop(curzone_n, tempname_p, sizeof(tempname_p)))) {
                snprintf(tempname_p, sizeof(tempname_p), "unknown/error");
            } 

            val_log(context, LOG_DEBUG, "Cannot find zone cut for %s", tempname_p);
            goto err;
        }

        if (*done == 0) {
            /* Need more data */
            *is_punsecure = 0;
            goto donefornow;
        }

        /* if the zonecut is same as before, try again */
        if (!namecmp(zonecut_n,  curzone_n)) {
            FREE(zonecut_n);
            zonecut_n = NULL;
            continue;
        }

        /* try validating the DS */
        if (VAL_NO_ERROR != (retval = 
                    try_chase_query(context, zonecut_n, ns_c_in, 
                                    ns_t_ds, flags, queries, &results, done)))
             goto err;

        if (*done == 0) {
            /* Need more data */
            *is_punsecure = 0;
            goto donefornow;
        }

        /* if done,  inspect the results */
        if (results == NULL) {
            goto err;
        }

        /* If result is not trustworthy, not provably unsecure */
        if (!val_isvalidated(results->val_rc_status)) {
            goto err; 
        }

        /* if non-existent set as provably unsecure and break */
        if ((results->val_rc_status == VAL_NONEXISTENT_TYPE) ||
            (results->val_rc_status == VAL_NONEXISTENT_TYPE_NOCHAIN)) {
            val_log(context, LOG_DEBUG, "%s is provably unsecure", name_p);
            *is_punsecure = 1;
            goto donefornow;
        }
#ifdef LIBVAL_NSEC3
        else if (results->val_rc_status == VAL_NONEXISTENT_NAME_OPTOUT) {
            val_log(context, LOG_DEBUG, "%s is optout provably unsecure", name_p);
            *is_punsecure = 1;
            goto donefornow;
        }
#endif

        /* validated DS; look for next zonecut */ 
        if (curzone_n) {
            FREE(curzone_n);
        }
        curzone_n = zonecut_n;
        zonecut_n = NULL;
    }

err:
    val_log(context, LOG_DEBUG,
            "Cannot show that %s is provably unsecure.", name_p);
    *is_punsecure = 0;

donefornow:
    if (q_zonecut_n)
        FREE(q_zonecut_n);
    if (zonecut_n)
        FREE(zonecut_n);
    if (curzone_n)
        FREE(curzone_n);
    if (results != NULL) {
        val_free_result_chain(results);
        results = NULL;
    }
    return retval;
}

static int
is_pu_trusted(val_context_t *ctx, u_int8_t *name_n, u_int32_t *ttl_x)
{
    policy_entry_t *pu_pol, *pu_cur;
    u_int8_t       *p;
    char            name_p[NS_MAXDNAME];
    int             name_len;

    RETRIEVE_POLICY(ctx, P_PROV_UNSECURE, pu_pol);
    if (pu_pol) {

        name_len = wire_name_length(name_n);
        
        for (pu_cur = pu_pol;
             pu_cur && (wire_name_length(pu_cur->zone_n) > name_len);
             pu_cur = pu_cur->next);

        /*
         * for all zones which are shorter or as long, do a strstr 
         */
        /*
         * Because of the ordering, the longest match is found first 
         */
        for (; pu_cur; pu_cur = pu_cur->next) {
            int             root_zone = 0;
            if (!namecmp(pu_cur->zone_n, (const u_int8_t *) ""))
                root_zone = 1;
            else {
                /*
                 * Find the last occurrence of zse_cur->zone_n in name_n 
                 */
                p = name_n;
                while (p && (*p != '\0')) {
                    if (!namecmp(p, pu_cur->zone_n))
                        break;
                    p = p + *p + 1;
                }
            }

            if ((root_zone || (!namecmp(p, pu_cur->zone_n))) && pu_cur->pol) {
                struct prov_unsecure_policy *pol =
                    (struct prov_unsecure_policy *)(pu_cur->pol);
                if (-1 == ns_name_ntop(name_n, name_p, sizeof(name_p)))
                    snprintf(name_p, sizeof(name_p), "unknown/error");
                if (pu_cur->exp_ttl > 0)
                    *ttl_x = pu_cur->exp_ttl;

                if (pol->trusted == ZONE_PU_UNTRUSTED) {
                    val_log(ctx, LOG_DEBUG, "zone %s provable unsecure status is not trusted",
                            name_p);
                    return 0;
                } else { 
                    val_log(ctx, LOG_DEBUG, "zone %s provably unsecure status is trusted", name_p);
                    return 1;
                }
            }
        }
    }
    return 1; /* trust provably unsecure state by default */
}

/*
 * Verify an assertion if possible. Complete assertions are those for which 
 * you have data, rrsigs and key information. 
 * Returns:
 * VAL_NO_ERROR                 Operation completed successfully
 * Other return values from add_to_query_chain()
 */
static int
try_verify_assertion(val_context_t * context, 
                     struct queries_for_query **queries,
                     struct val_digested_auth_chain *next_as)
{
    int             retval;
    struct rrset_rec *pending_rrset;
    struct queries_for_query *pc = NULL;

    /*
     * Sanity check 
     */
    if (NULL == context || NULL == queries)
        return VAL_BAD_ARGUMENT;

    if (next_as == NULL)
        return VAL_NO_ERROR;

    if (next_as->val_ac_status == VAL_AC_WAIT_FOR_RRSIG) {

        /* find the pending query */
        if (VAL_NO_ERROR != (retval = add_to_qfq_chain(context,
                                                       queries,
                                                       next_as->_as.ac_data->
                                                       rrs.
                                                       val_rrset_name_n,
                                                       ns_t_rrsig,
                                                       next_as->_as.ac_data->
                                                       rrs.
                                                       val_rrset_class_h,
                                                       next_as->val_ac_query->qc_flags,
                                                       &pc)))
                return retval;

        if (pc->qfq_query->qc_state > Q_ERROR_BASE) {
            next_as->val_ac_status = VAL_AC_RRSIG_MISSING;
            return VAL_NO_ERROR;
        }
        else if (pc->qfq_query->qc_state < Q_ANSWERED)
            return VAL_NO_ERROR; 
            
        if (next_as->_as.ac_data == NULL) {
            /*
             * if no data exists, why are we waiting for an RRSIG again? 
             */
            next_as->val_ac_status = VAL_AC_DATA_MISSING;
            return VAL_NO_ERROR;
        } else {
            struct val_digested_auth_chain *pending_as;
            for (pending_as = pc->qfq_query->qc_ans; pending_as;
                 pending_as = pending_as->_as.val_ac_rrset_next) {
                /*
                 * We were waiting for the RRSIG 
                 */
                pending_rrset = pending_as->_as.ac_data;
                if ((pending_rrset == NULL) ||
                    (pending_rrset->rrs.val_rrset_sig == NULL) ||
                    (pending_rrset->rrs.val_rrset_sig->rr_rdata == NULL)) {
                        continue;
                }

                /*
                 * Check if what we got was an RRSIG 
                 */
                if (pending_as->val_ac_status == VAL_AC_BARE_RRSIG) {
                    /*
                     * Find the RRSIG that matches the type 
                     * Check if type is in the RRSIG 
                     */
                    u_int16_t       rrsig_type_n;
                    memcpy(&rrsig_type_n,
                           pending_rrset->rrs.val_rrset_sig->rr_rdata,
                           sizeof(u_int16_t));
                    if (next_as->_as.ac_data->rrs.val_rrset_type_h ==
                        ntohs(rrsig_type_n)) {
                        /*
                         * store the RRSIG in the assertion 
                         */
                        next_as->_as.ac_data->rrs.val_rrset_sig =
                            copy_rr_rec_list(pending_rrset->rrs.
                                             val_rrset_type_h,
                                             pending_rrset->rrs.
                                             val_rrset_sig, 0);
                        next_as->val_ac_status = VAL_AC_WAIT_FOR_TRUST;
                        /*
                         * create a pending query for the trust portion 
                         */
                        if (VAL_NO_ERROR !=
                            (retval =
                             build_pending_query(context, queries, next_as, &pc)))
                            return retval;
                        break;
                    }
                }
            }
            if (pending_as == NULL) {
                /*
                 * Could not find any RRSIG matching query type
                 */
                next_as->val_ac_status = VAL_AC_RRSIG_MISSING;
                return VAL_NO_ERROR;
            }
        }
    } else if (next_as->val_ac_status == VAL_AC_WAIT_FOR_TRUST) {

        if (next_as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey) {
            if (next_as->_as.ac_data->rrs_zonecut_n == NULL) {
                next_as->val_ac_status = VAL_AC_DS_MISSING;
                return VAL_NO_ERROR;
            }

            if (VAL_NO_ERROR !=
                (retval =
                    add_to_qfq_chain(context, queries, 
                          next_as->_as.ac_data->rrs_zonecut_n, ns_t_ds,
                          next_as->_as.ac_data->rrs.val_rrset_class_h, 
                          next_as->val_ac_query->qc_flags, &pc)))
                return retval;

            if (pc->qfq_query->qc_state > Q_ERROR_BASE) {
                next_as->val_ac_status = VAL_AC_DS_MISSING;
                return VAL_NO_ERROR; 
            }
            else if (pc->qfq_query->qc_state < Q_ANSWERED)
                return VAL_NO_ERROR; 
            
        } else {
            if (next_as->_as.ac_data->rrs_zonecut_n == NULL) {
                next_as->val_ac_status = VAL_AC_DNSKEY_MISSING;
                return VAL_NO_ERROR;
            }
            if (VAL_NO_ERROR !=
                (retval =
                    add_to_qfq_chain(context, queries, 
                          next_as->_as.ac_data->rrs_zonecut_n, ns_t_dnskey,
                          next_as->_as.ac_data->rrs.val_rrset_class_h, 
                          next_as->val_ac_query->qc_flags, &pc)))
                return retval;

            if (pc->qfq_query->qc_state > Q_ERROR_BASE) {
                next_as->val_ac_status = VAL_AC_DNSKEY_MISSING;
                return VAL_NO_ERROR;
            }
            else if (pc->qfq_query->qc_state < Q_ANSWERED)
                return VAL_NO_ERROR; 
        }
        
        if ((pc->qfq_query->qc_ans) && 
            (pc->qfq_query->qc_ans->_as.ac_data) && 
            (pc->qfq_query->qc_ans->_as.ac_data->rrs_ans_kind == SR_ANS_STRAIGHT)) {
            /*
             * if the pending assertion contains a straight answer, 
             * trust is useful for verification 
             */
            next_as->val_ac_status = VAL_AC_CAN_VERIFY;

        } else if (pc->qfq_query->qc_proof) {
            /*
             * proof of non-existence should follow 
             */
            next_as->val_ac_status = VAL_AC_NEGATIVE_PROOF;
            return VAL_NO_ERROR;

        } else {
            if (pc->qfq_query->qc_type_h == ns_t_ds)
                next_as->val_ac_status = VAL_AC_DS_MISSING;
            else if (pc->qfq_query->qc_type_h == ns_t_dnskey)
                next_as->val_ac_status = VAL_AC_DNSKEY_MISSING;
            return VAL_NO_ERROR;
        }
    }

    if (next_as->val_ac_status == VAL_AC_CAN_VERIFY) {
        struct val_digested_auth_chain *the_trust;

        val_log(context, LOG_DEBUG, "verifying next assertion");
        the_trust = get_ac_trust(context, next_as, queries); 
        verify_next_assertion(context, next_as, the_trust);
        /* 
         * Set the TTL to the minimum of the authentication 
         * chain element and the trust element
         */
        if (the_trust && the_trust->_as.ac_data) {
            if (the_trust->_as.ac_data->rrs.val_rrset_ttl_x <
                    next_as->_as.ac_data->rrs.val_rrset_ttl_x) {
                next_as->_as.ac_data->rrs.val_rrset_ttl_x =
                    the_trust->_as.ac_data->rrs.val_rrset_ttl_x;

                SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x,
                            the_trust->_as.ac_data->rrs.val_rrset_ttl_x);
            }
        }
    }

    return VAL_NO_ERROR;
}


/*
 * Try and verify each assertion. Update results as and when they are available.
 * Do not try and validate assertions that have already been validated.
 */
static int
verify_and_validate(val_context_t * context,
                    struct queries_for_query **queries,
                    struct val_query_chain *top_q, int is_proof,
                    struct val_internal_result **results,
                    int *done)
{
    struct val_digested_auth_chain *next_as;
    struct val_digested_auth_chain *as_trust;
    int             retval;
    struct val_digested_auth_chain *as_more;
    struct val_digested_auth_chain *top_as;
    struct val_internal_result *res;
    struct val_internal_result *cur_res, *tail_res, *temp_res;
    struct queries_for_query *added_q = NULL;
    u_int32_t ttl_x = 0;

    if ((top_q == NULL) || (NULL == queries) || (NULL == results)
        || (NULL == done))
        return VAL_BAD_ARGUMENT;

    *done = 1;

    if (is_proof) {
        top_as = top_q->qc_proof;
    } else {
        top_as = top_q->qc_ans;
    }

    for (tail_res = *results;
         tail_res && tail_res->val_rc_next;
         tail_res = tail_res->val_rc_next);

    /*
     * Look at every answer that was returned 
     */
    for (as_more = top_as; as_more;
         as_more = as_more->_as.val_ac_rrset_next) {
        int             thisdone = 1;

        /*
         * If this assertion is already in the results list with a completed status
         * no need for repeating the validation process
         */
        for (res = *results; res; res = res->val_rc_next) {
            if (res->val_rc_rrset == as_more)
                break;
        }
        if (res) {
            if (!CHECK_MASKED_STATUS(res->val_rc_status, VAL_DONT_KNOW))
                /*
                 * we've already dealt with this one 
                 */
                continue;
        } else {
            /*
             * Add this result to the list 
             */
            res = (struct val_internal_result *)
                MALLOC(sizeof(struct val_internal_result));
            if (res == NULL) {
                /*
                 * free the result list 
                 */
                cur_res = *results;
                while (cur_res) {
                    temp_res = cur_res->val_rc_next;
                    FREE(cur_res);
                    cur_res = temp_res;
                }
                *results = NULL;
                return VAL_OUT_OF_MEMORY;
            }
            res->val_rc_is_proof = is_proof;
            res->val_rc_consumed = 0;
            res->val_rc_rrset = as_more;
            res->val_rc_next = NULL;
            res->val_rc_status = VAL_DONT_KNOW;

            if (top_q->qc_flags & VAL_FLAGS_DONT_VALIDATE) {
                res->val_rc_status = VAL_IGNORE_VALIDATION;
            }

            if (tail_res)
                tail_res->val_rc_next = res;
            else {
                *results = res;
            }
            tail_res = res;
        }

        /*
         * as_more is the next answer that we obtained; next_as is the 
         * next assertion in the chain of trust
         */
        next_as = as_more;
        while (next_as) {

            if (next_as->val_ac_status <= VAL_AC_INIT) {
                /*
                 * Go up the chain of trust 
                 */
                if (VAL_NO_ERROR != (retval = 
                            try_verify_assertion(context, queries,
                                              next_as)))
                    return retval;
            }

            as_trust = get_ac_trust(context, next_as, queries); 

            /*
             * break out of infinite loop -- trying to verify the proof of non-existence
             * for a DS record; but the DNSKEY that signs the proof is also in the 
             * chain of trust (not-validated)
             * also the case where trust for an SOA is returned as another SOA
             */
            if ((next_as->_as.ac_data != NULL) &&
                (next_as == as_trust)) {
                next_as->val_ac_status = VAL_AC_DNSKEY_MISSING;
                break;
            }

            /*
             * Check states 
             */
            if (next_as->val_ac_status <= VAL_AC_INIT) {
                /*
                 * still need more data to validate this assertion 
                 */
                *done = 0;
                thisdone = 0;
            } else if (next_as->val_ac_status == VAL_AC_NEGATIVE_PROOF) {
                /*
                 * This means that the trust point has a proof of non-existence 
                 */

                if (as_trust == NULL) {
                    res->val_rc_status = VAL_ERROR;
                    break;
                }

                /*
                 * We may have asked the child zone for the DS;
                 * This can only happen if the current member in
                 * the chain of trust is the DNSKEY record
                 */
                if (next_as->val_ac_rrset->val_rrset_type_h == ns_t_dnskey) {

                    int             asked_the_parent = 0;
                    struct val_digested_auth_chain *as;

                    /*
                     * Check if the name in the soa record is the same as the
                     * owner name of the DS record
                     */
                    for (as = as_trust; as;
                         as = as->_as.val_ac_rrset_next) {
                        if ((as->val_ac_rrset != NULL)
                            && (as->val_ac_rrset->val_rrset_type_h ==
                                ns_t_soa)) {
                            if (namecmp
                                (as->val_ac_rrset->val_rrset_name_n,
                                 next_as->val_ac_rrset->val_rrset_name_n))
                                asked_the_parent = 1;
                            break;
                        }
                    }

                    if (asked_the_parent) {
                        int is_punsecure;
                        if (VAL_NO_ERROR != 
                                (retval = verify_provably_unsecure(context, 
                                                                   queries, 
                                                                   next_as, 
                                                                   &thisdone,
                                                                   &is_punsecure)))
                            return retval;

                        if (thisdone) {
                            if (is_punsecure) {
                                next_as->val_ac_status = VAL_AC_PROVABLY_UNSECURE;
                                if (is_pu_trusted(context, 
                                        next_as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                                    res->val_rc_status = VAL_PROVABLY_UNSECURE;
                                else
                                    res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                                SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x, ttl_x);
                            } else {
                                res->val_rc_status = VAL_BOGUS_PROOF;
                            }
                            break;
                        } else {
                            *done = 0;
                        }
                    } 
                    /*
                     * We could only be asking the child if our default name server is 
                     * the child, so ty again starting from root; state will be WAIT_FOR_TRUST 
                     */
                    else if (context->root_ns == NULL) {
                        /*
                         * No root hints configured 
                         */
                        res->val_rc_status = VAL_BOGUS_PROOF;
                        val_log(context, LOG_WARNING, 
                                "response for DS from child; VAL_BOGUS_PROOF: no root.hints configured");
                        break;
                    } else {
                        /*
                         * send query to root 
                         */
                        next_as->val_ac_status = VAL_AC_WAIT_FOR_TRUST;
                        if (VAL_NO_ERROR !=
                            (retval =
                             build_pending_query(context, queries, next_as, &added_q)))
                            return retval;
                        if (added_q->qfq_query->qc_referral != NULL) {
                            /*
                             * If some nameserver actually sends a referral for the DS record
                             * to the child (faulty/malicious NS) we'll keep recursing from root
                             */
                            res->val_rc_status = VAL_BOGUS_PROOF;
                            break;
                        }
                        clone_ns_list(&added_q->qfq_query->qc_ns_list,
                                      context->root_ns);
                        *done = 0;
                        thisdone = 0;
                    }
                } else {
                    int is_punsecure;
                    if (VAL_NO_ERROR != 
                                (retval = verify_provably_unsecure(context, 
                                                                   queries, 
                                                                   next_as, 
                                                                   &thisdone,
                                                                   &is_punsecure)))
                        return retval;

                    if (thisdone) {
                        if (is_punsecure) {
                            next_as->val_ac_status = VAL_AC_PROVABLY_UNSECURE;
                            if (is_pu_trusted(context, 
                                    next_as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                                res->val_rc_status = VAL_PROVABLY_UNSECURE;
                            else
                                res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                            SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x, ttl_x);
                        } else {
                            res->val_rc_status = VAL_BOGUS_PROOF;
                        }
                        break;
                    } else {
                        *done = 0;
                    }
                }
            } else if (next_as->val_ac_status <= VAL_AC_LAST_STATE) {

                /*
                 * Check if success 
                 */
                if (res->val_rc_status == VAL_DONT_KNOW) {

                    if (next_as->val_ac_status == VAL_AC_IGNORE_VALIDATION) {
                        res->val_rc_status = VAL_IGNORE_VALIDATION;
                    } else if (next_as->val_ac_status ==
                               VAL_AC_TRUSTED_ZONE) {
                        res->val_rc_status = VAL_TRUSTED_ZONE;
                    } else if (next_as->val_ac_status == VAL_AC_TRUST_KEY) {
                        SET_CHAIN_COMPLETE(res->val_rc_status);
                    } else if (next_as->val_ac_status ==
                               VAL_AC_UNTRUSTED_ZONE) {
                        res->val_rc_status = VAL_UNTRUSTED_ZONE;
                    } else if (next_as->val_ac_status ==
                               VAL_AC_LOCAL_ANSWER) {
                        res->val_rc_status = VAL_LOCAL_ANSWER;
                    } else if (next_as->val_ac_status ==
                               VAL_AC_PROVABLY_UNSECURE) {
                        if (is_pu_trusted(context, 
                                    next_as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                            res->val_rc_status = VAL_PROVABLY_UNSECURE;
                        else
                            res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                        SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x, ttl_x);
                    } else if (next_as->val_ac_status == VAL_AC_BARE_RRSIG) {
                        res->val_rc_status = VAL_BARE_RRSIG;
                    } else if (next_as->val_ac_status ==
                               VAL_AC_NO_TRUST_ANCHOR) {
                        /*
                         * verified but no trust 
                         */
                        res->val_rc_status = VAL_VERIFIED_CHAIN;
                    }
                } else {

                    /*
                     * Reached a trust point but there was some error in between 
                     */
                    SET_CHAIN_COMPLETE(res->val_rc_status);
                }

                break;
            }

            /*
             * Check error conditions 
             */
            else if (next_as->val_ac_status <= VAL_AC_LAST_ERROR) {
                int is_punsecure;
                if (VAL_NO_ERROR != (retval = verify_provably_unsecure(context, 
                                                                   queries, 
                                                                   next_as, 
                                                                   &thisdone,
                                                                   &is_punsecure)))
                    return retval;

                if (thisdone) {
                    if (is_punsecure) {
                        next_as->val_ac_status = VAL_AC_PROVABLY_UNSECURE;
                        if (is_pu_trusted(context, 
                                next_as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                            res->val_rc_status = VAL_PROVABLY_UNSECURE;
                        else
                            res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                        SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x, ttl_x);
                    } else {
                        res->val_rc_status = VAL_BOGUS_UNPROVABLE;
                    }
                    break;
                } else {
                    *done = 0;
                }
            } else if (next_as->val_ac_status <= VAL_AC_LAST_BAD) {
                res->val_rc_status = VAL_ERROR;
                break;
            } else if (next_as->val_ac_status <= VAL_AC_LAST_FAILURE) {
                /*
                 * double failures are unprovable 
                 */
                if (CHECK_MASKED_STATUS
                    (res->val_rc_status, VAL_BOGUS_UNPROVABLE)) {

                    int is_punsecure;
                    if (VAL_NO_ERROR != (retval = verify_provably_unsecure(context, 
                                                                   queries, 
                                                                   next_as, 
                                                                   &thisdone,
                                                                   &is_punsecure)))
                        return retval;

                    if (thisdone) {
                        if (is_punsecure) {
                            next_as->val_ac_status = VAL_AC_PROVABLY_UNSECURE;
                            if (is_pu_trusted(context, 
                                            next_as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                                res->val_rc_status = VAL_PROVABLY_UNSECURE;
                            else
                                res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                            SET_MIN_TTL(next_as->val_ac_query->qc_ttl_x, ttl_x);
                        } else {
                            res->val_rc_status = VAL_BOGUS_UNPROVABLE;
                        }
                        break;
                    } else {
                        *done = 0;
                    }
                } else {
                    SET_MASKED_STATUS(res->val_rc_status,
                                      VAL_BOGUS_UNPROVABLE);
                }
            }
            next_as = as_trust; 
        }
        if (!thisdone) {
            /*
             * more work required 
             */
            SET_MASKED_STATUS(res->val_rc_status, VAL_DONT_KNOW);
        }
    }

    return VAL_NO_ERROR;
}


static int
ask_cache(val_context_t * context, 
          struct queries_for_query **queries,
          int *data_received,
          int *data_missing)
{
    struct queries_for_query *next_q, *top_q;
    int    retval;
    char   name_p[NS_MAXDNAME];
    struct domain_info *response = NULL;
    int more_data = 0;

    if (context == NULL || queries == NULL || data_received == NULL || data_missing == NULL)
        return VAL_BAD_ARGUMENT;

    if (*data_missing == 0)
        return VAL_NO_ERROR;
    
    top_q = *queries;

    *data_missing = 0;
    for (next_q = *queries; next_q; next_q = next_q->qfq_next) {
        if (next_q->qfq_query->qc_state < Q_ANSWERED) {
            *data_missing = 1;
        }
        if (next_q->qfq_query->qc_state == Q_INIT) {

            if (-1 == ns_name_ntop(next_q->qfq_query->qc_name_n, name_p, sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");

            val_log(context, LOG_DEBUG,
                    "ask_cache(): looking for {%s %s(%d) %s(%d)}", name_p,
                    p_class(next_q->qfq_query->qc_class_h), next_q->qfq_query->qc_class_h,
                    p_type(next_q->qfq_query->qc_type_h), next_q->qfq_query->qc_type_h);

            if (VAL_NO_ERROR !=
                (retval = get_cached_rrset(next_q->qfq_query, &response)))
                return retval;

            if (response) {
                val_log(context, LOG_DEBUG,
                        "ask_cache(): found data for {%s %d %d}", name_p,
                        next_q->qfq_query->qc_class_h, next_q->qfq_query->qc_type_h);

                if (next_q->qfq_query->qc_state == Q_ANSWERED) {

                    /* merge any answer from the referral (alias) portion */

                    if (next_q->qfq_query->qc_referral) {
                        merge_rrset_recs(&next_q->qfq_query->qc_referral->answers, response->di_answers);
                        response->di_answers = next_q->qfq_query->qc_referral->answers;
                        next_q->qfq_query->qc_referral->answers = NULL;

                        /*
                         * Consume qnames
                         */
                        if (response->di_qnames == NULL)
                            response->di_qnames = next_q->qfq_query->qc_referral->qnames;
                        else if (next_q->qfq_query->qc_referral->qnames) {
                            struct qname_chain *t_q;
                            for (t_q = response->di_qnames; t_q->qnc_next; t_q = t_q->qnc_next);
                            t_q->qnc_next = next_q->qfq_query->qc_referral->qnames;
                        }
                        next_q->qfq_query->qc_referral->qnames = NULL;
        
                        /*
                         * Note that we don't free qc_referral here 
                         */
                        free_referral_members(next_q->qfq_query->qc_referral);
                    }

                    if (VAL_NO_ERROR != (retval = assimilate_answers(context, queries,
                                                response, next_q->qfq_query))) {

                        free_domain_info_ptrs(response);
                        FREE(response);
                        return retval;
                    }

                } else {
                    /* got some response, but need to get more info (cname/dname) */
                    more_data = 1;
                    *data_missing = 1;

                    if (next_q->qfq_query->qc_referral == NULL) {
                        ALLOCATE_REFERRAL_BLOCK(next_q->qfq_query->qc_referral);
                    }
                    /*
                     * Consume qnames 
                     */
                    if (next_q->qfq_query->qc_referral->qnames == NULL)
                        next_q->qfq_query->qc_referral->qnames = response->di_qnames;
                    else if (response->di_qnames) {
                        struct qname_chain *t_q;
                        for (t_q = response->di_qnames; t_q->qnc_next; t_q = t_q->qnc_next);
                        t_q->qnc_next = next_q->qfq_query->qc_referral->qnames;
                        next_q->qfq_query->qc_referral->qnames = response->di_qnames;
                    }
                    response->di_qnames = NULL;
                }

                free_domain_info_ptrs(response);
                FREE(response);
            }

            if (next_q->qfq_query->qc_state > Q_SENT) 
                *data_received = 1;
        }
    }

    if ((top_q != *queries) || more_data)
        /*
         * more queries have been added, do this again 
         */
        return ask_cache(context, queries, data_received, data_missing);


    return VAL_NO_ERROR;
}

static int
ask_resolver(val_context_t * context, 
             struct queries_for_query **queries,
             int *data_received,
             int *data_missing)
             
{
    struct queries_for_query *next_q;
    struct domain_info *response;
    int             retval;
    int             need_data = 0;
    char            name_p[NS_MAXDNAME];
    u_int16_t       tzonestatus;
    u_int32_t ttl_x = 0;

    if ((context == NULL) || (queries == NULL) || (data_received == NULL) || (data_missing == NULL)) 
        return VAL_BAD_ARGUMENT;

    if (*data_missing == 0)
        return VAL_NO_ERROR;

    response = NULL;

    for (next_q = *queries; next_q; next_q = next_q->qfq_next) {
        if (next_q->qfq_query->qc_state == Q_INIT) {
            u_int8_t       *test_n;
            struct name_server *ns;

            need_data = 1;
            if (-1 ==
                ns_name_ntop(next_q->qfq_query->qc_name_n, name_p,
                             sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");
            val_log(context, LOG_DEBUG,
                    "ask_resolver(): sending query for {%s %d %d}",
                    name_p, next_q->qfq_query->qc_class_h, next_q->qfq_query->qc_type_h);

            if (next_q->qfq_query->qc_ns_list == NULL) {
                if (VAL_NO_ERROR != (retval =
                                     find_nslist_for_query(context,
                                                           next_q->qfq_query,
                                                           queries))) {
                    return retval;
                }
            }

            /*
             * Only set the CD and EDS0 options if we feel the server 
             * is capable of handling DNSSEC
             */
            if (next_q->qfq_query->qc_zonecut_n != NULL)
                test_n = next_q->qfq_query->qc_zonecut_n;
            else
                test_n = next_q->qfq_query->qc_name_n;

            if (next_q->qfq_query->qc_ns_list &&
                !(next_q->qfq_query->qc_ns_list->ns_options & RES_USE_DNSSEC)) {
                if (!(next_q->qfq_query->qc_flags & VAL_FLAGS_DONT_VALIDATE)) {
                    
                    if (VAL_NO_ERROR != (retval = 
                        get_zse(context, test_n, &tzonestatus, &ttl_x))) {
                        return retval;
                    }
                    SET_MIN_TTL(next_q->qfq_query->qc_ttl_x, ttl_x);

                    if (tzonestatus == VAL_AC_WAIT_FOR_TRUST) {
                        val_log(context, LOG_DEBUG,
                            "Setting D0 bit and using EDNS0");

                        for (ns = next_q->qfq_query->qc_ns_list; ns; ns = ns->ns_next)
                            ns->ns_options |= RES_USE_DNSSEC;
                    }

                } else {
                    val_log(context, LOG_DEBUG,
                            "Not setting D0 bit nor using EDNS0");
                }
            }

            /* find_nslist_for_query() could have modified the state */ 
            if (next_q->qfq_query->qc_state == Q_INIT) {
                if ((retval =
                     val_resquery_send(context, next_q->qfq_query)) != VAL_NO_ERROR)
                    return retval;
                next_q->qfq_query->qc_state = Q_SENT;
            }
        } else if (next_q->qfq_query->qc_state < Q_ANSWERED)
            need_data = 1;
    }

    if (need_data) {
        for (next_q = *queries; next_q; next_q = next_q->qfq_next) {
            if (next_q->qfq_query->qc_state == Q_SENT) {
                if ((retval =
                     val_resquery_rcv(context, next_q->qfq_query, &response,
                                      queries)) != VAL_NO_ERROR)
                    return retval;

                if ((next_q->qfq_query->qc_state == Q_ANSWERED)
                    && (response != NULL)) {
                    if (-1 ==
                        ns_name_ntop(next_q->qfq_query->qc_name_n, name_p,
                                     sizeof(name_p)))
                        snprintf(name_p, sizeof(name_p),
                                 "unknown/error");
                    val_log(context, LOG_DEBUG,
                            "ask_resolver(): found data for {%s %d %d}",
                            name_p, next_q->qfq_query->qc_class_h,
                            next_q->qfq_query->qc_type_h);
                    if (VAL_NO_ERROR !=
                        (retval =
                         assimilate_answers(context, queries, response,
                                            next_q->qfq_query))) {
                        free_domain_info_ptrs(response);
                        FREE(response);
                        return retval;
                    }
                }
                if (response != NULL) {
                    free_domain_info_ptrs(response);
                    FREE(response);
                }

                if (next_q->qfq_query->qc_state > Q_SENT) 
                    *data_received = 1;
            }
        }
    } else {
       *data_missing = 0;
    } 

    return VAL_NO_ERROR;
}

static int
fix_glue(val_context_t * context, 
         struct queries_for_query **queries,
         int *data_received,
         int *data_missing)
{
    struct queries_for_query *next_q;
    int    retval;
    char   name_p[NS_MAXDNAME];

    if (context == NULL || queries == NULL || data_received == NULL || data_missing == NULL)
        return VAL_BAD_ARGUMENT;

    if (*data_missing == 0)
        return VAL_NO_ERROR;

    *data_missing = 0;
    for (next_q = *queries; next_q; next_q = next_q->qfq_next) {
        if (next_q->qfq_query->qc_state < Q_ANSWERED) {
            *data_missing = 1;
        }
        if (next_q->qfq_query->qc_state == Q_WAIT_FOR_GLUE) {

            if (-1 == ns_name_ntop(next_q->qfq_query->qc_name_n, name_p, sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");
            /*
             * Check if we fetched this same glue before and it was answered 
             */
            if (VAL_NO_ERROR != (retval = 
                        merge_glue_in_referral(context, 
                                               next_q->qfq_query, 
                                               queries)))
                        return retval;

            if (next_q->qfq_query->qc_state == Q_INIT) {
                val_log(context, LOG_DEBUG,
                        "successfully fetched glue for {%s %d %d}", name_p,
                        next_q->qfq_query->qc_class_h, next_q->qfq_query->qc_type_h);
            } else if (next_q->qfq_query->qc_state != Q_WAIT_FOR_GLUE) {
                val_log(context, LOG_DEBUG,
                        "could not fetch glue for {%s %d %d}", name_p,
                        next_q->qfq_query->qc_class_h, next_q->qfq_query->qc_type_h);
            } 
        }
    }

    return VAL_NO_ERROR;
}


static int
check_proof_sanity(val_context_t * context,
                   struct val_internal_result *w_results,
                   struct queries_for_query **queries,
                   struct val_result_chain **results,
                   struct val_query_chain *top_q)
{
    struct val_digested_auth_chain *as;
    struct val_internal_result *res;
    struct val_result_chain *proof_res = NULL;
    val_status_t    status = VAL_DONT_KNOW;
    int             retval = VAL_NO_ERROR;
    u_int32_t soa_ttl_x;

    if (top_q == NULL)
        return VAL_BAD_ARGUMENT;

    if (top_q->qc_type_h == ns_t_ds) {
        /*
         * If we've asked for a DS and the soa has the same 
         * name, we've actually asked the child zone
         * Don't re-try from the root because we then will have the
         * possibility of an infinite loop
         */
        for (res = w_results; res; res = res->val_rc_next) {
            if (NULL == (as = res->val_rc_rrset))
                continue;
            if (as->val_ac_rrset->val_rrset_type_h == ns_t_soa) {
                if (!namecmp(as->val_ac_rrset->val_rrset_name_n,
                             top_q->qc_name_n)) {
                    val_log(context, LOG_DEBUG,
                            "Bogus Response: Proof of non-existence for DS received from child");
                    status = VAL_BOGUS_PROOF;
                }
                break;
            }
        }
    }

    if (status == VAL_DONT_KNOW) {
        if (VAL_NO_ERROR !=
            (retval =
             prove_nonexistence(context, w_results, queries, &proof_res, results,
                                top_q->qc_name_n, top_q->qc_type_h,
                                top_q->qc_class_h, 0, NULL, top_q->qc_proof,
                                &status, &soa_ttl_x)))
            return retval;
    }

    if (proof_res) {
        proof_res->val_rc_status = status;
        if (val_istrusted(status)) {
            SET_MIN_TTL(top_q->qc_ttl_x, soa_ttl_x);
        }
    }

    return VAL_NO_ERROR;
}

static int
check_wildcard_sanity(val_context_t * context,
                      struct val_internal_result *w_results,
                      struct queries_for_query **queries,
                      struct val_result_chain **results,
                      struct val_query_chain *top_q)
{
    struct val_internal_result *res;
    struct val_result_chain *target_res;
    struct val_result_chain *new_res;
    u_int8_t       *zonecut_n;
    val_status_t    status;
    int             retval;
    u_int32_t       ttl_x = 0;

    zonecut_n = NULL;
    target_res = NULL;

    for (res = w_results; res; res = res->val_rc_next) {
        if ((res->val_rc_status == VAL_SUCCESS) &&
            (res->val_rc_rrset) &&
            (!res->val_rc_consumed) &&
            (res->val_rc_rrset->val_ac_status == VAL_AC_WCARD_VERIFIED ||
             /* only rrsigs should have the next state, but adding here 
              * so that we know that this is also handled
              */ 
             res->val_rc_rrset->val_ac_status == VAL_AC_WCARD_VERIFIED_SKEW)) {

            /*
             * Any proofs that have been wildcard expanded are bogus 
             */
            if (res->val_rc_is_proof) {
                val_log(context, LOG_DEBUG,
                        "Wildcard sanity check failed");
                if (VAL_NO_ERROR !=
                    (retval =
                     transform_single_result(context, res, queries, results, 
                                             target_res, &new_res))) {
                    goto err;
                }
                target_res = new_res;
                target_res->val_rc_status = VAL_BOGUS_PROOF;

            } else {
                /*
                 * Move to a fresh result structure 
                 */
                if (VAL_NO_ERROR !=
                    (retval =
                     transform_single_result(context, res, queries, results, NULL,
                                             &new_res))) {
                    goto err;
                }
                target_res = new_res;

                /*
                 * we need to prove that this type exists in some
                 *  accompanying wildcard
                 */
                if ((res->val_rc_rrset->_as.ac_data) &&
                    ((zonecut_n =
                      res->val_rc_rrset->_as.ac_data->rrs_zonecut_n))) {
                    /*
                     * Check if this proves non-existence of name 
                     */
                    if (VAL_NO_ERROR !=
                        (retval =
                         prove_nonexistence(context, w_results, queries, &target_res,
                                            results, top_q->qc_name_n, top_q->qc_type_h,
                                            top_q->qc_class_h, 1, zonecut_n, top_q->qc_proof, &status,
                                            &ttl_x)))
                         
                         /*prove_existence(context, domain_name_n,
                                         res->val_rc_rrset->_as.ac_data->
                                         rrs.val_rrset_type_h, zonecut_n,
                                         w_results, queries, &target_res, results,
                                         &status, &ttl_x))) */
                        goto err;

                    SET_MIN_TTL(top_q->qc_ttl_x, ttl_x);

                    target_res->val_rc_status = status;
                    if ((status == VAL_NONEXISTENT_NAME ||
#ifdef LIBVAL_NSEC3
                         status == VAL_NONEXISTENT_NAME_OPTOUT ||
#endif
                         status == VAL_NONEXISTENT_NAME_NOCHAIN)
                        && (target_res->val_rc_answer)) {
                        /*
                         * Change from VAL_AC_WCARD_VERIFIED to VAL_AC_VERIFIED 
                         */
                        target_res->val_rc_answer->val_ac_status =
                            VAL_AC_VERIFIED;
                    }
                } else {
                    /*
                     * Can't prove wildcard 
                     */
                    val_log(context, LOG_DEBUG,
                            "Wildcard sanity check failed");
                    target_res->val_rc_status = VAL_BOGUS;
                }
            }
        }
    }
    return VAL_NO_ERROR;

  err:
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    return retval;

}

static int
check_alias_sanity(val_context_t * context,
                   struct val_internal_result *w_results,
                   struct queries_for_query **queries,
                   struct val_result_chain **results,
                   struct val_query_chain *top_q)
{
    struct val_internal_result *res;
    struct val_result_chain *new_res = NULL;
    int             done = 0;
    int             cname_seen = 0;
    int             dname_seen = 0;
    u_int8_t       *qname_n = NULL;
    struct query_list *ql = NULL;
    int             loop = 0;
    int             retval;
    u_int8_t       *p;
    int             is_same_name;
    u_int8_t        temp_name[NS_MAXCDNAME];
    u_int32_t       soa_ttl_x;

    if (top_q == NULL)
        return VAL_BAD_ARGUMENT;

    qname_n = top_q->qc_original_name;
    while (!done && qname_n) {
        done = 1;
        new_res = NULL;

        if (IT_HASNT !=
            register_query(&ql, qname_n, top_q->qc_type_h,
                           top_q->qc_zonecut_n)) {
            loop = 1;
            val_log(context, LOG_DEBUG, "Loop in alias chain detected");
            if (new_res) {
                new_res->val_rc_status = VAL_BOGUS;
            }
            break;
        }

        for (res = w_results; res; res = res->val_rc_next) {
            /*
             * try constructing a cname/dname chain 
             */

            if (!res->val_rc_rrset || !res->val_rc_rrset->_as.ac_data)
                continue;

            is_same_name =
                (0 ==
                 namecmp(qname_n,
                         res->val_rc_rrset->_as.ac_data->rrs.
                         val_rrset_name_n));

            if ((is_same_name) &&
                (res->val_rc_rrset->_as.ac_data->rrs_ans_kind ==
                 SR_ANS_CNAME)) {
                /*
                 * found the next element 
                 */
                done = 0;
                cname_seen = 1;
                /*
                 * find the next link in the cname chain 
                 */
                if (res->val_rc_rrset->_as.ac_data->rrs.val_rrset_data) {
                    qname_n =
                        res->val_rc_rrset->_as.ac_data->rrs.
                        val_rrset_data->rr_rdata;
                } else {
                    qname_n = NULL;
                    res->val_rc_status = VAL_BOGUS;
                }
            } else
                if ((res->val_rc_rrset->_as.ac_data->rrs_ans_kind ==
                     SR_ANS_DNAME)
                    && (NULL !=
                        (p =
                         (u_int8_t *) namename(qname_n,
                                             res->val_rc_rrset->_as.
                                             ac_data->rrs.
                                             val_rrset_name_n)))) {
                /*
                 * found the next dname element 
                 */
                done = 0;
                dname_seen = 1;

                /*
                 * find the next link in the dname chain 
                 */
                if (res->val_rc_rrset->_as.ac_data->rrs.val_rrset_data) {
                    int             len1 = p - qname_n;
                    int             len2 =
                        wire_name_length(res->val_rc_rrset->_as.ac_data->
                                         rrs.val_rrset_data->rr_rdata);
                    if (len1 + len2 > sizeof(temp_name)) {
                        qname_n = NULL;
                        res->val_rc_status = VAL_BOGUS;
                    } else {
                        memcpy(temp_name, qname_n, len1);
                        memcpy(&temp_name[len1],
                               res->val_rc_rrset->_as.ac_data->rrs.
                               val_rrset_data->rr_rdata, len2);
                        qname_n = temp_name;
                    }
                } else {
                    qname_n = NULL;
                    res->val_rc_status = VAL_BOGUS;
                }

            } else if (!is_same_name ||
                       (top_q->qc_type_h !=
                            res->val_rc_rrset->_as.ac_data->rrs.
                            val_rrset_type_h && 
                        top_q->qc_type_h != ns_t_any)
                       || (top_q->qc_class_h !=
                           res->val_rc_rrset->_as.ac_data->rrs.
                           val_rrset_class_h)) {
                continue;
            }

            if ((res->val_rc_consumed) && results) {
                /*
                 * search for existing result structure 
                 */
                for (new_res = *results; new_res;
                     new_res = new_res->val_rc_next) {
                    if (new_res->val_rc_answer
                        && new_res->val_rc_answer->val_ac_rrset) {
                        if (!namecmp
                            (qname_n,
                             new_res->val_rc_answer->val_ac_rrset->
                             val_rrset_name_n)) {
                            break;
                        }
                    }
                }
            }

            /*
             * or create a new one 
             */
            if (new_res == NULL) {
                if (VAL_NO_ERROR !=
                    (retval = transform_single_result(context, res, queries, results,
                                                      NULL, &new_res))) {
                    goto err;
                }
            }

            new_res->val_rc_status = res->val_rc_status;

            break;
        }
    }

    if (cname_seen || dname_seen) {
        if ((new_res == NULL) && qname_n && !loop) {
            /*
             * the last element in the chain was a cname or dname,
             * therefore we must check for a proof of non-existence 
             */
            val_status_t    status = VAL_DONT_KNOW;
            struct val_result_chain *proof_res = NULL;
            if (VAL_NO_ERROR !=
                (retval =
                 prove_nonexistence(context, w_results, queries, &proof_res,
                                    results, qname_n, top_q->qc_type_h,
                                    top_q->qc_class_h, 0, NULL, top_q->qc_proof,
                                    &status, &soa_ttl_x))) {
                goto err;
            }

            if (proof_res) {
                proof_res->val_rc_status = status;
                if (val_istrusted(status)) {
                    SET_MIN_TTL(top_q->qc_ttl_x, soa_ttl_x);
                }
            } else {
                /*
                 * create a new result element 
                 */
                if (VAL_NO_ERROR !=
                    (retval =
                     transform_single_result(context, NULL, queries, results, NULL,
                                             &new_res))) {
                    goto err;
                }
                new_res->val_rc_status = VAL_INCOMPLETE_PROOF;
            }
        }

        /*
         * All other cnames, dnames and answers are bogus 
         */
        for (res = w_results; res; res = res->val_rc_next) {
            if ((!res->val_rc_consumed) &&
                (res->val_rc_status != VAL_IGNORE_VALIDATION)) {
                res->val_rc_status = VAL_BOGUS;
            }
        }
    }

    deregister_queries(&ql);

    return VAL_NO_ERROR;

  err:
    deregister_queries(&ql);
    /*
     * free actual results 
     */
    val_free_result_chain(*results);
    *results = NULL;
    return retval;
}

/*
 * Identify if there is anything that must be proved
 */
static int
perform_sanity_checks(val_context_t * context,
                      struct val_internal_result *w_results,
                      struct queries_for_query **queries,
                      struct val_result_chain **results,
                      struct val_query_chain *top_q)
{
    struct val_internal_result *res;
    int             partially_wrong = 0;
    int             negative_proof = 1;
    int             retval;
    u_int32_t       ttl_x = 0;


    /*
     * Fix validation results 
     */
    for (res = w_results; res; res = res->val_rc_next) {

        /*
         * Some error most likely, reflected in the val_query_chain 
         */
        if (res->val_rc_rrset == NULL)
            res->val_rc_status = VAL_ERROR;

        /*
         *  Special case of provably unsecure: algorithms used
         *  for signing the DNSKEY record are not understood
         */
        if (res->val_rc_status == VAL_BOGUS_PROVABLE) {
            /*
             * implies that the trust flag is set 
             */
            struct val_digested_auth_chain *as;
            struct val_digested_auth_chain *top_as;
            top_as = res->val_rc_rrset;
            as = top_as;
            while(as) {
                if ((as->val_ac_rrset) &&
                    (as->val_ac_rrset->val_rrset_type_h == ns_t_dnskey)) {
                    if (as->val_ac_status == VAL_AC_NOT_VERIFIED) {
                        /*
                         * see if one of the DNSKEYs links up 
                         */
                        struct rr_rec  *drr;
                        for (drr = as->val_ac_rrset->val_rrset_data; drr;
                             drr = drr->rr_next) {
                            if (drr->rr_status ==
                                VAL_AC_UNKNOWN_ALGORITHM_LINK) {
                                if (is_pu_trusted(context, 
                                            as->val_ac_rrset->val_rrset_name_n, &ttl_x))
                                    res->val_rc_status = VAL_PROVABLY_UNSECURE;
                                else
                                    res->val_rc_status = VAL_BAD_PROVABLY_UNSECURE;
                                SET_MIN_TTL(as->val_ac_query->qc_ttl_x, ttl_x);
                                break;
                            }
                        }
                    }
                }

                as = get_ac_trust(context, as, queries); 
            }
        }

        if (res->val_rc_status == VAL_DONT_GO_FURTHER)
            res->val_rc_status = VAL_SUCCESS;

        /*
         * If we see something other than a proof, this is no longer
         * "only a negative response"
         */
        if (!res->val_rc_is_proof)
            negative_proof = 0;

        if (!val_istrusted(res->val_rc_status)) {
            /*
             * All components were not validated success
             */
            partially_wrong = 1;
            if (top_q->qc_bad >= 0)
                top_q->qc_bad = 1;
        } else if (val_isvalidated(res->val_rc_status)) {
            top_q->qc_bad = -1; /* good result */
        }
    }

    if (negative_proof) {
        if (partially_wrong) {
            /*
             * mark all answers as bogus - 
             * all answers are related in the proof 
             */
            val_log(context, LOG_DEBUG, "Bogus Proof");
            for (res = w_results; res; res = res->val_rc_next)
                res->val_rc_status = VAL_BOGUS_PROOF;
        } else {
            /*
             * We only received some proof of non-existence 
             */
            return check_proof_sanity(context, w_results, queries, results, top_q);
        }
        return VAL_NO_ERROR;
    }

    /*
     * Ensure that we have the relevant proofs to 
     * support the primary assertion 
     */

    /*
     * If there was some wildcard expansion, 
     * make sure that this was for a valid type
     */
    if (VAL_NO_ERROR !=
        (retval =
         check_wildcard_sanity(context, w_results, queries, results, top_q)))
        return retval;

    /*
     * Check cname/dname sanity
     */
    if (VAL_NO_ERROR !=
        (retval = check_alias_sanity(context, w_results, queries, results, top_q)))
        return retval;

    return VAL_NO_ERROR;
}

static int
create_error_result(struct val_query_chain *top_q,
                    struct val_internal_result **w_results)
{
    struct val_internal_result *w_temp;
    if (top_q == NULL)
        return VAL_BAD_ARGUMENT;

    *w_results = NULL;
    if (top_q->qc_ans) {
        w_temp = (struct val_internal_result *)
            MALLOC(sizeof(struct val_internal_result));
        if (w_temp == NULL) {
            return VAL_OUT_OF_MEMORY;
        }
        w_temp->val_rc_rrset = top_q->qc_ans;
        w_temp->val_rc_is_proof = 0;
        w_temp->val_rc_consumed = 0;
        w_temp->val_rc_status =
            VAL_DNS_ERROR_BASE + top_q->qc_state - Q_ERROR_BASE;
        w_temp->val_rc_next = NULL;
        *w_results = w_temp;
    }
    if (top_q->qc_proof) {
        w_temp = (struct val_internal_result *)
            MALLOC(sizeof(struct val_internal_result));
        if (w_temp == NULL) {
            return VAL_OUT_OF_MEMORY;
        }
        w_temp->val_rc_rrset = top_q->qc_proof;
        w_temp->val_rc_is_proof = 1;
        w_temp->val_rc_consumed = 0;
        w_temp->val_rc_status =
            VAL_DNS_ERROR_BASE + top_q->qc_state - Q_ERROR_BASE;
        w_temp->val_rc_next = NULL;
        if (*w_results == NULL)
            *w_results = w_temp;
        else
            (*w_results)->val_rc_next = w_temp;
    }
    if (*w_results == NULL) {
        *w_results = (struct val_internal_result *)
            MALLOC(sizeof(struct val_internal_result));
        if ((*w_results) == NULL) {
            return VAL_OUT_OF_MEMORY;
        }
        (*w_results)->val_rc_rrset = NULL;
        (*w_results)->val_rc_is_proof = 0;
        (*w_results)->val_rc_consumed = 0;
        (*w_results)->val_rc_status =
            VAL_DNS_ERROR_BASE + top_q->qc_state - Q_ERROR_BASE;
        (*w_results)->val_rc_next = NULL;
    }

    return VAL_NO_ERROR;
}

#define GET_LATEST_TIMESTAMP(ctx, file, cur_ts, new_ts) do { \
    memset(&new_ts, 0, sizeof(struct stat));\
    if (!file) {\
        if (cur_ts != 0) {\
            val_log(ctx, LOG_WARNING, "%s disappeared; continuing to use old", file);\
        }\
    } else {\
        if(0 != stat(file, &new_ts)) {\
            val_log(ctx, LOG_WARNING, "%s disappeared; continuing to use old", file);\
        }\
    }\
}while (0)

int construct_authentication_chain(val_context_t * context,
                                   struct val_query_chain *top_q,
                                   struct queries_for_query **queries,
                                   struct val_internal_result **w_results,
                                   struct val_result_chain **results,
                                   int *done)
{
    int             ans_done = 0;
    int             proof_done = 0;
    int             retval;

    if (context == NULL || top_q == NULL || 
        queries == NULL || results == NULL || done == NULL)
        return VAL_BAD_ARGUMENT;
    
    *done = 0;
    *results = NULL;
    
    /*
     * No point going ahead if our original query had error conditions 
     */
    if (top_q->qc_state > Q_ERROR_BASE) {
        /*
         * the original query had some error 
         */
        if (VAL_NO_ERROR != (retval = create_error_result(top_q, w_results)))
            return retval;    

        ans_done = 1;
        proof_done = 1;
    } else if (top_q->qc_state > Q_SENT) {

        /*
         * validate what ever is possible. 
         */

        /*
         * validate all answers 
         */
        if (VAL_NO_ERROR !=
            (retval =
             verify_and_validate(context, queries, top_q, 0,
                                 w_results, &ans_done))) {
            return retval;
        }

        /*
         * validate all proofs 
         */
        if (VAL_NO_ERROR !=
            (retval =
             verify_and_validate(context, queries, top_q, 1,
                                 w_results, &proof_done))) {
            return retval;
        }
    }

    if (ans_done && proof_done && *w_results) { 
        
        *done = 1;

        retval = perform_sanity_checks(context, *w_results, queries, results, top_q);

        if (retval == VAL_NO_ERROR)
            retval =
                transform_outstanding_results(context, *w_results, queries, results, NULL,
                                              VAL_IRRELEVANT_PROOF);
    }

    return VAL_NO_ERROR;
}

int try_chase_query(val_context_t * context,
                    u_char * domain_name_n,
                    const u_int16_t q_class,
                    const u_int16_t type,
                    const u_int8_t flags,
                    struct queries_for_query **queries,
                    struct val_result_chain **results,
                    int *done)
{
    struct queries_for_query *top_q = NULL;
    struct val_internal_result *w_res = NULL;
    struct val_internal_result *w_results = NULL;
    int retval;

    if (context == NULL || queries == NULL || results == NULL || done == NULL)
        return VAL_BAD_ARGUMENT;

    if (VAL_NO_ERROR !=
        (retval =
         add_to_qfq_chain(context, queries, domain_name_n, type,
                          q_class, flags, &top_q))) {
        return retval;
    }
    if (VAL_NO_ERROR != (retval = 
                    construct_authentication_chain(context, 
                                                   top_q->qfq_query, 
                                                   queries,
                                                   &w_results,
                                                   results, 
                                                   done)))
        return retval;

    /*
     *  The val_internal_result structure only has a reference to 
     *  the authentication chain. The actual authentication chain
     *  is still present in the validator context.
     */
    w_res = w_results;
    while (w_res) {
        w_results = w_res->val_rc_next;
        FREE(w_res);
        w_res = w_results;
    }

    return VAL_NO_ERROR;
}

/*
 * Look inside the cache, ask the resolver for missing data.
 * Then try and validate what ever is possible.
 * Return when we are ready with some useful answer (error condition is 
 * a useful answer)
 */
int
val_resolve_and_check(val_context_t * ctx,
                      u_char * domain_name_n,
                      const u_int16_t q_class,
                      const u_int16_t type,
                      const u_int8_t flags,
                      struct val_result_chain **results)
{

    int             retval;
    struct queries_for_query *top_q;
    struct queries_for_query *added_q;
    char            name_p[NS_MAXDNAME];
    struct val_internal_result *w_res = NULL;
    struct val_internal_result *w_results = NULL;
    struct queries_for_query *queries = NULL;
    int done = 0;
    int data_received;
    int data_missing;

    val_context_t  *context = NULL;
    
    if ((results == NULL) || (domain_name_n == NULL))
        return VAL_BAD_ARGUMENT;


    /*
     * Create a default context if one does not exist 
     */
    if (ctx == NULL) {
        if (VAL_NO_ERROR != (retval = val_create_context(NULL, &context)))
            return retval;
        CTX_LOCK_RESPOL_SH(context);
        CTX_LOCK_VALPOL_SH(context);
    } else {
        /* Check if the configuration file has changed since the last time we read it */
        struct stat rsb, vsb, hsb;
        
        context = (val_context_t *) ctx;

        CTX_LOCK_RESPOL_SH(context);
        GET_LATEST_TIMESTAMP(context, context->resolv_conf, context->r_timestamp, rsb);
        if (rsb.st_mtime != 0 && 
                rsb.st_mtime != context->r_timestamp) {
            CTX_UNLOCK_RESPOL(context);
            val_refresh_resolver_policy(context);
            CTX_LOCK_RESPOL_SH(context);
        }

        GET_LATEST_TIMESTAMP(context, context->root_conf, context->h_timestamp, hsb);
        if (hsb.st_mtime != 0 && 
                hsb.st_mtime != context->h_timestamp){
            CTX_UNLOCK_RESPOL(context);
            val_refresh_root_hints(context);
            CTX_LOCK_RESPOL_SH(context);
        }

        CTX_LOCK_VALPOL_SH(context);
        GET_LATEST_TIMESTAMP(context, context->dnsval_conf, context->v_timestamp, vsb);
        if (vsb.st_mtime != 0 && 
                vsb.st_mtime != context->v_timestamp) {
            CTX_UNLOCK_VALPOL(context);
            val_refresh_validator_policy(context);
            CTX_LOCK_VALPOL_SH(context);
        }
    }
  
    if (-1 == ns_name_ntop(domain_name_n, name_p, sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
    val_log(context, LOG_DEBUG,
            "val_resolve_and_check(): looking for {%s %d %d}", name_p,
            q_class, type);

    CTX_LOCK_ACACHE(context);
    
    if (VAL_NO_ERROR != (retval =
                add_to_qfq_chain(context, &queries, domain_name_n, type,
                                 q_class, flags, &added_q))) {
        goto err;
    }
    top_q = added_q;
        
    data_missing = 1;
    data_received = 0;
    while (!done) {
        struct queries_for_query *last_q;

        /*
         * keep track of the last entry added to the query chain 
         */
        last_q = queries;

        /*
         * Data might already be present in the cache 
         */
        /*
         * XXX by-pass this functionality through flags if needed 
         */
        if (VAL_NO_ERROR !=
            (retval = ask_cache(context, &queries, &data_received, &data_missing)))
            goto err;

        /*
         * Send un-sent queries 
         */
        /*
         * XXX by-pass this functionality through flags if needed 
         */
        if (VAL_NO_ERROR !=
            (retval = ask_resolver(context, &queries, &data_received, &data_missing)))
            goto err;


        if (VAL_NO_ERROR !=
            (retval = fix_glue(context, &queries, &data_received, &data_missing)))
            goto err;
        
        /*
         * check if more queries have been added 
         */
        if (last_q != queries) {
            /*
             * There are new queries to send out -- do this first; 
             * we may also find this data in the cache 
             */
            continue;
        }

        if (data_received || !data_missing) {

            if (VAL_NO_ERROR != (retval = 
                    construct_authentication_chain(context, 
                                                   top_q->qfq_query, 
                                                   &queries,
                                                   &w_results,
                                                   results, 
                                                   &done)))
                goto err;

            data_missing = 1;
            data_received = 0;
        }

        if (!done) {
            /* Release the lock, let some other thread get some time slice to run */
            CTX_UNLOCK_ACACHE(context);
                
#ifndef VAL_NO_THREADS
            sleep(0);
#endif

            /* Re-acquire the lock */
            CTX_LOCK_ACACHE(context);
        }
    }

    retval = VAL_NO_ERROR;

  err:
    CTX_UNLOCK_ACACHE(context);
    CTX_UNLOCK_RESPOL(context);
    CTX_UNLOCK_VALPOL(context);

    /*
     *  The val_internal_result structure only has a reference to 
     *  the authentication chain. The actual authentication chain
     *  is still present in the validator context.
     */
    w_res = w_results;
    while (w_res) {
        w_results = w_res->val_rc_next;
        FREE(w_res);
        w_res = w_results;
    }
    free_qfq_chain(queries);

    return retval;
}

/*
 * Function: val_istrusted
 *
 * Purpose:   Tells whether the given validation status code represents an
 *            answer that can be trusted.  An answer can be trusted if it
 *            has been obtained locally (for example from /etc/hosts) or if
 *            it was an authentic response from the validator.
 *
 * Parameter: val_status -- a validation status code returned by the validator
 *
 * Returns:   1 if the validation status represents a trusted response
 *            0 if the validation status does not represent a trusted response
 *
 */
int
val_istrusted(val_status_t val_status)
{
    
    switch (val_status) {
    case VAL_SUCCESS:
    case VAL_NONEXISTENT_NAME:
    case VAL_NONEXISTENT_TYPE:
    case VAL_NONEXISTENT_NAME_NOCHAIN:
    case VAL_NONEXISTENT_TYPE_NOCHAIN:
#ifdef LIBVAL_NSEC3
    case VAL_NONEXISTENT_NAME_OPTOUT:
#endif
    case VAL_VALIDATED_ANSWER:
    case VAL_TRUSTED_ANSWER:
    case VAL_PROVABLY_UNSECURE:
    case VAL_IGNORE_VALIDATION:
    case VAL_TRUSTED_ZONE:
    case VAL_LOCAL_ANSWER:
        return 1;

        
    default:
        return 0;
    }
}

/*
 * Function: val_isvalidated
 *
 * Purpose:   Tells whether the given validation status code represents an
 *            answer that was cryptographically validated up to a configured
 *            trust anchor. This is independent of whether or not the status
 *            is 'trusted', since trust is a policy decision.
 *
 * Parameter: val_status -- a validation status code returned by the validator
 *
 * Returns:   1 if the validation status represents a validated response
 *            0 if the validation status does not represent a validated response
 *
 */
int
val_isvalidated(val_status_t val_status)
{
    switch (val_status) {
    case VAL_SUCCESS:
    case VAL_NONEXISTENT_NAME:
    case VAL_NONEXISTENT_TYPE:
    case VAL_VALIDATED_ANSWER:
        return 1;

    default:
        return 0;
    }
}
