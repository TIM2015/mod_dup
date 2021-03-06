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
#include "RequestProcessor.hh"
#include "Utils.hh"
#include <httpd.h>
#include <http_config.h>
#include <http_request.h>
#include <http_protocol.h>

#include "TfyTestRunner.hh"
#include "testModDup.hh"

// cppunit
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>

CPPUNIT_TEST_SUITE_REGISTRATION( TestModDup );

extern module AP_DECLARE_DATA dup_module;


namespace DupModule {

    RequestProcessor *gProcessor;
    ThreadPool<boost::shared_ptr<RequestInfo> > *gThreadPool;
    std::set<std::string> gActiveLocations;
}

using namespace DupModule;
static boost::shared_ptr<RequestInfo> POISON_REQUEST(new RequestInfo());

cmd_parms * TestModDup::getParms() {
    cmd_parms * lParms = new cmd_parms;
    server_rec * lServer = new server_rec;
    memset(lParms, 0, sizeof(cmd_parms));
    memset(lServer, 0, sizeof(server_rec));
    lParms->server = lServer;
    apr_pool_create(&lParms->pool, 0);
    return lParms;
}

void TestModDup::testInit()
{
    preConfig(NULL, NULL, NULL);
    CPPUNIT_ASSERT(gProcessor);
    CPPUNIT_ASSERT(gThreadPool);

    gThreadPool = new DummyThreadPool<boost::shared_ptr<RequestInfo> >(boost::bind(&RequestProcessor::run, gProcessor, _1), POISON_REQUEST);
}

class Dummy {
 public:
     Dummy(int &r) : mR(r) {
         mR = 0;
     }

     ~Dummy(){
         mR = 42;
     }

     int &mR;
 };

void TestModDup::testInitAndCleanUp()
{
    cmd_parms * lParms = getParms();
    registerHooks(lParms->pool);
    childInit(lParms->pool, lParms->server);
    postConfig(lParms->pool, lParms->pool, lParms->pool, lParms->server);
    cleanUp(NULL);
    CPPUNIT_ASSERT(!gProcessor);
    CPPUNIT_ASSERT(!gThreadPool);

    // Cleaner test
    int test;
    Dummy d(test);
    CommonModule::cleaner<Dummy>((void *)&d);
    CPPUNIT_ASSERT_EQUAL(42, test);

}

void TestModDup::testConfig()
{

    CPPUNIT_ASSERT(setThreads(NULL, NULL, "", "1"));
    CPPUNIT_ASSERT(setThreads(NULL, NULL, "1", ""));
    CPPUNIT_ASSERT(setThreads(NULL, NULL, "2", "1"));
    CPPUNIT_ASSERT(!setThreads(NULL, NULL, "1", "2"));
    CPPUNIT_ASSERT(!setThreads(NULL, NULL, "0", "0"));
    CPPUNIT_ASSERT(setThreads(NULL, NULL, "-1", "2"));


    CPPUNIT_ASSERT(setQueue(NULL, NULL, "", "1"));
    CPPUNIT_ASSERT(setQueue(NULL, NULL, "1", ""));
    CPPUNIT_ASSERT(setQueue(NULL, NULL, "2", "1"));
    CPPUNIT_ASSERT(!setQueue(NULL, NULL, "1", "2"));
    CPPUNIT_ASSERT(!setQueue(NULL, NULL, "0", "0"));
    CPPUNIT_ASSERT(setQueue(NULL, NULL, "-1", "2"));

    cmd_parms * lParms = getParms();
    lParms->path = new char[10];
    strcpy(lParms->path, "/spp/main");
    // Pointer to a boolean meant to activate the module on a given path
    DupConf *lDoHandle = new DupConf();


    CPPUNIT_ASSERT(setDestination(lParms, (void *) lDoHandle, NULL, NULL));
    CPPUNIT_ASSERT(setDestination(lParms, (void *) lDoHandle, "", NULL));
    CPPUNIT_ASSERT(!setDestination(lParms, (void *) lDoHandle, "localhost", NULL));

    // Substitutions
    CPPUNIT_ASSERT(!setSubstitute(lParms, (void *)lDoHandle, "toto", "toto", "titi"));
    CPPUNIT_ASSERT(setSubstitute(lParms, (void *)lDoHandle, "toto", "*t(oto", "titi"));
    CPPUNIT_ASSERT(!setRawSubstitute(lParms, (void *)lDoHandle, "toMatch", "toReplace"));
    CPPUNIT_ASSERT(setRawSubstitute(lParms, (void *)lDoHandle, "toMatchInvalid(", "toReplace"));

    // Filters
    CPPUNIT_ASSERT(!setFilter(lParms, (void *)lDoHandle, "titi", "toto"));
    CPPUNIT_ASSERT(setFilter(lParms, (void *)lDoHandle, "titi", "*toto"));
    CPPUNIT_ASSERT(!setRawFilter(lParms, (void *)lDoHandle, "Filter"));
    CPPUNIT_ASSERT(setRawFilter(lParms, (void *)lDoHandle, "InvalidFilter("));

    // Program name tests
    CPPUNIT_ASSERT(!setName(lParms, (void *)lDoHandle, "ProgramName"));
    CPPUNIT_ASSERT(setName(lParms, (void *)lDoHandle, ""));
    CPPUNIT_ASSERT(setName(lParms, (void *)lDoHandle, NULL));

    // Timeout tests
    CPPUNIT_ASSERT(!setTimeout(lParms, (void *) lDoHandle, "1400"));
    CPPUNIT_ASSERT(setTimeout(lParms, (void *) lDoHandle, "what??"));

    // URL codec
    CPPUNIT_ASSERT(!setUrlCodec(lParms, (void *) lDoHandle, "Apache"));
    CPPUNIT_ASSERT(setUrlCodec(lParms, (void *) lDoHandle, ""));
    CPPUNIT_ASSERT(setUrlCodec(lParms, (void *) lDoHandle, NULL));

    CPPUNIT_ASSERT(!setActive(lParms, lDoHandle));



    delete [] lParms->path;
}

