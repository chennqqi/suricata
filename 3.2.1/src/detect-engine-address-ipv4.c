/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * IPV4 Address part of the detection engine.
 */

#include "suricata-common.h"

#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include "util-cidr.h"
#include "util-unittest.h"

#include "detect-engine-address.h"
#include "detect-engine-siggroup.h"
#include "detect-engine-port.h"

#include "util-error.h"
#include "util-debug.h"

/**
 * \brief Compares 2 addresses(address ranges) and returns the relationship
 *        between the 2 addresses.
 *
 * \param a Pointer to the first address instance to be compared.
 * \param b Pointer to the second address instance to be compared.
 *
 * \retval ADDRESS_EQ If the 2 address ranges a and b, are equal.
 * \retval ADDRESS_ES b encapsulates a. b_ip1[...a_ip1...a_ip2...]b_ip2.
 * \retval ADDRESS_EB a encapsulates b. a_ip1[...b_ip1....b_ip2...]a_ip2.
 * \retval ADDRESS_LE a_ip1(...b_ip1==a_ip2...)b_ip2
 * \retval ADDRESS_LT a_ip1(...b_ip1...a_ip2...)b_ip2
 * \retval ADDRESS_GE b_ip1(...a_ip1==b_ip2...)a_ip2
 * \retval ADDRESS_GT a_ip1 > b_ip2, i.e. the address range for 'a' starts only
 *                    after the end of the address range for 'b'
 */
int DetectAddressCmpIPv4(DetectAddress *a, DetectAddress *b)
{
    uint32_t a_ip1 = ntohl(a->ip.addr_data32[0]);
    uint32_t a_ip2 = ntohl(a->ip2.addr_data32[0]);
    uint32_t b_ip1 = ntohl(b->ip.addr_data32[0]);
    uint32_t b_ip2 = ntohl(b->ip2.addr_data32[0]);

    if (a_ip1 == b_ip1 && a_ip2 == b_ip2) {
        SCLogDebug("ADDRESS_EQ");
        return ADDRESS_EQ;
    } else if (a_ip1 >= b_ip1 && a_ip1 <= b_ip2 && a_ip2 <= b_ip2) {
        SCLogDebug("ADDRESS_ES");
        return ADDRESS_ES;
    } else if (a_ip1 <= b_ip1 && a_ip2 >= b_ip2) {
        SCLogDebug("ADDRESS_EB");
        return ADDRESS_EB;
    } else if (a_ip1 < b_ip1 && a_ip2 < b_ip2 && a_ip2 >= b_ip1) {
        SCLogDebug("ADDRESS_LE");
        return ADDRESS_LE;
    } else if (a_ip1 < b_ip1 && a_ip2 < b_ip2) {
        SCLogDebug("ADDRESS_LT");
        return ADDRESS_LT;
    } else if (a_ip1 > b_ip1 && a_ip1 <= b_ip2 && a_ip2 > b_ip2) {
        SCLogDebug("ADDRESS_GE");
        return ADDRESS_GE;
    } else if (a_ip1 > b_ip2) {
        SCLogDebug("ADDRESS_GT");
        return ADDRESS_GT;
    } else {
        /* should be unreachable */
        SCLogDebug("Internal Error: should be unreachable");
    }

    return ADDRESS_ER;
}

