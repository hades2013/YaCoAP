#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include "coap.h"

/* private stuff */
typedef union {
    uint8_t raw;
    struct {
        uint8_t tkl     : 4;
        uint8_t t       : 2;
        uint8_t ver     : 2;
        uint8_t code;
        uint16_t id;
    } hdr;
} coap_raw_header_t;

static int _parse_token(coap_packet_t *pkt,
                            const uint8_t *buf, const size_t buflen);
static int _parse_header(coap_header_t *hdr,
                             const uint8_t *buf, const size_t buflen);
static int _parse_options_payload(coap_packet_t *pkt,
                                      const uint8_t *buf, size_t buflen);
static int _parse_option(coap_option_t *option, uint16_t *running_delta,
                             const uint8_t **buf, size_t buflen);
static const coap_option_t *_find_options(const coap_packet_t *pkt,
                                              coap_option_num_t num,
                                              uint8_t *count);
static void _option_nibble(uint32_t value, uint8_t *nibble);
static void _dump_header(const coap_header_t *hdr);
static void _dump_options(const coap_option_t *opts, const size_t numopt);

#ifdef MICROCOAP_DEBUG
static void _dump_header(const coap_header_t *hdr)
{
    printf("Header:\n");
    printf("  ver  0x%02X\n", hdr->ver);
    printf("  t    0x%02X\n", hdr->t);
    printf("  tkl  0x%02X\n", hdr->tkl);
    printf("  code 0x%02X\n", hdr->code);
    printf("  id   0x%04X\n", hdr->id);
}

static void _dump_options(const coap_option_t *opts, const size_t numopt)
{
    printf(" Options:\n");
    for (size_t i = 0; i < numopt; ++i) {
        printf("  0x%02X [ ", opts[i].num);
        coap_dump(opts[i].buf.p, opts[i].buf.len, true);
        printf(" ]\n");
    }
}
#endif /* MICROCOAP_DEBUG */

static int _parse_header(coap_header_t *hdr,
                             const uint8_t *buf, const size_t buflen)
{
    if (buflen < sizeof(coap_raw_header_t)) {
        return COAP_ERR_HEADER_TOO_SHORT;
    }
    coap_raw_header_t *r = (coap_raw_header_t*)buf;
    /* parse header from raw buffer */
    hdr->ver = r->hdr.ver;
    hdr->t = r->hdr.t;
    hdr->tkl = r->hdr.tkl;
    hdr->code = r->hdr.code;
    hdr->id = r->hdr.id;
    if (hdr->ver != 1) {
        return COAP_ERR_VERSION_NOT_1;
    }
    return COAP_SUCCESS;
}

static int _parse_token(coap_packet_t *pkt,
                            const uint8_t *buf, const size_t buflen)
{
    coap_buffer_t *tok = &pkt->tok;
    int toklen = pkt->hdr.tkl;
    /* validate the token length */
    if (sizeof(coap_raw_header_t) + toklen > buflen || toklen > 8) {
        return COAP_ERR_TOKEN_TOO_SHORT;
    }
    tok->len = toklen;
    if (!toklen) {
        tok->p = NULL;
    }
    else {
        tok->p = buf + sizeof(coap_raw_header_t);
    }
    return COAP_SUCCESS;
}

