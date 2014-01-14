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

#include <httpd.h>
#include <http_config.h>
#include <http_request.h>
#include <http_protocol.h>
#include <http_connection.h>
#include <apr_pools.h>
#include <apr_hooks.h>
#include "apr_strings.h"
#include <unistd.h>
#include <curl/curl.h>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <exception>
#include <set>
#include <sstream>
#include <sys/syscall.h>

#include "mod_dup.hh"

namespace alg = boost::algorithm;


namespace DupModule {

RequestProcessor *gProcessor;
ThreadPool<const RequestInfo*> *gThreadPool;


const char *gName = "Dup";
const char *c_COMPONENT_VERSION = "Dup/1.0";
const char* c_UNIQUE_ID = "UNIQUE_ID";

namespace DuplicationType {

    const char* c_HEADER_ONLY =                 "HEADER_ONLY";
    const char* c_COMPLETE_REQUEST =            "COMPLETE_REQUEST";
    const char* c_REQUEST_WITH_ANSWER =         "REQUEST_WITH_ANSWER";
    const char* c_ERROR_ON_STRING_VALUE =       "Invalid Duplication Type Value. Supported Values: HEADER_ONLY | COMPLETE_REQUEST | REQUEST_WITH_ANSWER" ;

    eDuplicationType stringToEnum(const char *value) throw (std::exception){
        if (!strcmp(value, c_HEADER_ONLY)) {
            return HEADER_ONLY;
        }
        if (!strcmp(value, c_COMPLETE_REQUEST)) {
            return COMPLETE_REQUEST;
        }
        if (!strcmp(value, c_REQUEST_WITH_ANSWER)) {
            return REQUEST_WITH_ANSWER;
        }
        throw std::exception();
    }