/**
 * \brief Cut groups and merge sigs
 *
 *       a = 1.2.3.4, b = 1.2.3.4-1.2.3.5
 *       must result in: a == 1.2.3.4, b == 1.2.3.5, c == NULL
 *
 *       a = 1.2.3.4, b = 1.2.3.3-1.2.3.5
 *       must result in: a == 1.2.3.3, b == 1.2.3.4, c == 1.2.3.5
 *
 *       a = 1.2.3.0/24 b = 1.2.3.128-1.2.4.10
 *       must result in: a == 1.2.3.0/24, b == 1.2.4.0-1.2.4.10, c == NULL
 *
 *       a = 1.2.3.4, b = 1.2.3.0/24
 *       must result in: a == 1.2.3.0-1.2.3.3, b == 1.2.3.4, c == 1.2.3.5-1.2.3.255
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectAddressCutIPv4(DetectEngineCtx *de_ctx, DetectAddress *a,
                         DetectAddress *b, DetectAddress **c)
{
    uint32_t a_ip1 = ntohl(a->ip.addr_data32[0]);
    uint32_t a_ip2 = ntohl(a->ip2.addr_data32[0]);
    uint32_t b_ip1 = ntohl(b->ip.addr_data32[0]);
    uint32_t b_ip2 = ntohl(b->ip2.addr_data32[0]);
    DetectAddress *tmp = NULL;
    DetectAddress *tmp_c = NULL;
    int r = 0;

    /* default to NULL */
    *c = NULL;

    r = DetectAddressCmpIPv4(a, b);
    if (r != ADDRESS_ES && r != ADDRESS_EB && r != ADDRESS_LE && r != ADDRESS_GE) {
        SCLogDebug("we shouldn't be here");
        goto error;
    }

    /* get a place to temporary put sigs lists */
    tmp = DetectAddressInit();
    if (tmp == NULL)
        goto error;

    /* we have 3 parts: [aaa[abab)bbb]
     * part a: a_ip1 <-> b_ip1 - 1
     * part b: b_ip1 <-> a_ip2
     * part c: a_ip2 + 1 <-> b_ip2
     */
    if (r == ADDRESS_LE) {
        SCLogDebug("DetectAddressCutIPv4: r == ADDRESS_LE");

        a->ip.addr_data32[0]  = htonl(a_ip1);
        a->ip2.addr_data32[0] = htonl(b_ip1 - 1);

        b->ip.addr_data32[0]  = htonl(b_ip1);
        b->ip2.addr_data32[0] = htonl(a_ip2);

        tmp_c = DetectAddressInit();
        if (tmp_c == NULL)
            goto error;

        tmp_c->ip.family = AF_INET;
        tmp_c->ip.addr_data32[0] = htonl(a_ip2 + 1);
        tmp_c->ip2.addr_data32[0] = htonl(b_ip2);
        *c = tmp_c;

    /* we have 3 parts: [bbb[baba]aaa]
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> b_ip2
     * part c: b_ip2 + 1 <-> a_ip2
     */
    } else if (r == ADDRESS_GE) {
        SCLogDebug("DetectAddressCutIPv4: r == ADDRESS_GE");

        a->ip.addr_data32[0] = htonl(b_ip1);
        a->ip2.addr_data32[0] = htonl(a_ip1 - 1);

        b->ip.addr_data32[0] = htonl(a_ip1);
        b->ip2.addr_data32[0] = htonl(b_ip2);

        tmp_c = DetectAddressInit();
        if (tmp_c == NULL)
            goto error;

        tmp_c->ip.family = AF_INET;
        tmp_c->ip.addr_data32[0]  = htonl(b_ip2 + 1);
        tmp_c->ip2.addr_data32[0] = htonl(a_ip2);
        *c = tmp_c;

        /* we have 2 or three parts:
         *
         * 2 part: [[abab]bbb] or [bbb[baba]]
         * part a: a_ip1 <-> a_ip2
         * part b: a_ip2 + 1 <-> b_ip2
         *
         * part a: b_ip1 <-> a_ip1 - 1
         * part b: a_ip1 <-> a_ip2
         *
         * 3 part [bbb[aaa]bbb]
         * becomes[aaa[bbb]ccc]
         *
         * part a: b_ip1 <-> a_ip1 - 1
         * part b: a_ip1 <-> a_ip2
         * part c: a_ip2 + 1 <-> b_ip2
         */
    } else if (r == ADDRESS_ES) {
        SCLogDebug("DetectAddressCutIPv4: r == ADDRESS_ES");

        if (a_ip1 == b_ip1) {
            SCLogDebug("DetectAddressCutIPv4: 1");

            a->ip.addr_data32[0] = htonl(a_ip1);
            a->ip2.addr_data32[0] = htonl(a_ip2);

            b->ip.addr_data32[0] = htonl(a_ip2 + 1);
            b->ip2.addr_data32[0] = htonl(b_ip2);

        } else if (a_ip2 == b_ip2) {
            SCLogDebug("DetectAddressCutIPv4: 2");

            a->ip.addr_data32[0]   = htonl(b_ip1);
            a->ip2.addr_data32[0] = htonl(a_ip1 - 1);

            b->ip.addr_data32[0]   = htonl(a_ip1);
            b->ip2.addr_data32[0] = htonl(a_ip2);

        } else {
            SCLogDebug("3");

            a->ip.addr_data32[0]   = htonl(b_ip1);
            a->ip2.addr_data32[0] = htonl(a_ip1 - 1);

            b->ip.addr_data32[0]   = htonl(a_ip1);
            b->ip2.addr_data32[0] = htonl(a_ip2);

            tmp_c = DetectAddressInit();
            if (tmp_c == NULL)
                goto error;

            tmp_c->ip.family = AF_INET;
            tmp_c->ip.addr_data32[0] = htonl(a_ip2 + 1);
            tmp_c->ip2.addr_data32[0] = htonl(b_ip2);
            *c = tmp_c;
        }
        /* we have 2 or three parts:
         *
         * 2 part: [[baba]aaa] or [aaa[abab]]
         * part a: b_ip1 <-> b_ip2
         * part b: b_ip2 + 1 <-> a_ip2
         *
         * part a: a_ip1 <-> b_ip1 - 1
         * part b: b_ip1 <-> b_ip2
         *
         * 3 part [aaa[bbb]aaa]
         * becomes[aaa[bbb]ccc]
         *
         * part a: a_ip1 <-> b_ip2 - 1
         * part b: b_ip1 <-> b_ip2
         * part c: b_ip2 + 1 <-> a_ip2
         */
    } else if (r == ADDRESS_EB) {
        SCLogDebug("DetectAddressCutIPv4: r == ADDRESS_EB");

        if (a_ip1 == b_ip1) {
            SCLogDebug("DetectAddressCutIPv4: 1");

            a->ip.addr_data32[0] = htonl(b_ip1);
            a->ip2.addr_data32[0] = htonl(b_ip2);

            b->ip.addr_data32[0] = htonl(b_ip2 + 1);
            b->ip2.addr_data32[0] = htonl(a_ip2);
        } else if (a_ip2 == b_ip2) {
            SCLogDebug("DetectAddressCutIPv4: 2");

            a->ip.addr_data32[0]   = htonl(a_ip1);
            a->ip2.addr_data32[0] = htonl(b_ip1 - 1);

            b->ip.addr_data32[0]   = htonl(b_ip1);
            b->ip2.addr_data32[0] = htonl(b_ip2);
        } else {
            SCLogDebug("DetectAddressCutIPv4: 3");

            a->ip.addr_data32[0] = htonl(a_ip1);
            a->ip2.addr_data32[0] = htonl(b_ip1 - 1);

            b->ip.addr_data32[0] = htonl(b_ip1);
            b->ip2.addr_data32[0] = htonl(b_ip2);

            tmp_c = DetectAddressInit();
            if (tmp_c == NULL)
                goto error;

            tmp_c->ip.family = AF_INET;
            tmp_c->ip.addr_data32[0] = htonl(b_ip2 + 1);
            tmp_c->ip2.addr_data32[0] = htonl(a_ip2);
            *c = tmp_c;
        }
    }

    if (tmp != NULL)
        DetectAddressFree(tmp);

    return 0;