static int _parse_option(coap_option_t *option, uint16_t *running_delta,
                             const uint8_t **buf, size_t buflen)
{
    const uint8_t *p = *buf;
    uint8_t headlen = 1;
    uint16_t len, delta;

    if (buflen < headlen) {
        return COAP_ERR_OPTION_TOO_SHORT_FOR_HEADER;
    }
    delta = (p[0] & 0xF0) >> 4;
    len = p[0] & 0x0F;

    /* FIXME: untested and may be buggy */
    if (delta == 13) {
        headlen++;
        if (buflen < headlen)
            return COAP_ERR_OPTION_TOO_SHORT_FOR_HEADER;
        delta = p[1] + 13;
        p++;
    }
    else if (delta == 14) {
        headlen += 2;
        if (buflen < headlen) {
            return COAP_ERR_OPTION_TOO_SHORT_FOR_HEADER;
        }
        delta = ((p[1] << 8) | p[2]) + 269;
        p+=2;
    }
    else if (delta == 15) {
        return COAP_ERR_OPTION_DELTA_INVALID;
    }

    if (len == 13) {
        headlen++;
        if (buflen < headlen) {
            return COAP_ERR_OPTION_TOO_SHORT_FOR_HEADER;
        }
        len = p[1] + 13;
        p++;
    }
    else if (len == 14) {
        headlen += 2;
        if (buflen < headlen) {
            return COAP_ERR_OPTION_TOO_SHORT_FOR_HEADER;
        }
        len = ((p[1] << 8) | p[2]) + 269;
        p += 2;
    }
    else if (len == 15) {
        return COAP_ERR_OPTION_LEN_INVALID;
    }

    if ((p + 1 + len) > (*buf + buflen)) {
        return COAP_ERR_OPTION_TOO_BIG;
    }
    /* set option header */
    option->num = delta + *running_delta;
    option->buf.p = p+1;
    option->buf.len = len;
    /* advance buffer cursor */
    *buf = p + 1 + len;
    *running_delta += delta;

    return COAP_SUCCESS;
}

// http://tools.ietf.org/html/rfc7252#section-3.1
static int _parse_options_payload(coap_packet_t *pkt,
                                      const uint8_t *buf, size_t buflen)
{
    size_t optionIndex = 0;
    uint16_t delta = 0;
    const uint8_t *p = buf + sizeof(coap_raw_header_t) + pkt->hdr.tkl;
    const uint8_t *end = buf + buflen;
    int rc;
    if (p > end) {
        return COAP_ERR_OPTION_OVERRUNS_PACKET;
    }

    /* Note: 0xFF is payload marker */
    while ((optionIndex < MAXOPT) && (p < end) && (*p != 0xFF)) {
        rc = _parse_option(&pkt->opts[optionIndex], &delta, &p, end - p);
        if(rc) {
            return rc;
        }
        optionIndex++;
    }
    pkt->numopts = optionIndex;

    if ((p + 1) < end && *p == 0xFF) {
        pkt->payload.p = p + 1;
        pkt->payload.len = end - (p + 1);
    }
    else {
        pkt->payload.p = NULL;
        pkt->payload.len = 0;
    }
    return COAP_SUCCESS;
}

/*
 * options are always stored consecutively,
 * so can return a block with same option num
 */
static const coap_option_t *_find_options(const coap_packet_t *pkt,
                                              coap_option_num_t num,
                                              uint8_t *count)
{
    const coap_option_t * first = NULL;
    /* loop through packet opts */
    *count = 0;
    for (size_t i = 0; i < pkt->numopts; ++i) {
        if (pkt->opts[i].num == num) {
            if (!first) {
                first = &pkt->opts[i];
            }
            (*count)++;
        }
        /* options are ordered by num, skip if greater */
        else if (pkt->opts[i].num > num) {
            break;
        }
        /* single block for same option num, skip on first match */
        else if (first) {
            break;
        }
    }
    return first;
}

/* https://tools.ietf.org/html/rfc7252#section-3.1 */
static void _option_nibble(uint32_t value, uint8_t *nibble)
{
    if (value < 13) {
        *nibble = (0xFF & value);
    }
    else if (value <= 0xFF+13) {
        *nibble = 13;
    }
    else if (value <= 0xFFFF+269) {
        *nibble = 14;
    }
}

#ifdef MICROCOAP_DEBUG
void coap_dump(const uint8_t *buf, size_t buflen, bool bare)
{
    if (bare) {
        while(buflen--) {
            printf("%02X%s", *buf++, (buflen > 0) ? " " : "");
        }
    }
    else {
        printf("Dump: ");
        while(buflen--) {
            printf("%02X%s", *buf++, (buflen > 0) ? " " : "");
        }
        printf("\n");
    }
}

