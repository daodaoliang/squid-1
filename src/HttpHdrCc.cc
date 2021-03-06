/*
 * Copyright (C) 1996-2015 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 65    HTTP Cache Control Header */

#include "squid.h"
#include "HttpHdrCc.h"
#include "HttpHeader.h"
#include "HttpHeaderFieldStat.h"
#include "HttpHeaderStat.h"
#include "HttpHeaderTools.h"
#include "SBuf.h"
#include "StatHist.h"
#include "Store.h"
#include "StrList.h"
#include "util.h"

#include <map>

/* a row in the table used for parsing cache control header and statistics */
class HttpHeaderCcFields
{
public:
    HttpHeaderCcFields() : name(NULL), id(CC_BADHDR), stat() {}
    HttpHeaderCcFields(const char *aName, http_hdr_cc_type aTypeId) : name(aName), id(aTypeId) {}
    HttpHeaderCcFields(const HttpHeaderCcFields &f) : name(f.name), id(f.id) {}
    // nothing to do as name is a pointer to global static string
    ~HttpHeaderCcFields() {}

    const char *name;
    http_hdr_cc_type id;
    HttpHeaderFieldStat stat;

private:
    HttpHeaderCcFields &operator =(const HttpHeaderCcFields &); // not implemented
};

/* order must match that of enum http_hdr_cc_type. The constraint is verified at initialization time */
static HttpHeaderCcFields CcAttrs[CC_ENUM_END] = {
    HttpHeaderCcFields("public", CC_PUBLIC),
    HttpHeaderCcFields("private", CC_PRIVATE),
    HttpHeaderCcFields("no-cache", CC_NO_CACHE),
    HttpHeaderCcFields("no-store", CC_NO_STORE),
    HttpHeaderCcFields("no-transform", CC_NO_TRANSFORM),
    HttpHeaderCcFields("must-revalidate", CC_MUST_REVALIDATE),
    HttpHeaderCcFields("proxy-revalidate", CC_PROXY_REVALIDATE),
    HttpHeaderCcFields("max-age", CC_MAX_AGE),
    HttpHeaderCcFields("s-maxage", CC_S_MAXAGE),
    HttpHeaderCcFields("max-stale", CC_MAX_STALE),
    HttpHeaderCcFields("min-fresh", CC_MIN_FRESH),
    HttpHeaderCcFields("only-if-cached", CC_ONLY_IF_CACHED),
    HttpHeaderCcFields("stale-if-error", CC_STALE_IF_ERROR),
    HttpHeaderCcFields("Other,", CC_OTHER) /* ',' will protect from matches */
};

/// Map an header name to its type, to expedite parsing
typedef std::map<const SBuf,http_hdr_cc_type> CcNameToIdMap_t;
static CcNameToIdMap_t CcNameToIdMap;

/// used to walk a table of http_header_cc_type structs
http_hdr_cc_type &operator++ (http_hdr_cc_type &aHeader)
{
    int tmp = (int)aHeader;
    aHeader = (http_hdr_cc_type)(++tmp);
    return aHeader;
}

/// Module initialization hook
void
httpHdrCcInitModule(void)
{
    /* build lookup and accounting structures */
    for (int32_t i = 0; i < CC_ENUM_END; ++i) {
        const HttpHeaderCcFields &f=CcAttrs[i];
        assert(i == f.id); /* verify assumption: the id is the key into the array */
        const SBuf k(f.name);
        CcNameToIdMap[k]=f.id;
    }
}

/// Module cleanup hook.
void
httpHdrCcCleanModule(void)
{
    // HdrCcNameToIdMap is self-cleaning
}

void
HttpHdrCc::clear()
{
    *this=HttpHdrCc();
}