error:
    if (tmp != NULL)
        DetectAddressFree(tmp);
    return -1;
}

/**
 * \brief Check if the address group list covers the complete IPv4 IP space.
 *
 * \param ag Pointer to a DetectAddress list head, which has to be checked to
 *           see if the address ranges in it, cover the entire IPv4 IP space.
 *
 * \retval 1 Yes, it covers the entire IPv4 address range.
 * \retval 0 No, it doesn't cover the entire IPv4 address range.
 */
int DetectAddressIsCompleteIPSpaceIPv4(DetectAddress *ag)
{
    uint32_t next_ip = 0;

    if (ag == NULL)
        return 0;

    /* if we don't start with 0.0.0.0 we know we're good */
    if (ntohl(ag->ip.addr_data32[0]) != 0x00000000)
        return 0;

    /* if we're ending with 255.255.255.255 while we know we started with
     * 0.0.0.0 it's the complete space */
    if (ntohl(ag->ip2.addr_data32[0]) == 0xFFFFFFFF)
        return 1;

    next_ip = htonl(ntohl(ag->ip2.addr_data32[0]) + 1);
    ag = ag->next;

    for ( ; ag != NULL; ag = ag->next) {

        if (ag->ip.addr_data32[0] != next_ip)
            return 0;

        if (ntohl(ag->ip2.addr_data32[0]) == 0xFFFFFFFF)
            return 1;

        next_ip = htonl(ntohl(ag->ip2.addr_data32[0]) + 1);
    }

    return 0;
}