void coap_dump_packet(const coap_packet_t *pkt)
{
    _dump_header(&pkt->hdr);
    _dump_options(pkt->opts, pkt->numopts);
    printf("Payload: ");
    coap_dump(pkt->payload.p, pkt->payload.len, true);
    printf("\n");
}
#endif /* MICROCOAP_DEBUG */

int coap_parse(coap_packet_t *pkt, const uint8_t *buf, size_t buflen)
{
    int rc;
    /* parse header, token, options, and payload */
    rc = _parse_header(&pkt->hdr, buf, buflen);
    if(rc) {
        return rc;
    }
    rc = _parse_token(pkt, buf, buflen);
    if(rc) {
        return rc;
    }
    pkt->numopts = MAXOPT;
    rc = _parse_options_payload(pkt, buf, buflen);
    if(rc) {
        return rc;
    }
    return COAP_SUCCESS;
}

int coap_build(uint8_t *buf, size_t *buflen, const coap_packet_t *pkt)
{
    // build header
    if (*buflen < (sizeof(coap_raw_header_t) + pkt->hdr.tkl)) {
        return COAP_ERR_BUFFER_TOO_SMALL;
    }
    coap_raw_header_t *r = (coap_raw_header_t *)buf;
    r->hdr.ver = pkt->hdr.ver;
    r->hdr.t = pkt->hdr.t;
    r->hdr.tkl = pkt->hdr.tkl;
    r->hdr.code = pkt->hdr.code;
    r->hdr.id = pkt->hdr.id;
    // inject token
    uint8_t *p = buf + sizeof(coap_raw_header_t);
    if ((pkt->hdr.tkl > 0) && (pkt->hdr.tkl != pkt->tok.len)) {
        return COAP_ERR_UNSUPPORTED;
    }
    if (pkt->hdr.tkl > 0) {
        memcpy(p, pkt->tok.p, pkt->hdr.tkl);
    }
    p += pkt->hdr.tkl;
    // inject options, http://tools.ietf.org/html/rfc7252#section-3.1
    uint16_t running_delta = 0;
    for (size_t i = 0; i < pkt->numopts; ++i) {
        if (((size_t)(p - buf)) > *buflen) {
            return COAP_ERR_BUFFER_TOO_SMALL;
        }
        uint32_t optDelta = pkt->opts[i].num - running_delta;
        uint8_t delta = 0;
        _option_nibble(optDelta, &delta);
        uint8_t len = 0;
        _option_nibble((uint32_t)pkt->opts[i].buf.len, &len);

        *p++ = (0xFF & (delta << 4 | len));
        if (delta == 13) {
            *p++ = (optDelta - 13);
        }
        else if (delta == 14) {
            *p++ = ((optDelta-269) >> 8);
            *p++ = (0xFF & (optDelta-269));
        }
        if (len == 13) {
            *p++ = (pkt->opts[i].buf.len - 13);
        }
        else if (len == 14) {
            *p++ = (pkt->opts[i].buf.len >> 8);
            *p++ = (0xFF & (pkt->opts[i].buf.len-269));
        }

        memcpy(p, pkt->opts[i].buf.p, pkt->opts[i].buf.len);
        p += pkt->opts[i].buf.len;
        running_delta = pkt->opts[i].num;
    }
    // calc number of bytes used by options
    size_t opts_len = (p - buf) - sizeof(coap_raw_header_t);
    if (pkt->payload.len > 0) {
        if (*buflen < sizeof(coap_raw_header_t) + 1 + pkt->payload.len + opts_len) {
            return COAP_ERR_BUFFER_TOO_SMALL;
        }
        buf[sizeof(coap_raw_header_t) + opts_len] = 0xFF;  // payload marker
        memcpy(buf + sizeof(coap_raw_header_t) + opts_len + 1,
               pkt->payload.p, pkt->payload.len);
        *buflen = sizeof(coap_raw_header_t) + opts_len + 1 + pkt->payload.len;
    }
    else {
        *buflen = opts_len + sizeof(coap_raw_header_t);
    }
    return COAP_SUCCESS;
}