void TestModDup::testScope()
{
    cmd_parms * lParms = getParms();
    lParms->path = new char[10];
    strcpy(lParms->path, "/spp/main");
    DupConf *conf = new DupConf();

    // Default value
    CPPUNIT_ASSERT_EQUAL(ApplicationScope::HEADER, conf->currentApplicationScope);

    // Switching to ALL
    CPPUNIT_ASSERT(!setApplicationScope(lParms, (void *)conf, "ALL"));
    CPPUNIT_ASSERT_EQUAL(ApplicationScope::ALL, conf->currentApplicationScope);

    // Switching to HEADER
    CPPUNIT_ASSERT(!setApplicationScope(lParms, (void *)conf, "HEADER"));
    CPPUNIT_ASSERT_EQUAL(ApplicationScope::HEADER, conf->currentApplicationScope);

    // Switching to BODY
    CPPUNIT_ASSERT(!setApplicationScope(lParms, (void *)conf, "BODY"));
    CPPUNIT_ASSERT_EQUAL(ApplicationScope::BODY, conf->currentApplicationScope);

    // Incorrect value
    CPPUNIT_ASSERT(setApplicationScope(lParms, (void *)conf, "incorrect_vALUE"));
}

void TestModDup::testDuplicationType()
{
    cmd_parms * lParms = getParms();
    lParms->path = new char[10];
    strcpy(lParms->path, "/spp/main");
    DupConf *conf = new DupConf();

    // Default value
    CPPUNIT_ASSERT_EQUAL(DuplicationType::NONE, conf->getCurrentDuplicationType());

    // Switching to COMPLETE_REQUEST
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "COMPLETE_REQUEST"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::COMPLETE_REQUEST, conf->getCurrentDuplicationType());

    // Switching to REQUEST_WITH_ANSWER
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "REQUEST_WITH_ANSWER"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::REQUEST_WITH_ANSWER, conf->getCurrentDuplicationType());

    // Switching to NONE
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "NONE"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::NONE, conf->getCurrentDuplicationType());

    // Switching to HEADER_ONLY
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "HEADER_ONLY"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::HEADER_ONLY, conf->getCurrentDuplicationType());

    // Incorrect value
    CPPUNIT_ASSERT(setDuplicationType(lParms, (void *)conf, "incorrect_vALUE"));
}

void TestModDup::testHighestDuplicationType()
{
    cmd_parms * lParms = getParms();
    lParms->path = new char[10];
    strcpy(lParms->path, "/spp/main");
    DupConf *conf = new DupConf();

    // Default value
    CPPUNIT_ASSERT_EQUAL(DuplicationType::NONE, conf->getHighestDuplicationType());

    // Switching to COMPLETE_REQUEST
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "COMPLETE_REQUEST"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::COMPLETE_REQUEST, conf->getHighestDuplicationType());

    // Should NOT go back to HEADER_ONLY
    CPPUNIT_ASSERT(!setDuplicationType(lParms, (void *)conf, "HEADER_ONLY"));
    CPPUNIT_ASSERT_EQUAL(DuplicationType::COMPLETE_REQUEST, conf->getHighestDuplicationType());
}