/**
 * \brief Cuts and returns an address range, which is the complement of the
 *        address range that is supplied as the argument.
 *
 *        For example:
 *
 *        If a = 0.0.0.0-1.2.3.4,
 *            then a = 1.2.3.4-255.255.255.255 and b = NULL
 *        If a = 1.2.3.4-255.255.255.255,
 *            then a = 0.0.0.0-1.2.3.4 and b = NULL
 *        If a = 1.2.3.4-192.168.1.1,
 *            then a = 0.0.0.0-1.2.3.3 and b = 192.168.1.2-255.255.255.255
 *
 * \param a Pointer to an address range (DetectAddress) instance whose complement
 *          has to be returned in a and b.
 * \param b Pointer to DetectAddress pointer, that will be supplied back with a
 *          new DetectAddress instance, if the complement demands so.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectAddressCutNotIPv4(DetectAddress *a, DetectAddress **b)
{
    uint32_t a_ip1 = ntohl(a->ip.addr_data32[0]);
    uint32_t a_ip2 = ntohl(a->ip2.addr_data32[0]);
    DetectAddress *tmp_b = NULL;

    /* default to NULL */
    *b = NULL;

    if (a_ip1 != 0x00000000 && a_ip2 != 0xFFFFFFFF) {
        a->ip.addr_data32[0]  = htonl(0x00000000);
        a->ip2.addr_data32[0] = htonl(a_ip1 - 1);

        tmp_b = DetectAddressInit();
        if (tmp_b == NULL)
            goto error;

        tmp_b->ip.family = AF_INET;
        tmp_b->ip.addr_data32[0]  = htonl(a_ip2 + 1);
        tmp_b->ip2.addr_data32[0] = htonl(0xFFFFFFFF);
        *b = tmp_b;
    } else if (a_ip1 == 0x00000000 && a_ip2 != 0xFFFFFFFF) {
        a->ip.addr_data32[0] = htonl(a_ip2 + 1);
        a->ip2.addr_data32[0] = htonl(0xFFFFFFFF);
    } else if (a_ip1 != 0x00000000 && a_ip2 == 0xFFFFFFFF) {
        a->ip.addr_data32[0] = htonl(0x00000000);
        a->ip2.addr_data32[0] = htonl(a_ip1 - 1);
    } else {
        goto error;
     }

    return 0;

error:
    return -1;
}

/**
 * \brief Extends a target address range if the the source address range is
 *        wider than the target address range on either sides.
 *
 *        Every address is a range, i.e. address->ip1....address->ip2.  For
 *        example 1.2.3.4 to 192.168.1.1.
 *        if source->ip1 is smaller than target->ip1, it indicates that the
 *        source's left address limit is greater(range wise) than the target's
 *        left address limit, and hence we reassign the target's left address
 *        limit to source's left address limit.
 *        Similary if source->ip2 is greater than target->ip2, it indicates that
 *        the source's right address limit is greater(range wise) than the
 *        target's right address limit, and hence we reassign the target's right
 *        address limit to source's right address limit.
 *
 * \param de_ctx Pointer to the detection engine context.
 * \param target Pointer to the target DetectAddress instance that has to be
 *               updated.
 * \param source Pointer to the source DetectAddress instance that is used
 *               to decided whether we extend the target's address range.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectAddressJoinIPv4(DetectEngineCtx *de_ctx, DetectAddress *target,
                          DetectAddress *source)
{
    if (source == NULL || target == NULL)
        return -1;

    if (ntohl(source->ip.addr_data32[0]) < ntohl(target->ip.addr_data32[0]))
        target->ip.addr_data32[0] = source->ip.addr_data32[0];

    if (ntohl(source->ip2.addr_data32[0]) > ntohl(target->ip2.addr_data32[0]))
        target->ip2.addr_data32[0] = source->ip2.addr_data32[0];

    return 0;
}

/********************************Unittests*************************************/