bool
HttpHdrCc::parse(const String & str)
{
    const char *item;
    const char *p;      /* '=' parameter */
    const char *pos = NULL;
    http_hdr_cc_type type;
    int ilen;
    int nlen;

    /* iterate through comma separated list */

    while (strListGetItem(&str, ',', &item, &ilen, &pos)) {
        /* isolate directive name */

        if ((p = (const char *)memchr(item, '=', ilen)) && (p - item < ilen)) {
            nlen = p - item;
            ++p;
        } else {
            nlen = ilen;
        }

        /* find type */
        const CcNameToIdMap_t::const_iterator i=CcNameToIdMap.find(SBuf(item,nlen));
        if (i==CcNameToIdMap.end())
            type=CC_OTHER;
        else
            type=i->second;

        // ignore known duplicate directives
        if (isSet(type)) {
            if (type != CC_OTHER) {
                debugs(65, 2, "hdr cc: ignoring duplicate cache-directive: near '" << item << "' in '" << str << "'");
                ++CcAttrs[type].stat.repCount;
                continue;
            }
        }

        /* special-case-parsing and attribute-setting */
        switch (type) {

        case CC_MAX_AGE:
            if (!p || !httpHeaderParseInt(p, &max_age) || max_age < 0) {
                debugs(65, 2, "cc: invalid max-age specs near '" << item << "'");
                clearMaxAge();
            } else {
                setMask(type,true);
            }
            break;

        case CC_S_MAXAGE:
            if (!p || !httpHeaderParseInt(p, &s_maxage) || s_maxage < 0) {
                debugs(65, 2, "cc: invalid s-maxage specs near '" << item << "'");
                clearSMaxAge();
            } else {
                setMask(type,true);
            }
            break;

        case CC_MAX_STALE:
            if (!p || !httpHeaderParseInt(p, &max_stale) || max_stale < 0) {
                debugs(65, 2, "cc: max-stale directive is valid without value");
                maxStale(MAX_STALE_ANY);
            } else {
                setMask(type,true);
            }
            break;

        case CC_MIN_FRESH:
            if (!p || !httpHeaderParseInt(p, &min_fresh) || min_fresh < 0) {
                debugs(65, 2, "cc: invalid min-fresh specs near '" << item << "'");
                clearMinFresh();
            } else {
                setMask(type,true);
            }
            break;

        case CC_STALE_IF_ERROR:
            if (!p || !httpHeaderParseInt(p, &stale_if_error) || stale_if_error < 0) {
                debugs(65, 2, "cc: invalid stale-if-error specs near '" << item << "'");
                clearStaleIfError();
            } else {
                setMask(type,true);
            }
            break;

        case CC_PRIVATE: {
            String temp;
            if (!p)  {
                // Value parameter is optional.
                private_.clean();
            }            else if (/* p &&*/ httpHeaderParseQuotedString(p, (ilen-nlen-1), &temp)) {
                private_.append(temp);
            }            else {
                debugs(65, 2, "cc: invalid private= specs near '" << item << "'");
            }
            // to be safe we ignore broken parameters, but always remember the 'private' part.
            setMask(type,true);
        }
        break;

        case CC_NO_CACHE: {
            String temp;
            if (!p) {
                // On Requests, missing value parameter is expected syntax.
                // On Responses, value parameter is optional.
                setMask(type,true);
                no_cache.clean();
            } else if (/* p &&*/ httpHeaderParseQuotedString(p, (ilen-nlen-1), &temp)) {
                // On Requests, a value parameter is invalid syntax.
                // XXX: identify when parsing request header and dump err message here.
                setMask(type,true);
                no_cache.append(temp);
            } else {
                debugs(65, 2, "cc: invalid no-cache= specs near '" << item << "'");
            }
        }
        break;

        case CC_PUBLIC:
            Public(true);
            break;
        case CC_NO_STORE:
            noStore(true);
            break;
        case CC_NO_TRANSFORM:
            noTransform(true);
            break;
        case CC_MUST_REVALIDATE:
            mustRevalidate(true);
            break;
        case CC_PROXY_REVALIDATE:
            proxyRevalidate(true);
            break;
        case CC_ONLY_IF_CACHED:
            onlyIfCached(true);
            break;

        case CC_OTHER:
            if (other.size())
                other.append(", ");

            other.append(item, ilen);
            break;

        default:
            /* note that we ignore most of '=' specs (RFCVIOLATION) */
            break;
        }
    }

    return (mask != 0);
}

void
HttpHdrCc::packInto(Packer * p) const
{
    // optimization: if the mask is empty do nothing
    if (mask==0)
        return;

    http_hdr_cc_type flag;
    int pcount = 0;
    assert(p);

    for (flag = CC_PUBLIC; flag < CC_ENUM_END; ++flag) {
        if (isSet(flag) && flag != CC_OTHER) {

            /* print option name for all options */
            packerPrintf(p, (pcount ? ", %s": "%s") , CcAttrs[flag].name);

            /* for all options having values, "=value" after the name */
            switch (flag) {
            case CC_MAX_AGE:
                packerPrintf(p, "=%d", (int) maxAge());
                break;
            case CC_S_MAXAGE:
                packerPrintf(p, "=%d", (int) sMaxAge());
                break;
            case CC_MAX_STALE:
                /* max-stale's value is optional.
                  If we didn't receive it, don't send it */
                if (maxStale()!=MAX_STALE_ANY)
                    packerPrintf(p, "=%d", (int) maxStale());
                break;
            case CC_MIN_FRESH:
                packerPrintf(p, "=%d", (int) minFresh());
                break;
            default:
                /* do nothing, directive was already printed */
                break;
            }

            ++pcount;
        }
    }

    if (other.size() != 0)
        packerPrintf(p, (pcount ? ", " SQUIDSTRINGPH : SQUIDSTRINGPH),
                     SQUIDSTRINGPRINT(other));
}

void
httpHdrCcUpdateStats(const HttpHdrCc * cc, StatHist * hist)
{
    http_hdr_cc_type c;
    assert(cc);

    for (c = CC_PUBLIC; c < CC_ENUM_END; ++c)
        if (cc->isSet(c))
            hist->count(c);
}

void
httpHdrCcStatDumper(StoreEntry * sentry, int, double val, double, int count)
{
    extern const HttpHeaderStat *dump_stat; /* argh! */
    const int id = (int) val;
    const int valid_id = id >= 0 && id < CC_ENUM_END;
    const char *name = valid_id ? CcAttrs[id].name : "INVALID";

    if (count || valid_id)
        storeAppendPrintf(sentry, "%2d\t %-20s\t %5d\t %6.2f\n",
                          id, name, count, xdiv(count, dump_stat->ccParsedCount));
}

#if !_USE_INLINE_
#include "HttpHdrCc.cci"
#endif

