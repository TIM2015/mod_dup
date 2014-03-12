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
#include "Utils.hh"

#include <boost/shared_ptr.hpp>
#include <http_config.h>

namespace DupModule {

const unsigned int CMaxBytes = 8192;

bool
extractBrigadeContent(apr_bucket_brigade *bb, request_rec *pRequest, std::string &content) {
    if (ap_get_brigade(pRequest->input_filters,
                       bb, AP_MODE_READBYTES, APR_BLOCK_READ, CMaxBytes) != APR_SUCCESS) {
      Log::error(42, "Get brigade failed, skipping the rest of the body");
      return true;
    }

    // Read brigade content
    for (apr_bucket *b = APR_BRIGADE_FIRST(bb);
	 b != APR_BRIGADE_SENTINEL(bb);
	 b = APR_BUCKET_NEXT(b) ) {
      // Metadata end of stream
      if (APR_BUCKET_IS_EOS(b) || APR_BUCKET_IS_METADATA(b) ) {
		return true;
      }
      const char *data = 0;
      apr_size_t len = 0;
      apr_status_t rv = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
      if (rv != APR_SUCCESS) {
	Log::error(42, "Bucket read failed, skipping the rest of the body");
	return true;
      }
      if (len) {
		content.append(data, len);
      }
    }
    return false;
}

/*
 * Translate_name level HOOK
 *   - Read the body of a request.
 *   - Analyse request (body and header) with a synchroneous call on RequestProcessor::enrichContext method
 *   - If enrichment criteria are satisfied, the request context is enriched
 *   - This context can be used by mod_rewrite to redirect/modify the request
 *   - A RequestInfo object (containing the body) is stored in the request_config array. This object
 *      will be used by the inputfilter named 'inputFilterBody2Brigade' that reinjects the body to the input filters that follow him
 *   - A shared pointer manages the requestcontext object lifespan.
 */
int
translateHook(request_rec *pRequest) {
    if (!pRequest->per_dir_config)
        return DECLINED;
    DupConf *tConf = reinterpret_cast<DupConf *>(ap_get_module_config(pRequest->per_dir_config, &dup_module));
    if (!tConf || !tConf->dirName) {
        // Not a location that we treat, we decline the request
        return DECLINED;
    }
    unsigned int lReqID = DupModule::getNextReqId();
    std::string reqId = boost::lexical_cast<std::string>(lReqID);
    RequestInfo *info = new RequestInfo(reqId);
    // Allocation on a shared pointer on the request pool
    // We guarantee that whatever happens, the RequestInfo will be deleted
    void *space = apr_palloc(pRequest->pool, sizeof(boost::shared_ptr<RequestInfo>));
    new (space) boost::shared_ptr<RequestInfo>(info);
    // Registering of the shared pointer destructor on the pool
    apr_pool_cleanup_register(pRequest->pool, space, cleaner<boost::shared_ptr<RequestInfo> >,
                              apr_pool_cleanup_null);
    // Backup in request context
    ap_set_module_config(pRequest->request_config, &dup_module, (void *)space);

    if (!pRequest->connection->pool) {
        Log::error(42, "No connection pool associated to the request");
        return DECLINED;
    }
    if (!pRequest->connection->bucket_alloc) {
        pRequest->connection->bucket_alloc = apr_bucket_alloc_create(pRequest->connection->pool);
        if (!pRequest->connection->bucket_alloc) {
            Log::error(42, "Request bucket allocation failed");
            return DECLINED;
        }
    }

    apr_bucket_brigade *bb = apr_brigade_create(pRequest->connection->pool, pRequest->connection->bucket_alloc);
    if (!bb) {
        Log::error(42, "Bucket brigade allocation failed");
        return DECLINED;
    }
    while (!extractBrigadeContent(bb, pRequest, info->mBody)){
        apr_brigade_cleanup(bb);
    }
    apr_brigade_cleanup(bb);
    // Body read :)

    const char* lID = apr_table_get(pRequest->headers_in, c_UNIQUE_ID);
    // Copy Request ID in both headers
    //std::string reqId = boost::lexical_cast<std::string>(info->mId);
    if( lID == NULL){
        apr_table_set(pRequest->headers_in, c_UNIQUE_ID, info->mId.c_str());
        apr_table_set(pRequest->headers_out, c_UNIQUE_ID, info->mId.c_str());
    }
    else
    {
        apr_table_set(pRequest->headers_out, c_UNIQUE_ID, lID);
    }

    // Synchronous context enrichment
    info->mConfPath = tConf->dirName;
    info->mArgs = pRequest->args ? pRequest->args : "";
    gProcessor->enrichContext(pRequest, *info);

    return DECLINED;
}


/**
 * InputFilter which target is to fill the brigade passed as an argument with
 * the body read in the previous hook: 'translateName'
 */
apr_status_t
inputFilterBody2Brigade(ap_filter_t *pF, apr_bucket_brigade *pB, ap_input_mode_t pMode, apr_read_type_e pBlock, apr_off_t pReadbytes)
{
  if (pMode != AP_MODE_READBYTES) {
    return ap_get_brigade(pF->next, pB, pMode, pBlock, pReadbytes);
  }

    request_rec *pRequest = pF->r;
    if (!pRequest || !pRequest->per_dir_config) {
        apr_bucket *e = apr_bucket_eos_create(pF->c->bucket_alloc);
        assert(e);
        APR_BRIGADE_INSERT_TAIL(pB, e);
        return APR_SUCCESS;
    }
    // Retrieve request info from context
    boost::shared_ptr<RequestInfo> *shPtr = reinterpret_cast<boost::shared_ptr<RequestInfo> *>(ap_get_module_config(pRequest->request_config, &dup_module));
    if (!shPtr) {
        // Should not happen
        apr_bucket *e = apr_bucket_eos_create(pF->c->bucket_alloc);
        assert(e);
        APR_BRIGADE_INSERT_TAIL(pB, e);
        return APR_SUCCESS;
    }
    RequestInfo *info = shPtr->get();
    if (!pF->ctx) {
        // No context associated with the filter, we serve the body
        if (!info->mBody.empty()) {
            apr_status_t st;
            if ((st = apr_brigade_write(pB, NULL, NULL, info->mBody.c_str(), info->mBody.size())) != APR_SUCCESS ) {
                Log::warn(1, "Failed to write request body in a brigade: %s",  info->mBody.c_str());
                return st;
            }
        }
        // Marks that we do not have any more data to read
        pF->ctx = (void *) -1;
    }
    // Sending EOS to end the different calls to this filter
    apr_bucket *e = apr_bucket_eos_create(pF->c->bucket_alloc);
    assert(e);
    APR_BRIGADE_INSERT_TAIL(pB, e);
    return APR_SUCCESS;
}

/*
 * Callback to iterate over the headers tables
 * Pushes a copy of key => value in a list passed without typing as the first argument
 */
static int iterateOverHeadersCallBack(void *d, const char *key, const char *value) {
    RequestInfo::tHeaders *headers = reinterpret_cast<RequestInfo::tHeaders *>(d);
    headers->push_back(std::pair<std::string, std::string>(key, value));
    return 1;
}

static void
prepareRequestInfo(DupConf *tConf, request_rec *pRequest, RequestInfo &r) {
    // Basic
    r.mPoison = false;
    r.mConfPath = tConf->dirName;
    r.mPath = pRequest->uri;
    r.mArgs = pRequest->args ? pRequest->args : "";

    // Copy headers in
    apr_table_do(&iterateOverHeadersCallBack, &r.mHeadersIn, pRequest->headers_in, NULL);
}

static void
printRequest(request_rec *pRequest, RequestInfo *pBH, DupConf *tConf) {
    const char *reqId = apr_table_get(pRequest->headers_in, c_UNIQUE_ID);
    Log::debug("### Pushing a request with ID: %s, body size:%ld", reqId, pBH->mBody.size());
    Log::debug("### Uri:%s, dir name:%s", pRequest->uri, tConf->dirName);
    Log::debug("### Request args: %s", pRequest->args);
}

/**
 * Output Body filter handler
 * Writes the response body to the RequestInfo
 * Unless not needed because we only duplicate request and no reponses
 */
apr_status_t
outputBodyFilterHandler(ap_filter_t *pFilter, apr_bucket_brigade *pBrigade) {
    request_rec *pRequest = pFilter->r;
    // Reject requests that do not meet our requirements
    if ( pFilter->ctx == (void *) -1 ) {
      return ap_pass_brigade(pFilter->next, pBrigade);
    }

    if (!pRequest || !pRequest->per_dir_config) {
      ap_remove_output_filter(pFilter);
      return ap_pass_brigade(pFilter->next, pBrigade);
    }

    struct DupConf *tConf = reinterpret_cast<DupConf *>(ap_get_module_config(pRequest->per_dir_config, &dup_module));
    if ((!tConf) || (!tConf->dirName) || (tConf->getHighestDuplicationType() == DuplicationType::NONE)) {
      ap_remove_output_filter(pFilter);
         return ap_pass_brigade(pFilter->next, pBrigade);
    }

    // We need to get the highest one as we haven't matched which rule it is yet
    if (tConf->getHighestDuplicationType() != DuplicationType::REQUEST_WITH_ANSWER) {
        // No need to get the brigades here if we don't forward the answer
        pFilter->ctx = (void *) -1;
	ap_remove_output_filter(pFilter);
        return ap_pass_brigade(pFilter->next, pBrigade);
    }


    boost::shared_ptr<RequestInfo> * reqInfo(reinterpret_cast<boost::shared_ptr<RequestInfo> *>(ap_get_module_config(pFilter->r->request_config, &dup_module)));
    assert(reqInfo);
    RequestInfo * ri = reqInfo->get();
    assert(ri);
    
    // Pushing the answer to the processor      
    prepareRequestInfo(tConf, pRequest, *ri);

    // Write the response body to the RequestInfo if found
    apr_bucket *currentBucket;
    for ( currentBucket = APR_BRIGADE_FIRST(pBrigade); currentBucket != APR_BRIGADE_SENTINEL(pBrigade); currentBucket = APR_BUCKET_NEXT(currentBucket) ) { 
      if (APR_BUCKET_IS_EOS(currentBucket)) {
	ap_remove_output_filter(pFilter);
	pFilter->ctx = (void *) -1;
	continue;
      }
      else if ( APR_BUCKET_IS_METADATA(currentBucket) ) {
	continue;
      }

      const char *data;
      apr_size_t len;
      apr_status_t rv;
      rv = apr_bucket_read(currentBucket, &data, &len, APR_BLOCK_READ);
      
      if ((rv == APR_SUCCESS) && (data != NULL)) {
	ri->mAnswer.append(data, len);
      }
    }

    return ap_pass_brigade(pFilter->next, pBrigade);
}

/**
 * Output filter handler
 * Retrieves in/out headers
 * Pushes the RequestInfo object to the RequestProcessor
 */
apr_status_t
outputHeadersFilterHandler(ap_filter_t *pFilter, apr_bucket_brigade *pBrigade) {
  if ( pFilter->ctx == (void *) -1 ) {
    return ap_pass_brigade(pFilter->next, pBrigade);
  }
 
    request_rec *pRequest = pFilter->r;
    // Reject requests that do not meet our requirements
    if (!pRequest) {
      ap_remove_output_filter(pFilter);
      return ap_pass_brigade(pFilter->next, pBrigade);
    }
    if (!pRequest->per_dir_config ) {
      ap_remove_output_filter(pFilter);
      return ap_pass_brigade(pFilter->next, pBrigade);
    }

    struct DupConf *tConf = reinterpret_cast<DupConf *>(ap_get_module_config(pRequest->per_dir_config, &dup_module));
    if ((!tConf) || (!tConf->dirName) || (tConf->getHighestDuplicationType() == DuplicationType::NONE)) {
      ap_remove_output_filter(pFilter);
      return ap_pass_brigade(pFilter->next, pBrigade);
    }


    boost::shared_ptr<RequestInfo> * reqInfo(reinterpret_cast<boost::shared_ptr<RequestInfo> *>(ap_get_module_config(pFilter->r->request_config, &dup_module)));
    assert(reqInfo);
    RequestInfo * ri = reqInfo->get();
    assert(ri);

    // Copy headers out
    apr_table_do(&iterateOverHeadersCallBack, &ri->mHeadersOut, pRequest->headers_out, NULL);
    printRequest(pRequest, ri, tConf);
    
    
    if ( tConf->synchronous ) {
        static __thread CURL * lCurl = NULL;
        if ( ! lCurl ) {
            lCurl = gProcessor->initCurl(); 
        }
        gProcessor->runOne(*ri, lCurl);
    }
    else {
        gThreadPool->push(*reqInfo);
    }

    pFilter->ctx = (void *) -1;
    ap_remove_output_filter(pFilter);

    return ap_pass_brigade(pFilter->next, pBrigade);

}


};