void TestModDup::testDuplicationPercentage() {
    // Basic part
    {
        testInit();
        cmd_parms * lParms = getParms();
        lParms->path = strdup("/spp/main");
        // Pointer to a boolean meant to activate the module on a given path
        DupConf *lDoHandle = new DupConf();

        // NAN test
        CPPUNIT_ASSERT(setDestination(lParms, (void *) lDoHandle, "localhost", "JeNeSuisPasUnNombre"));

        // Out of range
        CPPUNIT_ASSERT(setDestination(lParms, (void *) lDoHandle, "localhost", "666"));

        // Nominal test
        CPPUNIT_ASSERT(!setDestination(lParms, (void *) lDoHandle, "localhost", "12"));
    }

    {
        testInit();
        cmd_parms * lParms = getParms();
        lParms->path = strdup("/spp/main");
        // Pointer to a boolean meant to activate the module on a given path
        DupConf *lDoHandle = new DupConf();

        CPPUNIT_ASSERT(!setDestination(lParms, (void *) lDoHandle, "localhost:42", "42"));
        CPPUNIT_ASSERT(!setFilter(lParms, (void *) lDoHandle, "SID", "fortytwo"));

        CPPUNIT_ASSERT(!setDestination(lParms, (void *) lDoHandle, "localhost:84", "84"));
        CPPUNIT_ASSERT(!setFilter(lParms, (void *) lDoHandle, "SID", "eightyfour"));


        {
            RequestInfo ri = RequestInfo(std::string("42"),"/spp/main", "/spp/main/foo/", "SID=eightyfour");
            std::list<std::pair<std::string, std::string> > lParsedArgs;
            gProcessor->parseArgs(lParsedArgs, ri.mArgs);
            std::list<const tFilter *> ff = gProcessor->processRequest(ri, lParsedArgs);
            CPPUNIT_ASSERT_EQUAL(1, (int)ff.size());

            // Check the filter that matched
            const tFilter *first = ff.front();
            CPPUNIT_ASSERT_EQUAL(std::string("localhost:84"), first->mDestination);
            CPPUNIT_ASSERT_EQUAL(tFilter::eFilterTypes::REGULAR, first->mFilterType);

            // Get a handle on the corresponding command
            CommandsByDestination &cbd = gProcessor->mCommands.at(ri.mConfPath);
            Commands &c = cbd.mCommands.at(first->mDestination);
            // Checks the duplication percentage
            CPPUNIT_ASSERT_EQUAL((unsigned int)84, c.mDuplicationPercentage);
        }

        {
            RequestInfo ri = RequestInfo(std::string("42"),"/spp/main", "/spp/main/foo/", "SID=fortytwo");
            std::list<std::pair<std::string, std::string> > lParsedArgs;
            gProcessor->parseArgs(lParsedArgs, ri.mArgs);
            std::list<const tFilter *> ff = gProcessor->processRequest(ri, lParsedArgs);
            CPPUNIT_ASSERT_EQUAL(1, (int)ff.size());

            // Check the filter that matched
            const tFilter *first = ff.front();
            CPPUNIT_ASSERT_EQUAL(std::string("localhost:42"), first->mDestination);
            CPPUNIT_ASSERT_EQUAL(tFilter::eFilterTypes::REGULAR, first->mFilterType);

            // Get a handle on the corresponding command
            CommandsByDestination &cbd = gProcessor->mCommands.at(ri.mConfPath);
            Commands &c = cbd.mCommands.at(first->mDestination);
            // Checks the duplication percentage
            CPPUNIT_ASSERT_EQUAL((unsigned int)42, c.mDuplicationPercentage);
        }

    }


}

#ifdef UNIT_TESTING
//--------------------------------------
// the main method
//--------------------------------------
int main(int argc, char* argv[])
{
    Log::init();
    apr_initialize();
    TfyTestRunner runner(argv[0]);
    runner.addTest(CppUnit::TestFactoryRegistry::getRegistry().makeTest());
    bool failed = runner.run();

    return !failed;
}
#endif
