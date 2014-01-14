/*
* mod_dup - duplicates apache requests
*
* Copyright (C) 2013 Orange
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "mod_dup.hh"

#include <http_config.h>

namespace DupModule {

apr_status_t
inputFilterHandler(ap_filter_t *pF, apr_bucket_brigade *pB, ap_input_mode_t pMode, apr_read_type_e pBlock, apr_off_t pReadbytes)
{
    apr_status_t lStatus = ap_get_brigade(pF->next, pB, pMode, pBlock, pReadbytes);
    if (lStatus != APR_SUCCESS) {
        return lStatus;
    }
    request_rec *pRequest = pF->r;
    if (pRequest) {
	struct DupConf *tConf = reinterpret_cast<DupConf *>(ap_get_module_config(pRequest->per_dir_config, &dup_module));
	if (!tConf) {
            return OK; // SHOULD NOT HAPPEN
	}
        // No context? new request
        if (!pF->ctx) {
            RequestInfo *info = new RequestInfo(tConf->getNextReqId());
            ap_set_module_config(pRequest->request_config, &dup_module, (void *)info);
            // Copy Request ID in both headers
            std::string reqId = boost::lexical_cast<std::string>(info->mId);
            apr_table_set(pRequest->headers_in, c_UNIQUE_ID, reqId.c_str());
            apr_table_set(pRequest->headers_out, c_UNIQUE_ID, reqId.c_str());
            // Backup of info struct in the request context
            pF->ctx = info;
        } else if (pF->ctx == (void *)1) {
            return OK;
        }
        RequestInfo *pBH = static_cast<RequestInfo *>(pF->ctx);
        for (apr_bucket *b = APR_BRIGADE_FIRST(pB);
             b != APR_BRIGADE_SENTINEL(pB);
             b = APR_BUCKET_NEXT(b) ) {
            // Metadata end of stream
            if ( APR_BUCKET_IS_EOS(b) ) {
                // Synchronous context enrichment
                gProcessor->enrichContext();
                pF->ctx = (void *)1;
                break;
            }
            if (DuplicationType::value != DuplicationType::HEADER_ONLY) {
                const char* lReqPart = NULL;
                apr_size_t lLength = 0;
                apr_status_t lStatus = apr_bucket_read(b, &lReqPart, &lLength, APR_BLOCK_READ);
                if ((lStatus != APR_SUCCESS) || (lReqPart == NULL)) {
                    continue;
                }
                pBH->mBody += std::string(lReqPart, lLength);
            }
        }
    }
    return OK;
}

/*
 * Request context used during brigade run
 */
class RequestContext {
public:
    apr_bucket_brigade  *mTmpBB;
    RequestInfo         *mReq; /** Req is pushed to requestprocessor for duplication and will be deleted later*/

    RequestContext(ap_filter_t *pFilter) {
        mTmpBB = apr_brigade_create(pFilter->r->pool, pFilter->c->bucket_alloc);
        mReq = reinterpret_cast<RequestInfo *>(ap_get_module_config(pFilter->r->request_config, &dup_module));
        assert(mReq);
    }

    ~RequestContext() {
        apr_brigade_cleanup(mTmpBB);
    }
};

/*
 * Callback to iterate over the headers tables
 * Pushes a copy of key => value in a list
 */
static int iterateOverHeadersCallBack(void *d, const char *key, const char *value) {
    RequestInfo::tHeaders *headers = reinterpret_cast<RequestInfo::tHeaders *>(d);
    headers->push_back(std::pair<std::string, std::string>(key, value));
    return 0;
}

static void
prepareRequestInfo(DupConf *tConf, request_rec *pRequest, RequestInfo &r, bool withAnswer) {
    // Basic
    r.mPoison = false;
    r.mConfPath = tConf->dirName;
    r.mPath = pRequest->uri;
    r.mArgs = pRequest->args ? pRequest->args : "";

    // Copy headers in
    apr_table_do(&iterateOverHeadersCallBack, &r.mHeadersIn, pRequest->headers_in, NULL);
    if (withAnswer) {
        // Copy headers out
        apr_table_do(&iterateOverHeadersCallBack, &r.mHeadersOut, pRequest->headers_out, NULL);
    }
}

static void
printRequest(request_rec *pRequest, RequestInfo *pBH, DupConf *tConf) {
    const char *reqId = apr_table_get(pRequest->headers_in, c_UNIQUE_ID);
    Log::debug("### Pushing a request with ID: %s, body size:%ld", reqId, pBH->mBody.size());
    Log::debug("### Uri:%s, dir name:%s", pRequest->uri, tConf->dirName);
    Log::debug("### Request args: %s", pRequest->args);
}

apr_status_t
outputFilterHandler(ap_filter_t *pFilter, apr_bucket_brigade *pBrigade) {
    request_rec *pRequest = pFilter->r;
    if (!pRequest || !pRequest->per_dir_config)
        return ap_pass_brigade(pFilter->next, pBrigade);
    struct DupConf *tConf = reinterpret_cast<DupConf *>(ap_get_module_config(pRequest->per_dir_config, &dup_module));
    assert(tConf);
    // Request answer analyse
    RequestContext *ctx = static_cast<RequestContext *>(pFilter->ctx);
    if (ctx == NULL) {
        // Context init
        ctx = new RequestContext(pFilter);
        pFilter->ctx = ctx;

    } else if (ctx == (void *) -1) {
        return ap_pass_brigade(pFilter->next, pBrigade);
    }

    if (DuplicationType::value != DuplicationType::REQUEST_WITH_ANSWER) {
        // Asynchronous push of request without the answer
        RequestInfo *rH = ctx->mReq;
        prepareRequestInfo(tConf, pRequest, *rH, false);
        printRequest(pRequest, rH, tConf);
        gThreadPool->push(rH);
        delete ctx;
        pFilter->ctx = (void *) -1;
        return ap_pass_brigade(pFilter->next, pBrigade);
    }
    // Asynchronous push of request WITH the answer
    apr_bucket *currentBucket;
    while ((currentBucket = APR_BRIGADE_FIRST(pBrigade)) != APR_BRIGADE_SENTINEL(pBrigade)) {
        const char *data;
        apr_size_t len;
        apr_status_t rv;
        rv = apr_bucket_read(currentBucket, &data, &len, APR_BLOCK_READ);

        if ((rv == APR_SUCCESS) && (data != NULL)) {
            ctx->mReq->mAnswer.append(data, len);
        }
        /* Remove bucket e from bb. */
        APR_BUCKET_REMOVE(currentBucket);
        /* Insert it into  temporary brigade. */
        APR_BRIGADE_INSERT_HEAD(ctx->mTmpBB, currentBucket);
        /* Pass brigade downstream. */
        rv = ap_pass_brigade(pFilter->next, ctx->mTmpBB);
        if (rv != APR_SUCCESS) {
            // Something went wrongm no duplication performed
            delete ctx;
            pFilter->ctx = (void *) -1;
            return rv;
        }
        if (APR_BUCKET_IS_EOS(currentBucket)) {
            // Pushing the answer to the processor
            prepareRequestInfo(tConf, pRequest, *(ctx->mReq), true);
            printRequest(pRequest, ctx->mReq, tConf);
            gThreadPool->push(ctx->mReq);
            delete ctx;
            pFilter->ctx = (void *) -1;
        }
        else {
            apr_brigade_cleanup(ctx->mTmpBB);
        }
    }
    return OK;
}

};
