#include <unistd.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "macaddress.h"
#include "producerstatetable.h"
#include "vxlanmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

/*
 * Following global variables are defined here for the purpose of
 * using existing Orch class which is to be refactored soon to
 * eliminate the direct exposure of the global variables.
 *
 * Once Orch class refactoring is done, these global variables
 * should be removed from here.
 */
int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;
/* Global database mutex */
mutex gDbMutex;
MacAddress gMacAddress;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vxlanmgrd");

    SWSS_LOG_NOTICE("--- Starting vxlanmgrd ---");

    try
    {

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        WarmStart::initialize("vxlanmgrd", "swss");
        WarmStart::checkWarmStart("vxlanmgrd", "swss");
        if (WarmStart::isWarmStart())
        {
            WarmStart::setWarmStartState("vxlanmgrd", WarmStart::INITIALIZED);
        }

        vector<std::string> cfg_vnet_tables = {
            CFG_VNET_TABLE_NAME,
            CFG_VXLAN_TUNNEL_TABLE_NAME,
            CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
            CFG_VXLAN_EVPN_NVO_TABLE_NAME,
        };

        VxlanMgr vxlanmgr(&cfgDb, &appDb, &stateDb, cfg_vnet_tables);
        std::vector<Orch *> cfgOrchList = {&vxlanmgr};
        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        /*
         * swss service starts after interfaces-config.service which will have
         * switch_mac set.
         * Dynamic switch_mac update is not supported for now.
         */
        Table table(&cfgDb, "DEVICE_METADATA");
        std::vector<FieldValueTuple> ovalues;
        table.get("localhost", ovalues);
        auto it = std::find_if( ovalues.begin(), ovalues.end(), [](const FieldValueTuple& t){ return t.first == "mac";} );
        if ( it == ovalues.end() ) {
            throw runtime_error("couldn't find MAC address of the device from config DB");
        }
        gMacAddress = MacAddress(it->second);

        auto in_recon = true;
        vxlanmgr.beginReconcile(true);

        if (WarmStart::isWarmStart())
        {
            vxlanmgr.waitTillReadyToReconcile();
            vxlanmgr.restoreVxlanNetDevices();
            WarmStart::setWarmStartState("vxlanmgrd", WarmStart::REPLAYED);
            uint16_t wait_secs = 0;
            string val = "";
            Table wb_tbl = Table(&stateDb, STATE_WARM_RESTART_TABLE_NAME);
            wb_tbl.hget("orchagent", "restore_count", val);
            if ((val != "") or (val != "0"))
            {
                wb_tbl.hget("orchagent", "state", val);
                while (val != "reconciled")
                {
                    SWSS_LOG_NOTICE("Waiting Until Orchagent is reconciled. Current %s. Waited %u secs", val.c_str(), wait_secs);
                    sleep(1);
                    wait_secs++;
                    wb_tbl.hget("orchagent", "state", val);
                }
            }
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                if (true == in_recon)
                {
                    in_recon = false;
                    vxlanmgr.endReconcile(false);
                    if (WarmStart::isWarmStart())
                    {
                        WarmStart::setWarmStartState("vxlanmgrd", WarmStart::RECONCILED);
                    }
                }
                vxlanmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