    eDuplicationType value = HEADER_ONLY;
}

DupConf::DupConf()
    : currentApplicationScope(ApplicationScope::HEADER)
    , dirName(NULL) {
    srand(time(NULL));
}

unsigned int DupConf::getNextReqId() {
    // Thread-local static variables
    // Makes sure the random pattern/sequence is different for each thread
    static __thread bool lInitialized = false;
    static __thread struct random_data lRD = { 0, 0, 0, 0, 0, 0, 0} ;
    static __thread char lRSB[8];

    // Initialized per thread
    int lRet = 0;
    if (!lInitialized) {
        memset(lRSB,0, 8);
        struct timespec lTimeSpec;
        clock_gettime(CLOCK_MONOTONIC, &lTimeSpec);
        // The seed is randomized using thread ID and nanoseconds
        unsigned int lSeed = lTimeSpec.tv_nsec + (pid_t) syscall(SYS_gettid);

        // init State must be different for all threads or each will answer the same sequence
        lRet |= initstate_r(lSeed, lRSB, 8, &lRD);
        lInitialized = true;
    }
    // Thread-safe calls with thread local initialization
    int lRandNum = 1;
    lRet |= random_r(&lRD, &lRandNum);
    if (lRet)
        Log::error(5, "Error on number randomisation");
    return lRandNum;
}

apr_status_t DupConf::cleaner(void *self) {
    if (self) {
        DupConf *c = reinterpret_cast<DupConf *>(self);
        c->~DupConf();
    }
    return 0;
}

/**
 * @brief allocate a pointer to a string which will hold the path for the dir config if mod_dup is active on it
 * @param pPool the apache pool on which to allocate data
 * @param pDirName the directory name for which to create data
 * @return a void pointer to newly allocated object
 */
void *
createDirConfig(apr_pool_t *pPool, char *pDirName)
{
    void *addr= apr_pcalloc(pPool, sizeof(struct DupConf));
    new (addr) DupConf();
    apr_pool_cleanup_register(pPool, addr, DupConf::cleaner,  NULL);
    return addr;
}

/**
 * @brief Initialize the processor and thread pool pre-config
 * @param pPool the apache pool
 * @return Always OK
 */
int
preConfig(apr_pool_t * pPool, apr_pool_t * pLog, apr_pool_t * pTemp) {
    gProcessor = new RequestProcessor();
    gThreadPool = new ThreadPool<const RequestInfo *>(boost::bind(&RequestProcessor::run, gProcessor, _1), &POISON_REQUEST);
    // Add the request timeout stat provider. Compose the lexical_cast with getTimeoutCount so that the resulting stat provider returns a string
    gThreadPool->addStat("#TmOut", boost::bind(boost::lexical_cast<std::string, unsigned int>,
                                               boost::bind(&RequestProcessor::getTimeoutCount, gProcessor)));
    gThreadPool->addStat("#DupReq", boost::bind(boost::lexical_cast<std::string, unsigned int>,
                                                boost::bind(&RequestProcessor::getDuplicatedCount, gProcessor)));
    return OK;
}

/**
 * @brief Initialize logging post-config
 * @param pPool the apache pool
 * @param pServer the corresponding server record
 * @return Always OK
 */
int
postConfig(apr_pool_t * pPool, apr_pool_t * pLog, apr_pool_t * pTemp, server_rec * pServer) {
    Log::init();

    ap_add_version_component(pPool, c_COMPONENT_VERSION) ;
    return OK;
}

/**
 * @brief Set the program name used in the stats messages
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pName the name to be used
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setName(cmd_parms* pParams, void* pCfg, const char* pName) {
    if (!pName || strlen(pName) == 0) {
        return "Missing program name";
    }
    gThreadPool->setProgramName(pName);
    return NULL;
}

/**
 * @brief Set the url enc/decoding style
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pUrlCodec the url enc/decoding style to use
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setUrlCodec(cmd_parms* pParams, void* pCfg, const char* pUrlCodec) {
    if (!pUrlCodec || strlen(pUrlCodec) == 0) {
        return "Missing url codec style";
    }
    gProcessor->setUrlCodec(pUrlCodec);
    return NULL;
}

/**
 * @brief Set the destination host and port
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pDestionation the destination in <host>[:<port>] format
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setDestination(cmd_parms* pParams, void* pCfg, const char* pDestination) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }
    struct DupConf *tC = reinterpret_cast<DupConf *>(pCfg);
    assert(tC);
    if (!pDestination || strlen(pDestination) == 0) {
        return "Missing destination argument";
    }
    tC->currentDupDestination = pDestination;
    return NULL;
}

const char*
setApplicationScope(cmd_parms* pParams, void* pCfg, const char* pAppScope) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }
    struct DupConf *tC = reinterpret_cast<DupConf *>(pCfg);
    try {
        tC->currentApplicationScope = ApplicationScope::stringToEnum(pAppScope);
    } catch (std::exception e) {
        return ApplicationScope::c_ERROR_ON_STRING_VALUE;
    }
    return NULL;
}

const char*
setRawSubstitute(cmd_parms* pParams, void* pCfg,
                 const char* pMatch, const char* pReplace){
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }
    struct DupConf *conf = reinterpret_cast<DupConf *>(pCfg);
    assert(conf);

    try {
        gProcessor->addRawSubstitution(pParams->path, pMatch, pReplace,
                                       *conf);
    } catch (boost::bad_expression) {
        return "Invalid regular expression in substitution definition.";
    }
    return NULL;
}

/**
 * @brief Set the minimum and maximum number of threads
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pMin the minimum number of threads
 * @param pMax the maximum number of threads
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setThreads(cmd_parms* pParams, void* pCfg, const char* pMin, const char* pMax) {
	size_t lMin, lMax;
	try {
		lMin = boost::lexical_cast<size_t>(pMin);
		lMax = boost::lexical_cast<size_t>(pMax);
	} catch (boost::bad_lexical_cast) {
		return "Invalid value(s) for minimum and maximum number of threads.";
	}

	if (lMax < lMin) {
		return "Invalid value(s) for minimum and maximum number of threads.";
	}
	gThreadPool->setThreads(lMin, lMax);
	return NULL;
}

/**
 * @brief Set the timeout for outgoing requests
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pTimeout the timeout for outgoing requests in ms
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setTimeout(cmd_parms* pParams, void* pCfg, const char* pTimeout) {
    size_t lTimeout;
    try {
        lTimeout = boost::lexical_cast<unsigned int>(pTimeout);
    } catch (boost::bad_lexical_cast) {
        return "Invalid value(s) for timeout.";
    }

    gProcessor->setTimeout(lTimeout);
    return NULL;
}

const char*
setDuplicationType(cmd_parms* pParams, void* pCfg, const char* pDupType) {
    try {
        DuplicationType::eDuplicationType v = DuplicationType::stringToEnum(pDupType);
        DuplicationType::value = v;
    } catch (std::exception e) {
        return DuplicationType::c_ERROR_ON_STRING_VALUE;
    }
    return NULL;
}


/**
 * @brief Set the minimum and maximum queue size
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pMin the minimum queue size
 * @param pMax the maximum queue size
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setQueue(cmd_parms* pParams, void* pCfg, const char* pMin, const char* pMax) {
    size_t lMin, lMax;
    try {
        lMin = boost::lexical_cast<size_t>(pMin);
        lMax = boost::lexical_cast<size_t>(pMax);
    } catch (boost::bad_lexical_cast) {
        return "Invalid value(s) for minimum and maximum queue size.";
    }

    if (lMax < lMin) {
        return "Invalid value(s) for minimum and maximum queue size.";
    }

    gThreadPool->setQueue(lMin, lMax);
    return NULL;
}

/**
 * @brief Add a substitution definition
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pScope the scope of the substitution (HEADER, BODY, ALL)
 * @param pField the field on which to do the substitution
 * @param pMatch the regexp matching what should be replaced
 * @param pReplace the value which the match should be replaced with
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setSubstitute(cmd_parms* pParams, void* pCfg, const char *pField, const char* pMatch, const char* pReplace) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }

    struct DupConf *conf = reinterpret_cast<DupConf *>(pCfg);
    assert(conf);

    try {
        gProcessor->addSubstitution(pParams->path, pField, pMatch, pReplace, *conf);
    } catch (boost::bad_expression) {
        return "Invalid regular expression in substitution definition.";
    }
    return NULL;
}

const char*
setEnrichContext(cmd_parms* pParams, void* pCfg, const char *pVarName, const char* pMatchRegex, const char* pSetValue) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }

    struct DupConf *conf = reinterpret_cast<DupConf *>(pCfg);
    assert(conf);

    try {
        gProcessor->addEnrichContext(pParams->path, pVarName, pMatchRegex, pSetValue, *conf);
    } catch (boost::bad_expression) {
        return "Invalid regular expression in EnrichContext definition.";
    }
    return NULL;
}

/**
 * @brief Activate duplication
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @return NULL
 */
const char*
setActive(cmd_parms* pParams, void* pCfg) {
    struct DupConf *lConf = reinterpret_cast<DupConf *>(pCfg);
    if (!lConf) {
        return "No per_dir conf defined. This should never happen!";
    }
    // No dir name initialized
    if (!(lConf->dirName)) {
        lConf->dirName = (char *) apr_pcalloc(pParams->pool, sizeof(char) * (strlen(pParams->path) + 1));
        strcpy(lConf->dirName, pParams->path);
    }
    return NULL;
}

/**
 * @brief Add a filter definition
 * @param pParams miscellaneous data
 * @param pCfg user data for the directory/location
 * @param pField the field on which to do the substitution
 * @param pFilter a reg exp which has to match for this request to be duplicated
 * @return NULL if parameters are valid, otherwise a string describing the error
 */
const char*
setFilter(cmd_parms* pParams, void* pCfg, const char *pField, const char* pFilter) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }

    struct DupConf *conf = reinterpret_cast<DupConf *>(pCfg);
    assert(conf);

    try {
        gProcessor->addFilter(pParams->path, pField, pFilter, *conf);
    } catch (boost::bad_expression) {
        return "Invalid regular expression in filter definition.";
    }
    return NULL;
}