#if 0

static int DetectAddressIPv4TestAddressCmp01(void)
{
    struct in_addr in;
    int result = 1;

    DetectAddress *a = DetectAddressInit();
    if (a == NULL)
        return 0;

    DetectAddress *b = DetectAddressInit();
    if (b == NULL) {
        DetectAddressFree(a);
        return 0;
    }

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_EQ);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_ES);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_EB);

    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LE);

    if (inet_pton(AF_INET, "170.170.170.169", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET, "170.170.170.169", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LE);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_LT);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "185.185.185.185", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    /* we could get a LE */
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    /* we could get a LE */
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET, "128.128.128.128", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.169", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "180.180.180.180", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET, "170.170.170.169", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.169.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "200.200.200.200", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "185.185.185.185", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) == ADDRESS_GT);

    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "200.200.200.200", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_GT);

    if (inet_pton(AF_INET, "182.168.1.2", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "200.200.200.200", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "170.170.170.170", &in) < 0)
        goto error;
    b->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    b->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCmpIPv4(a, b) != ADDRESS_GT);

    DetectAddressFree(a);
    DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4IsCompleteIPSpace02(void)
{
    DetectAddress *a = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 1);

    if (inet_pton(AF_INET, "0.0.0.1", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    DetectAddressFree(a);

    if ( (a = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.254", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    DetectAddressFree(a);

    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    return 0;
}

static int DetectAddressIPv4IsCompleteIPSpace03(void)
{
    DetectAddress *a = NULL;
    DetectAddress *temp = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    temp = a;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "126.36.62.61", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "126.36.62.62", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "222.52.21.62", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "222.52.21.63", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.254", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 1);

    DetectAddressFree(a);

    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    return 0;
}

static int DetectAddressIPv4IsCompleteIPSpace04(void)
{
    DetectAddress *a = NULL;
    DetectAddress *temp = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    temp = a;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "126.36.62.61", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "126.36.62.62", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "222.52.21.62", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "222.52.21.64", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.254", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    if ( (temp->next = DetectAddressInit()) == NULL)
        goto error;
    temp = temp->next;

    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    temp->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    temp->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressIsCompleteIPSpaceIPv4(a) == 0);

    DetectAddressFree(a);

    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    return 0;
}

static int DetectAddressIPv4CutNot05(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        return 0;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCutNotIPv4(a, &b) == -1);

    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4CutNot06(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        return 0;

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCutNotIPv4(a, &b) == 0);

    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    result = (a->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    result &= (a->ip2.addr_data32[0] = in.s_addr);

    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4CutNot07(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        return 0;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCutNotIPv4(a, &b) == 0);

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    result = (a->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    result &= (a->ip2.addr_data32[0] = in.s_addr);

    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4CutNot08(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        return 0;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCutNotIPv4(a, &b) == 0);

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    result &= (a->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    result &= (a->ip2.addr_data32[0] = in.s_addr);

    if (b == NULL) {
        result = 0;
        goto error;
    } else {
        result &= 1;
    }
    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    result &= (b->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    result &= (b->ip2.addr_data32[0] = in.s_addr);

    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4CutNot09(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in_addr in;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        return 0;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    a->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    a->ip2.addr_data32[0] = in.s_addr;
    result &= (DetectAddressCutNotIPv4(a, &b) == 0);

    if (inet_pton(AF_INET, "0.0.0.0", &in) < 0)
        goto error;
    result &= (a->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "1.2.3.3", &in) < 0)
        goto error;
    result &= (a->ip2.addr_data32[0] = in.s_addr);

    if (b == NULL) {
        result = 0;
        goto error;
    } else {
        result &= 1;
    }
    if (inet_pton(AF_INET, "192.168.1.3", &in) < 0)
        goto error;
    result &= (b->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "255.255.255.255", &in) < 0)
        goto error;
    result &= (b->ip2.addr_data32[0] = in.s_addr);

    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int DetectAddressIPv4Join10(void)
{
    struct in_addr in;
    int result = 1;

    DetectAddress *source = DetectAddressInit();
    if (source == NULL)
        return 0;

    DetectAddress *target = DetectAddressInit();
    if (target == NULL) {
        DetectAddressFree(source);
        return 0;
    }

    if (inet_pton(AF_INET, "128.51.61.124", &in) < 0)
        goto error;
    target->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    target->ip2.addr_data32[0] = in.s_addr;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    source->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    source->ip2.addr_data32[0] = in.s_addr;

    result &= (DetectAddressJoinIPv4(NULL, target, source) == 0);
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    result &= (target->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    result &= (target->ip2.addr_data32[0] == in.s_addr);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    target->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    target->ip2.addr_data32[0] = in.s_addr;

    if (inet_pton(AF_INET, "1.2.3.5", &in) < 0)
        goto error;
    source->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.1", &in) < 0)
        goto error;
    source->ip2.addr_data32[0] = in.s_addr;

    result &= (DetectAddressJoinIPv4(NULL, target, source) == 0);
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    result &= (target->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    result &= (target->ip2.addr_data32[0] == in.s_addr);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    target->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    target->ip2.addr_data32[0] = in.s_addr;

    if (inet_pton(AF_INET, "128.1.5.15", &in) < 0)
        goto error;
    source->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "200.202.200.200", &in) < 0)
        goto error;
    source->ip2.addr_data32[0] = in.s_addr;

    result &= (DetectAddressJoinIPv4(NULL, target, source) == 0);
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    result &= (target->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "200.202.200.200", &in) < 0)
        goto error;
    result &= (target->ip2.addr_data32[0] == in.s_addr);

    if (inet_pton(AF_INET, "128.51.61.124", &in) < 0)
        goto error;
    target->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    target->ip2.addr_data32[0] = in.s_addr;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    source->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    source->ip2.addr_data32[0] = in.s_addr;

    result &= (DetectAddressJoinIPv4(NULL, target, source) == 0);
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    result &= (target->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    result &= (target->ip2.addr_data32[0] == in.s_addr);

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    target->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    target->ip2.addr_data32[0] = in.s_addr;

    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    source->ip.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    source->ip2.addr_data32[0] = in.s_addr;

    result &= (DetectAddressJoinIPv4(NULL, target, source) == 0);
    if (inet_pton(AF_INET, "1.2.3.4", &in) < 0)
        goto error;
    result &= (target->ip.addr_data32[0] == in.s_addr);
    if (inet_pton(AF_INET, "192.168.1.2", &in) < 0)
        goto error;
    result &= (target->ip2.addr_data32[0] == in.s_addr);

    DetectAddressFree(source);
    DetectAddressFree(target);
    return result;

 error:
    DetectAddressFree(source);
    DetectAddressFree(target);
    return 0;
}

#endif

void DetectAddressIPv4Tests(void)
{
#if 0
    UtRegisterTest("DetectAddressIPv4TestAddressCmp01",
                   DetectAddressIPv4TestAddressCmp01);
    UtRegisterTest("DetectAddressIPv4IsCompleteIPSpace02",
                   DetectAddressIPv4IsCompleteIPSpace02);
    UtRegisterTest("DetectAddressIPv4IsCompleteIPSpace03",
                   DetectAddressIPv4IsCompleteIPSpace03);
    UtRegisterTest("DetectAddressIPv4IsCompleteIPSpace04",
                   DetectAddressIPv4IsCompleteIPSpace04);
    UtRegisterTest("DetectAddressIPv4CutNot05", DetectAddressIPv4CutNot05);
    UtRegisterTest("DetectAddressIPv4CutNot06", DetectAddressIPv4CutNot06);
    UtRegisterTest("DetectAddressIPv4CutNot07", DetectAddressIPv4CutNot07);
    UtRegisterTest("DetectAddressIPv4CutNot08", DetectAddressIPv4CutNot08);
    UtRegisterTest("DetectAddressIPv4CutNot09", DetectAddressIPv4CutNot09);
    UtRegisterTest("DetectAddressIPv4Join10", DetectAddressIPv4Join10);
#endif
}