int coap_make_response(coap_rw_buffer_t *scratch, coap_packet_t *pkt,
                       const uint8_t *content, size_t content_len,
                       uint16_t msgid, const coap_buffer_t* tok,
                       coap_responsecode_t rspcode,
                       coap_content_type_t content_type)
{
    pkt->hdr.ver = 0x01;
    pkt->hdr.t = COAP_TYPE_ACK;
    pkt->hdr.tkl = 0;
    pkt->hdr.code = rspcode;
    pkt->hdr.id = msgid;
    pkt->numopts = 1;
    // need token in response
    if (tok) {
        pkt->hdr.tkl = tok->len;
        pkt->tok = *tok;
    }
    // safe because 1 < MAXOPT
    pkt->opts[0].num = COAP_OPTION_CONTENT_FORMAT;
    pkt->opts[0].buf.p = scratch->p;
    if (scratch->len < 2) {
        return COAP_ERR_BUFFER_TOO_SMALL;
    }
    scratch->p[0] = ((uint16_t)content_type & 0xFF00) >> 8;
    scratch->p[1] = ((uint16_t)content_type & 0x00FF);
    pkt->opts[0].buf.len = 2;
    pkt->payload.p = content;
    pkt->payload.len = content_len;
    return COAP_SUCCESS;
}

int coap_handle_request(const coap_endpoint_t *endpoints,
                        coap_rw_buffer_t *scratch,
                        const coap_packet_t *inpkt, coap_packet_t *outpkt)
{
    uint8_t count;
    const coap_option_t *opt = _find_options(inpkt, COAP_OPTION_URI_PATH, &count);
    // find handler for requested endpoint
    for (const coap_endpoint_t *ep = endpoints; ep->handler && opt; ++ep) {
        if ((ep->method == inpkt->hdr.code) && (count == ep->path->count)){
            int i;
            for (i = 0; i < count; i++) {
                if (opt[i].buf.len != strlen(ep->path->elems[i])) {
                    break;
                }
                if (memcmp(ep->path->elems[i], opt[i].buf.p, opt[i].buf.len)) {
                    break;
                }
            }
            if (i == count) {
                return ep->handler(scratch, inpkt, outpkt, inpkt->hdr.id);
            }
        }
    }
    coap_make_response(scratch, outpkt, NULL, 0, inpkt->hdr.id, &inpkt->tok,
                       COAP_RSPCODE_NOT_FOUND, COAP_CONTENTTYPE_NONE);
    return COAP_SUCCESS;
}

int coap_build_endpoints(const coap_endpoint_t *endpoints, char *buf, size_t buflen)
{
    if (buflen < 4) { // <>;
        return COAP_ERR_BUFFER_TOO_SMALL;
    }
    memset(buf,0,buflen);
    // loop over endpoints
    int len = buflen - 1;
    for (const coap_endpoint_t *ep = endpoints; ep->handler; ++ep) {
        if (0 > len) {
            return COAP_ERR_BUFFER_TOO_SMALL;
        }
        // skip if missing content type
        if (ep->ct == COAP_CONTENTTYPE_NONE) {
            continue;
        }
        // comma separated list
        if (0 < strlen(buf)) {
            strncat(buf, ",", len);
            len--;
        }
        // insert < at path beginning
        strncat(buf, "<", len);
        len--;
        // insert path by elements
        for (int i = 0; i < ep->path->count; i++) {
            strncat(buf, "/", len);
            len--;

            strncat(buf, ep->path->elems[i], len);
            len -= strlen(ep->path->elems[i]);
        }
        // insert >; after path
        strncat(buf, ">;", len);
        len -= 2;
        // append content type
        len -= sprintf(buf + (buflen - len - 1), "ct=%d", (int)ep->ct);
    }
    return COAP_SUCCESS;
}
