#ifndef __FDBSYNC__
#define __FDBSYNC__

#include <string>
#include <arpa/inet.h>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "subscriberstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"

// The timeout value (in seconds) for neighsyncd reconcilation logic
//#define DEFAULT_FDBSYNC_WARMSTART_TIMER 5

/*
 * This is the timer value (in seconds) that the neighsyncd waits for restore_neighbors
 * service to finish, should be longer than the restore_neighbors timeout value (110)
 * This should not happen, if happens, system is in a unknown state, we should exit.
 */
//#define RESTORE_FDB_WAIT_TIME_OUT 120

namespace swss {

enum FDB_OP_TYPE {
    FDB_OPER_ADD =1,
    FDB_OPER_DEL = 2,
};

enum FDB_TYPE {
    FDB_TYPE_STATIC = 1,
    FDB_TYPE_DYNAMIC = 2,
};

struct fdb_info
{
    char mac[32];
    unsigned int vid;
    char port_name[32];
    short type;/*dynamic or static*/
    short op_type;/*add or del*/
};

class FdbSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    FdbSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb/*, DBConnector *config_db*/);
    ~FdbSync();

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isFdbRestoreDone();
#if 0
    AppRestartAssist *getRestartAssist()
    {
        return m_AppRestartAssist;
    }
#endif
    SubscriberStateTable *getFdbStateTable()
    {
        return &m_fdbStateTable;
    }

    void processStateFdb();

    //void processCfgVxlanMap();


private:
    //Table m_stateFdbRestoreTable;
    ProducerStateTable m_fdbTable;
    ProducerStateTable m_imetTable;
    SubscriberStateTable m_fdbStateTable;
    //AppRestartAssist  *m_AppRestartAssist;

    struct fdbmacinfo
    {
        std::string port_name;        
        short type;/*dynamic or static*/
    };
    std::unordered_map<std::string, fdbmacinfo> fdbmac; 

    void macDelVxlanEntry(std::string auxkey, struct fdb_info *info);

    void macAddSrcDB(struct fdb_info *info);

    bool macCheckSrcDB(struct fdb_info *info);

    void macUpdateStateDB(struct fdb_info *info);

    void macRefreshStateDB(int vlan, std::string kmac);

    bool checkImetExit(std::string key, uint32_t vni);

    bool checkDelImet(std::string key, uint32_t vni);

    struct macinfo
    {
        std::string vtep;
        unsigned int type;
        unsigned int vni;
        std::string  ifname;
    };
    std::unordered_map<std::string, macinfo> m_mac;

    struct imetinfo
    {
        unsigned int vni;
        bool         redist_add_status;
    };
    std::unordered_map<std::string, imetinfo> imet_route;

    struct intf
    {
        std::string ifname;
        unsigned int vni;
    };
    std::unordered_map<int, intf> intf_info;

    std::map<unsigned int, unsigned int> m_vlanVniMap;

    void macAddVxlan(std::string key, struct in_addr vtep, uint32_t type, uint32_t vni, std::string intf_name);
    void macDelVxlan(std::string auxkey);
    void macUpdateVxlanDB(std::string key);
    void macDelVxlanDB(std::string key);
    void imetAddRoute(struct in_addr vtep, std::string ifname, uint32_t vni);
    void imetDelRoute(struct in_addr vtep, std::string ifname, uint32_t vni);
	void onMsgNbr(int nlmsg_type, struct nl_object *obj);
	void onMsgLink(int nlmsg_type, struct nl_object *obj);
};

}

#endif