const char*
setRawFilter(cmd_parms* pParams, void* pCfg, const char* pExpression) {
    const char *lErrorMsg = setActive(pParams, pCfg);
    if (lErrorMsg) {
        return lErrorMsg;
    }
    struct DupConf *conf = reinterpret_cast<DupConf *>(pCfg);
    assert(conf);

    try {
        gProcessor->addRawFilter(pParams->path, pExpression, *conf);
    } catch (boost::bad_expression) {
        return "Invalid regular expression in filter definition.";
    }
    return NULL;
}

/**
 * @brief Clean up before the child exits
 */
apr_status_t
cleanUp(void *) {
	gThreadPool->stop();
	delete gThreadPool;
	gThreadPool = NULL;

	delete gProcessor;
	gProcessor = NULL;
	return APR_SUCCESS;
}

/**
 * @brief init curl and our own thread pool on child init
 * @param pPool the apache pool
 * @param pServer the apache server record
 */
void
childInit(apr_pool_t *pPool, server_rec *pServer) {
	curl_global_init(CURL_GLOBAL_ALL);
	gThreadPool->start();

	apr_pool_cleanup_register(pPool, NULL, cleanUp, cleanUp);
}


/** @brief Declaration of configuration commands */
command_rec gCmds[] = {
    // AP_INIT_(directive,
    //          function,
    //          void * extra data,
    //          overrides to allow in order to enable,
    //          help message),
    AP_INIT_TAKE1("DupName",
                  reinterpret_cast<const char *(*)()>(&setName),
                  0,
                  OR_ALL,
                  "Set the program name for the stats log messages"),
    AP_INIT_TAKE1("DupDuplicationType",
                  reinterpret_cast<const char *(*)()>(&setDuplicationType),
                  0,
                  OR_ALL,
                  "Sets the duplication type that will used for all the following filters declarations"),
    AP_INIT_TAKE1("DupUrlCodec",
                  reinterpret_cast<const char *(*)()>(&setUrlCodec),
                  0,
                  OR_ALL,
                  "Set the url enc/decoding style for url arguments (default or apache)"),
    AP_INIT_TAKE1("DupTimeout",
                  reinterpret_cast<const char *(*)()>(&setTimeout),
                  0,
                  OR_ALL,
                  "Set the timeout for outgoing requests in milliseconds."),
    AP_INIT_TAKE2("DupThreads",
                  reinterpret_cast<const char *(*)()>(&setThreads),
                  0,
                  OR_ALL,
                  "Set the minimum and maximum number of threads per pool."),
    AP_INIT_TAKE2("DupQueue",
                  reinterpret_cast<const char *(*)()>(&setQueue),
                  0,
                  OR_ALL,
                  "Set the minimum and maximum queue size for each thread pool."),
    AP_INIT_TAKE1("DupDestination",
                  reinterpret_cast<const char *(*)()>(&setDestination),
                  0,
                  ACCESS_CONF,
                  "Set the destination for the duplicated requests. Format: host[:port]"),
    AP_INIT_TAKE1("DupApplicationScope",
                  reinterpret_cast<const char *(*)()>(&setApplicationScope),
                  0,
                  ACCESS_CONF,
                  "Sets the application scope of the filters and subsitution rules that follow this declaration"),
    AP_INIT_TAKE2("DupFilter",
                  reinterpret_cast<const char *(*)()>(&setFilter),
                  0,
                  ACCESS_CONF,
                  "Filter incoming request fields before duplicating them. "
                  "If one or more filters are specified, at least one of them has to match."),
    AP_INIT_TAKE1("DupRawFilter",
                  reinterpret_cast<const char *(*)()>(&setRawFilter),
                  0,
                  ACCESS_CONF,
                  "Filter incoming request fields before duplicating them."
                  "1st Arg: BODY HEAD ALL, data to match with the regex"
                  "Simply performs a match with the specified REGEX."),
    AP_INIT_TAKE2("DupRawSubstitute",
                  reinterpret_cast<const char *(*)()>(&setRawSubstitute),
                  0,
                  ACCESS_CONF,
                  "Filter incoming request fields before duplicating them."
                  "1st Arg: BODY HEAD ALL, data to match with the regex"
                  "Simply performs a match with the specified REGEX."),
    AP_INIT_TAKE3("DupSubstitute",
                  reinterpret_cast<const char *(*)()>(&setSubstitute),
                  0,
                  ACCESS_CONF,
                  ""),
    AP_INIT_TAKE3("DupEnrichContext",
                  reinterpret_cast<const char *(*)()>(&setEnrichContext),
                  0,
                  ACCESS_CONF,
                  "Enrich apache context with some variable."
                  "Usage: DupEnrichContext VarName MatchRegex SetRegex"
                  "VarName: The name of the variable to define"
                  "MatchRegex: The regex that must match to define the variable"
                  "SetRegex: The value to set if MatchRegex matches"),
    AP_INIT_NO_ARGS("Dup",
                    reinterpret_cast<const char *(*)()>(&setActive),
                    0,
                    ACCESS_CONF,
                    "Duplicating requests on this location using the dup module. "
                    "This is only needed if no filter or substitution is defined."),
    {0}
};

/**
 * @brief register hooks in apache
 * @param pPool the apache pool
 */
void
registerHooks(apr_pool_t *pPool) {
#ifndef UNIT_TESTING
    ap_hook_pre_config(preConfig, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(postConfig, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(&childInit, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_input_filter(gName, inputFilterHandler, NULL, AP_FTYPE_CONTENT_SET);
    ap_register_output_filter(gName, outputFilterHandler, NULL, AP_FTYPE_CONNECTION);
#endif
}

} // End namespace

/// Apache module declaration
module AP_MODULE_DECLARE_DATA dup_module = {
    STANDARD20_MODULE_STUFF,
    DupModule::createDirConfig,
    0, // merge_dir_config
    0, // create_server_config
    0, // merge_server_config
    DupModule::gCmds,
    DupModule::registerHooks
};
