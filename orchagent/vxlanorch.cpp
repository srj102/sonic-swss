#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <inttypes.h>


#include "sai.h"
#include "macaddress.h"
#include "ipaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "vxlanorch.h"
#include "directory.h"
#include "swssnet.h"

/* Global variables */
extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_next_hop_api_t *sai_next_hop_api;
extern Directory<Orch*> gDirectory;
extern PortsOrch*       gPortsOrch;
extern sai_object_id_t  gUnderlayIfId;

const map<MAP_T, uint32_t> vxlanTunnelMap =
{
    { MAP_T::VNI_TO_VLAN_ID, SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID },
    { MAP_T::VLAN_ID_TO_VNI, SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI },
    { MAP_T::VRID_TO_VNI, SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI },
    { MAP_T::VNI_TO_VRID, SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID },
    { MAP_T::BRIDGE_TO_VNI, SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI },
    { MAP_T::VNI_TO_BRIDGE,  SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF},
};

const map<MAP_T, std::pair<uint32_t, uint32_t>> vxlanTunnelMapKeyVal =
{
    { MAP_T::VNI_TO_VLAN_ID,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE }
    },
    { MAP_T::VLAN_ID_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VRID_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VNI_TO_VRID,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE }
    },
    { MAP_T::BRIDGE_TO_VNI,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE }
    },
    { MAP_T::VNI_TO_BRIDGE,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_VALUE }
    },
};

/*
 * Manipulators for the above Map
 */
static inline uint32_t tunnel_map_type (MAP_T map_t)
{
    return vxlanTunnelMap.at(map_t);
}

static inline uint32_t tunnel_map_key (MAP_T map_t)
{
    return vxlanTunnelMapKeyVal.at(map_t).first;
}

static inline uint32_t tunnel_map_val (MAP_T map_t)
{
    return vxlanTunnelMapKeyVal.at(map_t).second;
}

//------------------- SAI Interface functions --------------------------//

static sai_object_id_t
create_tunnel_map(MAP_T map_t)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    if (map_t == MAP_T::MAP_TO_INVALID)
    {
        SWSS_LOG_ERROR("Invalid map type %d", static_cast<int>(map_t));
        return SAI_NULL_OBJECT_ID;
    }

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    attr.value.s32 = tunnel_map_type(map_t);

    tunnel_map_attrs.push_back(attr);

    sai_object_id_t tunnel_map_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_map(
                                &tunnel_map_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_map_attrs.size()),
                                tunnel_map_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create tunnel map object");
    }

    return tunnel_map_id;
}

void
remove_tunnel_map(sai_object_id_t tunnel_map_id)
{
    sai_status_t status = sai_tunnel_api->remove_tunnel_map(tunnel_map_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't remove a tunnel map object");
    }
}

static sai_object_id_t create_tunnel_map_entry(
    MAP_T map_t,
    sai_object_id_t tunnel_map_id,
    sai_uint32_t vni,
    sai_uint16_t vlan_id,
    sai_object_id_t obj_id=SAI_NULL_OBJECT_ID,
    bool encap=false
    )
{
    sai_attribute_t attr;
    sai_object_id_t tunnel_map_entry_id;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.s32 = tunnel_map_type(map_t);
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap)? tunnel_map_key(map_t):tunnel_map_val(map_t);
    if (obj_id != SAI_NULL_OBJECT_ID)
    {
        attr.value.oid = obj_id;
    }
    else
    {
        attr.value.u16 = vlan_id;
    }

    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap)? tunnel_map_val(map_t):tunnel_map_key(map_t);
    attr.value.u32 = vni;
    tunnel_map_entry_attrs.push_back(attr);

    sai_status_t status = sai_tunnel_api->create_tunnel_map_entry(&tunnel_map_entry_id, gSwitchId,
                                            static_cast<uint32_t> (tunnel_map_entry_attrs.size()),
                                            tunnel_map_entry_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel map entry object");
    }

    return tunnel_map_entry_id;
}

void remove_tunnel_map_entry(sai_object_id_t obj_id)
{
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (obj_id != SAI_NULL_OBJECT_ID)
    {
        status = sai_tunnel_api->remove_tunnel_map_entry(obj_id);
    }

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't delete a tunnel map entry object");
    }
}

static sai_status_t create_nexthop_tunnel(
    sai_ip_address_t host_ip,
    sai_uint32_t vni, // optional vni
    sai_mac_t *mac, // inner destination mac
    sai_object_id_t tunnel_id,
    sai_object_id_t *next_hop_id)
{
    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    next_hop_attr.value.ipaddr = host_ip;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    next_hop_attr.value.oid = tunnel_id;
    next_hop_attrs.push_back(next_hop_attr);

    if (vni != 0)
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_VNI;
        next_hop_attr.value.u32 = vni;
        next_hop_attrs.push_back(next_hop_attr);
    }

    if (mac != nullptr)
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_MAC;
        memcpy(next_hop_attr.value.mac, mac, sizeof(sai_mac_t));
        next_hop_attrs.push_back(next_hop_attr);
    }

    sai_status_t status = sai_next_hop_api->create_next_hop(next_hop_id, gSwitchId,
                                            static_cast<uint32_t>(next_hop_attrs.size()),
                                            next_hop_attrs.data());
    return status;
}

// Create Tunnel
static sai_object_id_t
create_tunnel(
    struct tunnel_ids_t* ids,
    sai_ip_address_t *src_ip,
    sai_ip_address_t *dst_ip,
    sai_object_id_t underlay_rif,
    sai_uint8_t encap_ttl=0)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = underlay_rif;
    tunnel_attrs.push_back(attr);

    sai_object_id_t map_list[TUNNEL_MAP_T_MAX_MAPPER+1];
    uint8_t num_map=0;

    for(int i=TUNNEL_MAP_T_VLAN;i<TUNNEL_MAP_T_MAX_MAPPER;i++)
    {
      if(ids->tunnel_decap_id[i] != SAI_NULL_OBJECT_ID)
      {
        map_list[num_map] = ids->tunnel_decap_id[i];
        SWSS_LOG_NOTICE("create_tunnel:maplist[%d]=0x%lx",num_map,map_list[num_map]);
        num_map++;
      }
    }
      
    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = num_map;
    attr.value.objlist.list = map_list;
    tunnel_attrs.push_back(attr);

    sai_object_id_t emap_list[TUNNEL_MAP_T_MAX_MAPPER+1];
    uint8_t num_emap=0;

    for(int i=TUNNEL_MAP_T_VLAN;i<TUNNEL_MAP_T_MAX_MAPPER;i++)
    {
      if(ids->tunnel_encap_id[i] != SAI_NULL_OBJECT_ID)
      {
        emap_list[num_emap] = ids->tunnel_encap_id[i];
        SWSS_LOG_NOTICE("create_tunnel:encapmaplist[%d]=0x%lx",num_emap,emap_list[num_emap]);
        num_emap++;
      }
    }

    attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    attr.value.objlist.count = num_emap;
    attr.value.objlist.list = emap_list;
    tunnel_attrs.push_back(attr);

    // source ip
    if (src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        attr.value.ipaddr = *src_ip;
        tunnel_attrs.push_back(attr);
    }

    // dest ip
    if (dst_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
        attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2P;
        tunnel_attrs.push_back(attr);

        attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
        attr.value.ipaddr = *dst_ip;
        tunnel_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
        attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2MP;
        tunnel_attrs.push_back(attr);
    }

    if (encap_ttl != 0)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_TTL_MODE;
        attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
        tunnel_attrs.push_back(attr);

        attr.id = SAI_TUNNEL_ATTR_ENCAP_TTL_VAL;
        attr.value.u8 = encap_ttl;
        tunnel_attrs.push_back(attr);
    }

    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(
                                &tunnel_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel object");
    }

    return tunnel_id;
}

void
remove_tunnel(sai_object_id_t tunnel_id)
{
    if (tunnel_id != SAI_NULL_OBJECT_ID)
    {
        sai_status_t status = sai_tunnel_api->remove_tunnel(tunnel_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw std::runtime_error("Can't remove a tunnel object");
        }
    }
    else
    {
        SWSS_LOG_DEBUG("Tunnel id is NULL.");
    }
}

// Create tunnel termination
static sai_object_id_t
create_tunnel_termination(
    sai_object_id_t tunnel_oid,
    sai_ip_address_t srcip,
    sai_ip_address_t *dstip,
    sai_object_id_t default_vrid)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    if(dstip == nullptr) // It's P2MP tunnel
    {
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
        tunnel_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P;
        tunnel_attrs.push_back(attr);

        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP;
        attr.value.ipaddr = *dstip;
        tunnel_attrs.push_back(attr);
    }

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = default_vrid;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    attr.value.ipaddr = srcip;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_oid;
    tunnel_attrs.push_back(attr);

    sai_object_id_t term_table_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(
                                &term_table_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel term table object");
    }

    return term_table_id;
}

void
remove_tunnel_termination(sai_object_id_t term_table_id)
{
    if (term_table_id != SAI_NULL_OBJECT_ID)
    {
        sai_status_t status = sai_tunnel_api->remove_tunnel_term_table_entry(term_table_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw std::runtime_error("Can't remove a tunnel term table object");
        }
    }
    else
    {
        SWSS_LOG_DEBUG("Tunnel term table id is NULL.");
    }
}

//------------------- VxlanTunnel Implementation --------------------------//

bool VxlanTunnel::createTunnel(MAP_T encap, MAP_T decap, uint8_t encap_ttl)
{
    try
    {
        sai_ip_address_t ips, ipd, *ip=nullptr;
        uint8_t mapper_list = 0;
        swss::copy(ips, src_ip_);

        // Only a single mapper type is created

        if(decap == MAP_T::VNI_TO_BRIDGE)
          TUNNELMAP_SET_BRIDGE(mapper_list);
        else if(decap == MAP_T::VNI_TO_VLAN_ID)
          TUNNELMAP_SET_VLAN(mapper_list);
        else
          TUNNELMAP_SET_VRF(mapper_list);
        
        createMapperHW(mapper_list, (encap == MAP_T::MAP_TO_INVALID) ? USE_DECAP_ONLY: USE_DEDICATED_ENCAP_DECAP);

        if (encap != MAP_T::MAP_TO_INVALID)
            ip = &ips;

        ids_.tunnel_id = create_tunnel(&ids_, ip, NULL, gUnderlayIfId, encap_ttl);

        ip = nullptr;
        if (!dst_ip_.isZero())
        {
            swss::copy(ipd, dst_ip_);
            ip = &ipd;
        }

        ids_.tunnel_term_id = create_tunnel_termination(ids_.tunnel_id, ips, ip, gVirtualRouterId);
        active_ = true;
        tunnel_map_ = { encap, decap };
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
        // FIXME: add code to remove already created objects
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan tunnel '%s' was created", tunnel_name_.c_str());
    return true;
}

sai_object_id_t VxlanTunnel::addEncapMapperEntry(sai_object_id_t obj, uint32_t vni, tunnel_map_type_t type)
{
    const auto encap_id = getEncapMapId(type);
    const auto map_t = tunnel_map_type(type,true);
    return create_tunnel_map_entry(map_t, encap_id, vni, 0, obj, true);
}

sai_object_id_t VxlanTunnel::addDecapMapperEntry(sai_object_id_t obj, uint32_t vni, tunnel_map_type_t type)
{
    const auto decap_id = getDecapMapId(type);
    const auto map_t = tunnel_map_type(type,false);
    return create_tunnel_map_entry(map_t, decap_id, vni, 0, obj);
}

void VxlanTunnel::insertMapperEntry(sai_object_id_t encap, sai_object_id_t decap, uint32_t vni)
{
    tunnel_map_entries_[vni] = std::pair<sai_object_id_t, sai_object_id_t>(encap, decap);
}

std::pair<sai_object_id_t, sai_object_id_t> VxlanTunnel::getMapperEntry(uint32_t vni)
{
    if (tunnel_map_entries_.find(vni) != tunnel_map_entries_.end())
    {
        return tunnel_map_entries_[vni];
    }

    return std::make_pair(SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID);
}

void VxlanTunnel::updateNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni, sai_object_id_t nh_id)
{
    auto key = nh_key_t(ipAddr, macAddress, vni);

    SWSS_LOG_NOTICE("Update NH tunnel for ip %s, mac %s, vni %d",
            ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni);

    auto it = nh_tunnels_.find(key);
    if (it == nh_tunnels_.end())
    {
        nh_tunnels_[key] = {nh_id, 1};
        return;
    } else {
        SWSS_LOG_NOTICE("Dup Update NH tunnel for ip %s, mac %s, vni %d",
            ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni);
    }

}

sai_object_id_t VxlanTunnel::getNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni) const
{
    auto key = nh_key_t(ipAddr, macAddress, vni);

    auto it = nh_tunnels_.find(key);
    if (it == nh_tunnels_.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return nh_tunnels_.at(key).nh_id;
}

void VxlanTunnel::incNextHopRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    auto key = nh_key_t(ipAddr, macAddress, vni);
    nh_tunnels_[key].ref_count ++;
    SWSS_LOG_NOTICE("refcnt increment NH tunnel for ip %s, mac %s, vni %d, ref_count %d",
            ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni,
            nh_tunnels_[key].ref_count);

}

void VxlanTunnel::decNextHopRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    auto key = nh_key_t(ipAddr, macAddress, vni);
    nh_tunnels_[key].ref_count --;
    SWSS_LOG_NOTICE("refcnt decrement NH tunnel for ip %s, mac %s, vni %d, ref_count %d",
                    ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni,
                    nh_tunnels_[key].ref_count);

}

bool VxlanTunnel::removeNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    auto key = nh_key_t(ipAddr, macAddress, vni);

    auto it = nh_tunnels_.find(key);
    if (it == nh_tunnels_.end())
    {
        SWSS_LOG_NOTICE("remove NH tunnel for ip %s, mac %s, vni %d doesn't exist",
                        ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni);
        return false;
    }

    SWSS_LOG_NOTICE("remove NH tunnel for ip %s, mac %s, vni %d, ref_count %d",
                    ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni,
                    nh_tunnels_[key].ref_count);

    //Decrement ref count if already exists
    nh_tunnels_[key].ref_count --;

    if (!nh_tunnels_[key].ref_count)
    {
        if (sai_next_hop_api->remove_next_hop(nh_tunnels_[key].nh_id) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("delete NH tunnel for ip '%s', mac '%s' vni %d failed",
                            ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni);
            string err_msg = "NH tunnel delete failed for " + ipAddr.to_string();
            throw std::runtime_error(err_msg);
        }

        nh_tunnels_.erase(key);
    }

    SWSS_LOG_NOTICE("NH tunnel for ip '%s', mac '%s' vni %d updated/deleted",
                    ipAddr.to_string().c_str(), macAddress.to_string().c_str(), vni);

    return true;
}

bool VxlanTunnel::deleteMapperHW(uint8_t mapper_list,
                                 tunnel_map_src_t map_src)
{
    sai_status_t status = SAI_STATUS_SUCCESS;

    SWSS_LOG_INFO("Enter deleteMapper HW maplst=%d src=%d",mapper_list, map_src);

    try
    {
      if(map_src == USE_DEDICATED_ENCAP_DECAP)
      {
        if(IS_TUNNELMAP_SET_VLAN(mapper_list))
        {
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN]);
          SWSS_LOG_INFO("deletevlandecap = %d", status);
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN]);
          SWSS_LOG_INFO("deletevlanencap = %d", status);
        }

        if(IS_TUNNELMAP_SET_VRF(mapper_list))
        {
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER]);
          SWSS_LOG_INFO("deleteVRFdecap = %d", status);
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER]);
          SWSS_LOG_INFO("deleteVRFencap = %d", status);
        }

        if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
        {
          sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_BRIDGE]);
          sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE]);
        }
      }
      else if(map_src == USE_COMMON_DECAP_DEDICATED_ENCAP)
      {
        if(IS_TUNNELMAP_SET_VLAN(mapper_list))
        {
          sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN]);
        }

        if(IS_TUNNELMAP_SET_VRF(mapper_list))
        {
          sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER]);
        }

        if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
        {
          sai_tunnel_api->remove_tunnel_map(ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE]);
        }
      }
      else if(map_src == USE_DECAP_ONLY)
      {
        if(IS_TUNNELMAP_SET_VLAN(mapper_list))
        {
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN]);
          SWSS_LOG_INFO("deletevlandecap = %d", status);
        }

        if(IS_TUNNELMAP_SET_VRF(mapper_list))
        {
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER]);
          SWSS_LOG_INFO("deleteVRFdecap = %d", status);
        }

        if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
        {
          status = sai_tunnel_api->remove_tunnel_map(ids_.tunnel_decap_id[TUNNEL_MAP_T_BRIDGE]);
          SWSS_LOG_INFO("deletebridgedecap = %d", status);
        }
      }
    }

    catch (const std::runtime_error& error)
    {
      SWSS_LOG_ERROR("Error deleting mapper %s: %s", tunnel_name_.c_str(), error.what());
      // FIXME: add code to remove already created objects
      return false;
    }

    return true;
}

bool VxlanTunnel::createMapperHW(uint8_t mapper_list, 
                                 tunnel_map_src_t map_src)
{
    try
    {
        for(int i=TUNNEL_MAP_T_VLAN;i<TUNNEL_MAP_T_MAX_MAPPER;i++)
        {
          ids_.tunnel_decap_id[i] = SAI_NULL_OBJECT_ID;
          ids_.tunnel_encap_id[i] = SAI_NULL_OBJECT_ID;
        }

        if(USE_DEDICATED_ENCAP_DECAP == map_src)
        {
           if(IS_TUNNELMAP_SET_VLAN(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN] = create_tunnel_map(MAP_T::VNI_TO_VLAN_ID);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN] = create_tunnel_map(MAP_T::VLAN_ID_TO_VNI);

             TUNNELMAP_SET_VLAN(encap_dedicated_mappers_);
             TUNNELMAP_SET_VLAN(decap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_VRF(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = create_tunnel_map(MAP_T::VNI_TO_VRID);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = create_tunnel_map(MAP_T::VRID_TO_VNI);
             TUNNELMAP_SET_VRF(encap_dedicated_mappers_);
             TUNNELMAP_SET_VRF(decap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_BRIDGE] = create_tunnel_map(MAP_T::VNI_TO_BRIDGE);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE] = create_tunnel_map(MAP_T::BRIDGE_TO_VNI);

             TUNNELMAP_SET_BRIDGE(encap_dedicated_mappers_);
             TUNNELMAP_SET_BRIDGE(decap_dedicated_mappers_);
           }
        }
        else if (map_src == USE_COMMON_DECAP_DEDICATED_ENCAP)
        {
           if(IS_TUNNELMAP_SET_VLAN(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_VLAN);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN] = create_tunnel_map(MAP_T::VLAN_ID_TO_VNI);
             TUNNELMAP_SET_VLAN(encap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_VRF(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_VIRTUAL_ROUTER);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = create_tunnel_map(MAP_T::VRID_TO_VNI);
             TUNNELMAP_SET_VRF(encap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_BRIDGE] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_BRIDGE);

             ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE] = create_tunnel_map(MAP_T::VRID_TO_VNI);

             TUNNELMAP_SET_BRIDGE(encap_dedicated_mappers_);
           }

        }
        else if (USE_COMMON_ENCAP_DECAP == map_src)
        {
           if(IS_TUNNELMAP_SET_VLAN(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_VLAN);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN] = vtep_ptr->getEncapMapId(TUNNEL_MAP_T_VLAN);
           }

           if(IS_TUNNELMAP_SET_VRF(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_VIRTUAL_ROUTER);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = vtep_ptr->getEncapMapId(TUNNEL_MAP_T_VIRTUAL_ROUTER);
           }

           if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_BRIDGE] = vtep_ptr->getDecapMapId(TUNNEL_MAP_T_BRIDGE);
             ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE] = vtep_ptr->getEncapMapId(TUNNEL_MAP_T_BRIDGE);
           }

        }
        else if(USE_DECAP_ONLY == map_src)
        {
           if(IS_TUNNELMAP_SET_VLAN(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VLAN] = create_tunnel_map(MAP_T::VNI_TO_VLAN_ID);
             TUNNELMAP_SET_VLAN(decap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_VRF(mapper_list))
           {
             ids_.tunnel_decap_id[TUNNEL_MAP_T_VIRTUAL_ROUTER] = create_tunnel_map(MAP_T::VNI_TO_VRID);
             TUNNELMAP_SET_VRF(decap_dedicated_mappers_);
           }

           if(IS_TUNNELMAP_SET_BRIDGE(mapper_list))
           {
             ids_.tunnel_encap_id[TUNNEL_MAP_T_BRIDGE] = create_tunnel_map(MAP_T::BRIDGE_TO_VNI);

             TUNNELMAP_SET_BRIDGE(decap_dedicated_mappers_);
           }
        }
    }

    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
        // FIXME: add code to remove already created objects
        return false;
    }

    return true;
}

bool VxlanTunnel::deleteTunnelHW(uint8_t mapper_list, 
                                 tunnel_map_src_t map_src, bool with_term)
{
    SWSS_LOG_INFO("Enter Delete Tunnel HW");
    try
    {
      sai_status_t ret;

      if(with_term)
      {
         ret = sai_tunnel_api->remove_tunnel_term_table_entry(ids_.tunnel_term_id);
         SWSS_LOG_INFO("term table delete reststatus = %d",ret);
      }

      ret = sai_tunnel_api->remove_tunnel(ids_.tunnel_id);
      SWSS_LOG_INFO("tunnel table delete reststatus = %d",ret);
      deleteMapperHW(mapper_list, map_src);
      total_diptunnel_del++;
    }

    catch (const std::runtime_error& error)
    {
      SWSS_LOG_ERROR("Error deleting tunnel %s: %s", tunnel_name_.c_str(), error.what());
      // FIXME: add code to remove already created objects
      return false;
    }

    active_ = false;

    return true;
}

//Creation of SAI Tunnel Object with multiple mapper types

bool VxlanTunnel::createTunnelHW(uint8_t mapper_list, 
                                 tunnel_map_src_t map_src, bool with_term)
{
    try
    {
        sai_ip_address_t ips, ipd, *ip=nullptr;
        swss::copy(ips, src_ip_);

        createMapperHW(mapper_list, map_src);

        ip = nullptr;
        if (!dst_ip_.isZero())
        {
            swss::copy(ipd, dst_ip_);
            ip = &ipd;
            total_diptunnel_add++;
        }

        ids_.tunnel_id = create_tunnel(&ids_, &ips, ip, gUnderlayIfId);

        if(with_term)
          ids_.tunnel_term_id = create_tunnel_termination(ids_.tunnel_id, ips, ip, gVirtualRouterId);

        active_ = true;
    }

    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
        // FIXME: add code to remove already created objects
        return false;
    }

    SWSS_LOG_INFO("Vxlan tunnel '%s' was created", tunnel_name_.c_str());
    return true;
}

void VxlanTunnel::deletePendingSIPTunnel()
{
   if((getDipTunnelCnt() == 0) && del_tnl_hw_pending)
   {
    uint8_t mapper_list=0;
    TUNNELMAP_SET_VLAN(mapper_list);
    TUNNELMAP_SET_VRF(mapper_list);
    deleteTunnelHW(mapper_list, USE_DEDICATED_ENCAP_DECAP);
    del_tnl_hw_pending = false;
    SWSS_LOG_INFO("Removing SIP Tunnel HW which is pending");
   }

   return;
}
int VxlanTunnel::getDipTunnelCnt()
{
  int ret;

  ret = (int)tnl_users_.size();
  return ret;
}

void VxlanTunnel::increment_spurious_imr_add(const std::string remote_vtep)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(remote_vtep); 
    if(it == tnl_users_.end())
      return ; 
    else
    {
      tnl_refcnts = it->second;
      tnl_refcnts.spurious_add_imr_refcnt++;
      tnl_users_[remote_vtep] = tnl_refcnts;
    }
}

void VxlanTunnel::increment_spurious_imr_del(const std::string remote_vtep)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(remote_vtep); 
    if(it == tnl_users_.end())
      return ; 
    else
    {
      tnl_refcnts = it->second;
      tnl_refcnts.spurious_del_imr_refcnt++;
      tnl_users_[remote_vtep] = tnl_refcnts;
    }
}

int VxlanTunnel::getDipTunnelRefCnt(const std::string remote_vtep)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(remote_vtep); 
    if(it == tnl_users_.end())
      return -1; 
    else
    {
      tnl_refcnts = it->second;
      return (tnl_refcnts.imr_refcnt + tnl_refcnts.mac_refcnt + tnl_refcnts.ip_refcnt);
    }
}

int VxlanTunnel::getDipTunnelIMRRefCnt(const std::string remote_vtep)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(remote_vtep); 
    if(it == tnl_users_.end())
      return -1; 
    else
    {
      tnl_refcnts = it->second;
      return (tnl_refcnts.imr_refcnt);
    }
}

int VxlanTunnel::getDipTunnelIPRefCnt(const std::string remote_vtep)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(remote_vtep);
    if(it == tnl_users_.end())
      return -1;
    else
    {
      tnl_refcnts = it->second;
      return (tnl_refcnts.ip_refcnt);
    }
}

void VxlanTunnel::updateDipTunnelRefCnt(bool inc, tunnel_refcnt_t& tnl_refcnts, 
                                        tunnel_user_type_e usr)
{
     switch(usr)
     {
       case TNL_SRC_IMR: 
       {
         if(inc)
             tnl_refcnts.imr_refcnt++;
         else
             tnl_refcnts.imr_refcnt--;

         break;
       }
       case TNL_SRC_MAC: 
       {
         if(inc)
             tnl_refcnts.mac_refcnt++;
         else
             tnl_refcnts.mac_refcnt--;

         break;
       }
       case TNL_SRC_IP: 
       {
         if(inc)
             tnl_refcnts.ip_refcnt++;
         else
             tnl_refcnts.ip_refcnt--;

         break;
       }
       default : break;
     }
}

VxlanTunnel* VxlanTunnel::getDipTunnel(const std::string dip)
{
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    tunnel_refcnt_t tnl_refcnts;

    it = tnl_users_.find(dip); 
    if(it == tnl_users_.end())
      return NULL; 
    else
    {
      tnl_refcnts = it->second;
      return(tnl_refcnts.dip_tunnel);
    }
}

bool VxlanTunnel::createDynamicDIPTunnel(const std::string dip, tunnel_user_type_e usr)
{
    uint8_t mapper_list = 0;
    tunnel_refcnt_t tnl_refcnts;
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    VxlanTunnel* dip_tunnel=NULL;
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

    it = tnl_users_.find(dip); 
    if(it == tnl_users_.end())
    {
       const string tunnel_name = "EVPN_"+dip;
       auto dipaddr = IpAddress(dip);
       dip_tunnel = (new VxlanTunnel(tunnel_name, src_ip_, dipaddr, TNL_CREATION_SRC_EVPN));
       tunnel_orch->addTunnel(tunnel_name,dip_tunnel);

       memset(&tnl_refcnts,0,sizeof(tunnel_refcnt_t));
       updateDipTunnelRefCnt(true,tnl_refcnts,usr);
       tnl_refcnts.dip_tunnel = dip_tunnel;
       tnl_users_[dip] = tnl_refcnts;

       TUNNELMAP_SET_VLAN(mapper_list);
       TUNNELMAP_SET_VRF(mapper_list);
       dip_tunnel->createTunnelHW(mapper_list,USE_COMMON_ENCAP_DECAP, FALSE);
       SWSS_LOG_NOTICE("Created P2P Tunnel remote IP %s ", dip.c_str());
    }
    else 
    {
       tnl_refcnts = it->second;
       updateDipTunnelRefCnt(true,tnl_refcnts,usr);
       tnl_users_[dip] = tnl_refcnts;
    }

    return true;
}

bool VxlanTunnel::deleteDynamicDIPTunnel(const std::string dip, tunnel_user_type_e usr, bool update_refcnt)
{
    uint8_t mapper_list = 0;
    tunnel_refcnt_t tnl_refcnts;
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    VxlanTunnel* dip_tunnel = NULL;
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    Port tunnelPort;

    SWSS_LOG_INFO("Delete Dynamic DIP Tunnel Enter");

    it = tnl_users_.find(dip); 
    if(it != tnl_users_.end())
    {
       tnl_refcnts = it->second;

       if(update_refcnt)
       {
         updateDipTunnelRefCnt(false,tnl_refcnts,usr);
         tnl_users_[dip] = tnl_refcnts;
       }

       SWSS_LOG_INFO("diprefcnt = %d",tnl_refcnts.imr_refcnt + tnl_refcnts.mac_refcnt + tnl_refcnts.ip_refcnt);
       
       if(tnl_refcnts.imr_refcnt + tnl_refcnts.mac_refcnt + tnl_refcnts.ip_refcnt)
         return true;

       if(tunnel_orch->getTunnelPort(dip, tunnelPort))
       {
         SWSS_LOG_NOTICE("DIP = %s Not deleting tunnel from HW as tunnelPort is not yet deleted. fdbcount = %d",
                          dip.c_str(),tunnelPort.m_fdb_count);
         return true;
       }

       dip_tunnel = tnl_refcnts.dip_tunnel;
       if(!dip_tunnel)
       {
         SWSS_LOG_INFO("DIP Tunnel is NULL unexpected");
         return false;
       }

       TUNNELMAP_SET_VLAN(mapper_list);
       TUNNELMAP_SET_VRF(mapper_list);
       dip_tunnel->deleteTunnelHW(mapper_list,USE_COMMON_ENCAP_DECAP, FALSE);

       tnl_users_.erase(dip);

       const string tunnel_name = "EVPN_"+dip;
       tunnel_orch->delTunnel(tunnel_name);
       SWSS_LOG_NOTICE("P2P Tunnel deleted : %s", tunnel_name.c_str());
    }
    else 
    {
       SWSS_LOG_WARN("Unable to find dynamic tunnel for deletion");
    }

    return true;
}

//------------------- VxlanTunnelOrch Implementation --------------------------//

sai_object_id_t
VxlanTunnelOrch::createNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    SWSS_LOG_ENTER();

    if(!isTunnelExists(tunnelName))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' does not exists", tunnelName.c_str());
        return SAI_NULL_OBJECT_ID;
    }

    SWSS_LOG_NOTICE("NH tunnel create for %s, ip %s, mac %s, vni %d",
                     tunnelName.c_str(), ipAddr.to_string().c_str(), 
                     macAddress.to_string().c_str(), vni);

    auto tunnel_obj = getVxlanTunnel(tunnelName);
    sai_object_id_t nh_id, tunnel_id = tunnel_obj->getTunnelId();

    if ((nh_id = tunnel_obj->getNextHop(ipAddr, macAddress, vni)) != SAI_NULL_OBJECT_ID)
    {
        tunnel_obj->incNextHopRefCount(ipAddr, macAddress, vni);
        return nh_id;
    }

    sai_ip_address_t host_ip;
    swss::copy(host_ip, ipAddr);

    sai_mac_t mac, *macptr = nullptr;
    if (macAddress)
    {
        memcpy(mac, macAddress.getMac(), ETHER_ADDR_LEN);
        macptr = &mac;
    }

    if (create_nexthop_tunnel(host_ip, vni, macptr, tunnel_id, &nh_id) != SAI_STATUS_SUCCESS)
    {
        string err_msg = "NH tunnel create failed for " + ipAddr.to_string() + " " + to_string(vni);
        throw std::runtime_error(err_msg);
    }

    //Store the nh tunnel id
    tunnel_obj->updateNextHop(ipAddr, macAddress, vni, nh_id);

    SWSS_LOG_INFO("NH vxlan tunnel was created for %s, id 0x%" PRIx64, tunnelName.c_str(), nh_id);
    return nh_id;
}

bool
VxlanTunnelOrch::removeNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("NH tunnel remove for %s, ip %s, mac %s, vni %d",
                    tunnelName.c_str(), ipAddr.to_string().c_str(), 
                    macAddress.to_string().c_str(), vni);

    if(!isTunnelExists(tunnelName))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' does not exists", tunnelName.c_str());
        return false;
    }

    auto tunnel_obj = getVxlanTunnel(tunnelName);

    //Delete request for the nh tunnel id
    return tunnel_obj->removeNextHop(ipAddr, macAddress, vni);
}

bool VxlanTunnelOrch::createVxlanTunnelMap(string tunnelName, tunnel_map_type_t map, uint32_t vni,
                                           sai_object_id_t encap, sai_object_id_t decap, uint8_t encap_ttl)
{
    SWSS_LOG_ENTER();

    if(!isTunnelExists(tunnelName))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' does not exists", tunnelName.c_str());
        return false;
    }

    auto tunnel_obj = getVxlanTunnel(tunnelName);

    if (!tunnel_obj->isActive())
    {
        if (map == TUNNEL_MAP_T_VIRTUAL_ROUTER)
        {
            tunnel_obj->createTunnel(MAP_T::VRID_TO_VNI, MAP_T::VNI_TO_VRID, encap_ttl);
        }
        else if (map == TUNNEL_MAP_T_BRIDGE)
        {
            tunnel_obj->createTunnel(MAP_T::BRIDGE_TO_VNI, MAP_T::VNI_TO_BRIDGE, encap_ttl);
        }
    }

    tunnel_obj->vlan_vrf_vni_count++;

    try
    {
        /*
         * Create encap and decap mapper
         */
        auto encap_id = tunnel_obj->addEncapMapperEntry(encap, vni);
        auto decap_id = tunnel_obj->addDecapMapperEntry(decap, vni);

        tunnel_obj->insertMapperEntry(encap_id, decap_id, vni);

        SWSS_LOG_DEBUG("Vxlan tunnel encap entry '%" PRIx64 "' decap entry '0x%" PRIx64 "'", encap_id, decap_id);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error adding tunnel map entry. Tunnel: %s. Error: %s",
                       tunnelName.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan map for tunnel '%s' and vni '%d' was created",
            tunnelName.c_str(), vni);
    return true;
}

bool VxlanTunnelOrch::removeVxlanTunnelMap(string tunnelName, uint32_t vni)
{
    SWSS_LOG_ENTER();

    if(!isTunnelExists(tunnelName))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' does not exists", tunnelName.c_str());
        return false;
    }

    auto tunnel_obj = getVxlanTunnel(tunnelName);

    if (!tunnel_obj->isActive())
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' is not Active", tunnelName.c_str());
        return false;
    }

    try
    {
        /*
         * Delete encap and decap mapper
         */

        std::pair<sai_object_id_t, sai_object_id_t> mapper = tunnel_obj->getMapperEntry(vni);

        remove_tunnel_map_entry(mapper.first);
        remove_tunnel_map_entry(mapper.second);

        SWSS_LOG_DEBUG("Vxlan tunnel encap entry '%" PRIx64 "' decap entry '0x%" PRIx64 "'", mapper.first, mapper.second);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error removing tunnel map entry. Tunnel: %s. Error: %s",
                       tunnelName.c_str(), error.what());
        return false;
    }

    // Update the map count and if this is the last mapping entry 
    // make SAI calls to delete the tunnel and tunnel termination objects.

    tunnel_obj->vlan_vrf_vni_count--;
    if(tunnel_obj->vlan_vrf_vni_count == 0)
    {
       auto tunnel_term_id = vxlan_tunnel_table_[tunnelName].get()->getTunnelTermId();
       try
       {
           remove_tunnel_termination(tunnel_term_id);
       }
       catch(const std::runtime_error& error)
       {
           SWSS_LOG_ERROR("Error removing tunnel term entry. Tunnel: %s. Error: %s", tunnelName.c_str(), error.what());
           return false;
       }

       auto tunnel_id = vxlan_tunnel_table_[tunnelName].get()->getTunnelId();
       try
       {
           remove_tunnel(tunnel_id);
       }
       catch(const std::runtime_error& error)
       {
           SWSS_LOG_ERROR("Error removing tunnel entry. Tunnel: %s. Error: %s", tunnelName.c_str(), error.what());
           return false;
       }
    }

    SWSS_LOG_NOTICE("Vxlan map entry deleted for tunnel '%s' with vni '%d'", tunnelName.c_str(), vni);
    return true;
}

bool VxlanTunnelOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto src_ip = request.getAttrIP("src_ip");
    if (!src_ip.isV4())
    {
        SWSS_LOG_ERROR("Wrong format of the attribute: 'src_ip'. Currently only IPv4 address is supported");
        return true;
    }

    IpAddress dst_ip;
    auto attr_names = request.getAttrFieldNames();
    if (attr_names.count("dst_ip") == 0)
    {
        if(src_ip.isV4()) {
            dst_ip = IpAddress("0.0.0.0");
        } else {
            dst_ip = IpAddress("::");
        }
    }
    else
    {
        dst_ip = request.getAttrIP("dst_ip");
        if((src_ip.isV4() && !dst_ip.isV4()) ||
               (!src_ip.isV4() && dst_ip.isV4())) {
            SWSS_LOG_ERROR("Format mismatch: 'src_ip' and 'dst_ip' must be of the same family");
            return true;
	}
    }

    const auto& tunnel_name = request.getKeyString(0);

    if(isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' is already exists", tunnel_name.c_str());
        return true;
    }

    vxlan_tunnel_table_[tunnel_name] = std::unique_ptr<VxlanTunnel>(new VxlanTunnel(tunnel_name, src_ip, dst_ip), TNL_CREATION_SRC_CLI);

    SWSS_LOG_NOTICE("Vxlan tunnel '%s' was added", tunnel_name.c_str());
    return true;
}

bool VxlanTunnelOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const auto& tunnel_name = request.getKeyString(0);

    if(!isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return true;
    }

    auto vtep_ptr = getVxlanTunnel(tunnel_name);
    if(vtep_ptr && vtep_ptr->del_tnl_hw_pending)
    {
      SWSS_LOG_WARN("VTEP %s not deleted as hw delete is pending", tunnel_name.c_str());
      return false;
    }

    vxlan_tunnel_table_.erase(tunnel_name);

    SWSS_LOG_NOTICE("Vxlan tunnel '%s' was removed", tunnel_name.c_str());

    return true;
}

bool  VxlanTunnelOrch::addTunnelUser(const std::string remote_vtep, uint32_t vni_id, 
                                    uint32_t vlan, tunnel_user_type_e usr,
                                    sai_object_id_t vrf_id)
{
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    VxlanTunnel* dip_tunnel=NULL;

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    Port tunport;

    if(TNL_SRC_MAC == usr) return true;

    auto vtep_ptr = evpn_orch->getEVPNVtep();

    if(!vtep_ptr)
    {
      SWSS_LOG_WARN("Unable to find EVPN VTEP. user=%d remote_vtep=%s",
                     usr,remote_vtep.c_str());
      return false;
    }

    if (!vtep_ptr->isActive())
    {
      SWSS_LOG_WARN("VTEP not yet active.user=%d remote_vtep=%s",
                      usr,remote_vtep.c_str()); 
      return false;
    }

    vtep_ptr->createDynamicDIPTunnel(remote_vtep, usr);
    dip_tunnel = vtep_ptr->getDipTunnel(remote_vtep);

    SWSS_LOG_NOTICE("diprefcnt for remote %s = %d",
                     remote_vtep.c_str(), vtep_ptr->getDipTunnelRefCnt(remote_vtep));

    if(!tunnel_orch->getTunnelPort(remote_vtep, tunport))
    {
      Port tunnelPort;
      auto port_tunnel_name = "Port_EVPN_" + remote_vtep;
      SWSS_LOG_NOTICE("Creating Tunnel Port name = %s", port_tunnel_name.c_str());
      gPortsOrch->addTunnel(port_tunnel_name,dip_tunnel->getTunnelId(), false);
      SWSS_LOG_NOTICE("Created Tunnel Port name = %s", port_tunnel_name.c_str());
      gPortsOrch->getPort(port_tunnel_name,tunnelPort);
      gPortsOrch->addBridgePort(tunnelPort);
      SWSS_LOG_NOTICE("Created Tunnel BridgePort name = %s", port_tunnel_name.c_str());
    }

    // TODO : Add Encap Mapper entry for DCI usecase

    return true;
}

bool  VxlanTunnelOrch::delTunnelUser(const std::string remote_vtep, uint32_t vni_id, 
                                    uint32_t vlan, tunnel_user_type_e usr,
                                    sai_object_id_t vrf_id)
{
    // TODO : Del Encap Mapper entry for DCI usecase

    if(TNL_SRC_MAC == usr) return true;

    // If this is the last request to delete the tunnel.
    auto tunnel_name = "EVPN_" + remote_vtep;

    //if (isTunnelExists(tunnel_name))
    {
        auto port_tunnel_name = "Port_EVPN_" + remote_vtep;
        EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();

        auto vtep_ptr = evpn_orch->getEVPNVtep();

        if(!vtep_ptr) 
        {
          SWSS_LOG_WARN("Unable to find VTEP. remote=%s vlan=%d usr=%d",remote_vtep.c_str(), vlan, usr);
          return true;
        }

        Port tunnelPort;
        gPortsOrch->getPort(port_tunnel_name,tunnelPort);

        if((vtep_ptr->getDipTunnelRefCnt(remote_vtep) == 1) &&
           tunnelPort.m_fdb_count == 0)
        {
          bool ret;

          ret = gPortsOrch->removeBridgePort(tunnelPort);
          if(!ret) 
          {
            SWSS_LOG_ERROR("Remove Bridge port failed for remote = %s fdbcount = %d", 
                            remote_vtep.c_str(), tunnelPort.m_fdb_count);
            return true;
          }
        
          gPortsOrch->removeTunnel(tunnelPort);
        }

        vtep_ptr->deleteDynamicDIPTunnel(remote_vtep, usr);
        SWSS_LOG_NOTICE("diprefcnt for remote %s = %d",
                         remote_vtep.c_str(), vtep_ptr->getDipTunnelRefCnt(remote_vtep));

        vtep_ptr->deletePendingSIPTunnel();

    }

    return true;
}

void VxlanTunnelOrch::deleteTunnelPort(Port &tunnelPort)
{
    bool ret;
    std::map<const std::string, tunnel_refcnt_t>::iterator it;
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    std::string remote_vtep;

    auto vtep_ptr = evpn_orch->getEVPNVtep();

    SWSS_LOG_INFO("Delete Tunnelport Enter");
    if(!vtep_ptr) 
    {
      SWSS_LOG_WARN("Unable to find VTEP. tunnelPort=%s",tunnelPort.m_alias.c_str());
      return;
    }

    getTunnelDIPFromPort(tunnelPort, remote_vtep);

    //If there are IMR/IP routes to the remote VTEP then ignore this call
    if(vtep_ptr->getDipTunnelRefCnt(remote_vtep))
      return;

    // Remove Bridge port and Port objects for this DIP tunnel
    ret = gPortsOrch->removeBridgePort(tunnelPort);
    if(!ret) 
    {
      SWSS_LOG_ERROR("Remove Bridge port failed for remote = %s fdbcount = %d", 
                      remote_vtep.c_str(), tunnelPort.m_fdb_count);
      return;
    }
    gPortsOrch->removeTunnel(tunnelPort);

    // Remove DIP Tunnel HW 
    vtep_ptr->deleteDynamicDIPTunnel(remote_vtep, TNL_SRC_IMR, false);
    SWSS_LOG_NOTICE("diprefcnt for remote %s = %d",
                     remote_vtep.c_str(), vtep_ptr->getDipTunnelRefCnt(remote_vtep));

    // Remove SIP Tunnel HW which might be pending on delete
    vtep_ptr->deletePendingSIPTunnel();

    return ;
}

void VxlanTunnelOrch::getTunnelName(string& tunnel_portname, string& tunnel_name)
{
   tunnel_name = tunnel_portname;
   tunnel_name.erase(0, sizeof("Port_")-1);

   SWSS_LOG_DEBUG("tunnel name = %s",tunnel_name.c_str());

   return;
}

#if 0
void VxlanTunnelOrch::getTunnelPortname(string& dip, string& tunnel_portname)
{
   tunnel_port_name = "Port_EVPN_" + dip;

   return;
}
#endif

void VxlanTunnelOrch:: getTunnelDIPFromPort(Port& tunnelPort, string& remote_vtep)
{
    remote_vtep = tunnelPort.m_alias;
    remote_vtep.erase(0,sizeof("Port_EVPN_")-1);
}


void VxlanTunnelOrch::updateDbTunnelOperStatus(string tunnel_portname, 
                                               sai_port_oper_status_t status)
{
   std::vector<FieldValueTuple> fvVector;
   std::string tunnel_name;

   if(status == SAI_PORT_OPER_STATUS_UP)
     fvVector.emplace_back("operstatus", "up");
   else
     fvVector.emplace_back("operstatus", "down");

   getTunnelName(tunnel_portname, tunnel_name);

   m_stateVxlanTable.set(tunnel_name, fvVector);
}

void VxlanTunnelOrch::addRemoveStateTableEntry(string tunnel_name, 
                                           IpAddress& sip, IpAddress& dip, 
                                           tunnel_creation_src_t src, bool add)

{
    std::vector<FieldValueTuple> fvVector, tmpFvVector;
    WarmStart::WarmStartState state;

    WarmStart::getWarmStartState("orchagent",state);

    if(add)
    {
      // Add tunnel entry only for non-warmboot case or WB with new tunnel coming up
      // during WB
      if ( (state != WarmStart::INITIALIZED) || 
           !m_stateVxlanTable.get(tunnel_name, tmpFvVector))
      {
        fvVector.emplace_back("src_ip", (sip.to_string()).c_str());
        fvVector.emplace_back("dst_ip", (dip.to_string()).c_str());

        if(src == TNL_CREATION_SRC_CLI)
          fvVector.emplace_back("tnl_src", "CLI");
        else 
          fvVector.emplace_back("tnl_src", "EVPN");

        fvVector.emplace_back("operstatus", "down");
        m_stateVxlanTable.set(tunnel_name, fvVector);
        SWSS_LOG_INFO("adding tunnel %s during warmboot", tunnel_name.c_str());
      }
      else
        SWSS_LOG_NOTICE("Skip adding tunnel %s during warmboot", tunnel_name.c_str());
    }
    else
    {
      m_stateVxlanTable.del(tunnel_name);
    }
}

bool VxlanTunnelOrch::getTunnelPort(const std::string& remote_vtep,Port& tunnelPort)
{
    auto port_tunnel_name = "Port_EVPN_" + remote_vtep;

    bool ret = gPortsOrch->getPort(port_tunnel_name,tunnelPort);

    SWSS_LOG_INFO("getTunnelPort and getPort return ret=%d name=%s",
                  ret,port_tunnel_name.c_str());

    return ret;
}

//------------------- VXLAN_TUNNEL_MAP Table --------------------------//

bool VxlanTunnelMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    sai_vlan_id_t vlan_id = (sai_vlan_id_t)request.getAttrVlan("vlan");
    Port tempPort;

    const auto full_tunnel_map_entry_name = request.getFullKey();
    SWSS_LOG_NOTICE("Full name = %s",full_tunnel_map_entry_name.c_str());

    if (isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_ERROR("Vxlan tunnel map '%s' already exist", 
                      full_tunnel_map_entry_name.c_str());
        return true;
    }

    if(!gPortsOrch->getVlanByVlanId(vlan_id, tempPort))
    {
        SWSS_LOG_WARN("Vxlan tunnel map vlan id doesn't exist: %d", vlan_id);
        return false;
    }

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan tunnel map vni id is too big: %d", vni_id);
        return true;
    }

    tempPort.m_vnid = (uint32_t) vni_id;

    auto tunnel_name = request.getKeyString(0);
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
 
    // The hw delete is pending due to an earlier incomplete operation. 
    // process this add event when the deletion is complete. 
    if(tunnel_obj->del_tnl_hw_pending)
    {
      SWSS_LOG_WARN("Tunnel Mapper deletion is pending");
      return false;
    }

    if (!tunnel_obj->isActive())
    {

        //@Todo, currently only decap mapper is allowed
        //tunnel_obj->createTunnel(MAP_T::MAP_TO_INVALID, MAP_T::VNI_TO_VLAN_ID);
        uint8_t mapper_list = 0;
        TUNNELMAP_SET_VLAN(mapper_list);
        TUNNELMAP_SET_VRF(mapper_list);
        tunnel_obj->createTunnelHW(mapper_list,USE_DEDICATED_ENCAP_DECAP);
    }

    const auto tunnel_map_id = tunnel_obj->getDecapMapId(TUNNEL_MAP_T_VLAN);
    const auto tunnel_map_entry_name = request.getKeyString(1);

    tunnel_obj->vlan_vrf_vni_count++;
    SWSS_LOG_INFO("vni count increased to %d",tunnel_obj->vlan_vrf_vni_count);

    try
    {
        auto tunnel_map_entry_id = create_tunnel_map_entry(MAP_T::VNI_TO_VLAN_ID,
                                                           tunnel_map_id, vni_id, vlan_id);
        vxlan_tunnel_map_table_[full_tunnel_map_entry_name].map_entry_id = 
                                                             tunnel_map_entry_id;
        vxlan_tunnel_map_table_[full_tunnel_map_entry_name].vlan_id = 
                                                             vlan_id;
        vxlan_tunnel_map_table_[full_tunnel_map_entry_name].vni_id = 
                                                             vni_id;
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_WARN("Error adding tunnel map entry. Tunnel: %s. Entry: %s. Error: %s",
            tunnel_name.c_str(), tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    tunnel_orch->addVlanMappedToVni(vni_id, vlan_id);
    VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();
    if (0 == vrf_orch->getL3VniVlan(vni_id))
    {
        SWSS_LOG_NOTICE("update l3vni %d, vlan %d", vni_id, vlan_id);
        vrf_orch->updateL3VniVlan(vni_id, vlan_id);
    }

    SWSS_LOG_NOTICE("Vxlan tunnel map entry '%s' for tunnel '%s' was created",
                   tunnel_map_entry_name.c_str(), tunnel_name.c_str());

    return true;
}

bool VxlanTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    Port vlanPort;
    const auto& tunnel_name = request.getKeyString(0);
    const auto& tunnel_map_entry_name = request.getKeyString(1);
    const auto& full_tunnel_map_entry_name = request.getFullKey();


    if (!isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_WARN("Vxlan tunnel map '%s' doesn't exist", full_tunnel_map_entry_name.c_str());
        return true;
    }

    auto vlan_id = (sai_vlan_id_t) vxlan_tunnel_map_table_[full_tunnel_map_entry_name].vlan_id;
    if(!gPortsOrch->getVlanByVlanId(vlan_id, vlanPort))
    {
        SWSS_LOG_ERROR("Delete VLAN-VNI map.vlan id doesn't exist: %d", vlan_id);
        return true;
    }

    vlanPort.m_vnid = (uint32_t) 0xFFFFFFFF;

    auto tunnel_map_entry_id = vxlan_tunnel_map_table_[full_tunnel_map_entry_name];
    try
    {
        remove_tunnel_map_entry(tunnel_map_entry_id);
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error removing tunnel map %s: %s", full_tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    vxlan_tunnel_map_table_.erase(full_tunnel_map_entry_name);

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    tunnel_obj->vlan_vrf_vni_count--;

    SWSS_LOG_NOTICE("vni count = %d",tunnel_obj->vlan_vrf_vni_count);

    // Update the map count and if this is the last mapping entry 
    // make SAI calls to delete the tunnel and tunnel termination objects.

    if(tunnel_obj->vlan_vrf_vni_count == 0)
    {
      // If there are Dynamic DIP Tunnels referring to this SIP Tunnel 
      // then mark it as pending for delete. 
      if(tunnel_obj->getDipTunnelCnt() == 0)
      {
         uint8_t mapper_list=0;
         TUNNELMAP_SET_VLAN(mapper_list);
         TUNNELMAP_SET_VRF(mapper_list);
         tunnel_obj->deleteTunnelHW(mapper_list, USE_DEDICATED_ENCAP_DECAP);
      }
      else
      {
        tunnel_obj->del_tnl_hw_pending = true;
        SWSS_LOG_WARN("Postponing the SIP Tunnel HW deletion DIP Tunnel count = %d",
                      tunnel_obj->getDipTunnelCnt());
      }
    }

    vector<string> map_entries = tokenize(tunnel_map_entry_name, '_');
    SWSS_LOG_INFO("Vxlan tunnel map '%s' size %ld", tunnel_map_entry_name.c_str(), 
                                                    map_entries.size());
    if (map_entries.size() == 3)
    {
        SWSS_LOG_INFO("Vxlan tunnel map %s, %s, %s ", map_entries[0].c_str(), 
                                                      map_entries[1].c_str(), 
                                                      map_entries[2].c_str());
        uint32_t vni_id = static_cast<uint32_t>(stoul(map_entries[1]));
        if (vni_id) {
            tunnel_orch->delVlanMappedToVni(vni_id);
        }
    }
    SWSS_LOG_NOTICE("Vxlan tunnel map entry '%s' for tunnel '%s' was removed",
                   tunnel_map_entry_name.c_str(), tunnel_name.c_str());

    return true;
}

//------------------- VXLAN_VRF_MAP Table --------------------------//

bool VxlanVrfMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto tunnel_name = request.getKeyString(0);
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("Vxlan tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan vni id is too big: %d", vni_id);
        return true;
    }

    const auto full_map_entry_name = request.getFullKey();
    if (isVrfMapExists(full_map_entry_name))
    {
        SWSS_LOG_ERROR("Vxlan map '%s' is already exist", full_map_entry_name.c_str());
        return true;
    }

    auto tunnel_obj = tunnel_orch->getVxlanTunnel(tunnel_name);
    sai_object_id_t vrf_id;

    string vrf_name = request.getAttrString("vrf");
    VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();

    SWSS_LOG_NOTICE("VRF VNI mapping '%s' update vrf %s, vni %d",
            full_map_entry_name.c_str(), vrf_name.c_str(), vni_id);
    if (vrf_orch->isVRFexists(vrf_name))
    {
        if (!tunnel_obj->isActive()) {
            tunnel_obj->createTunnel(MAP_T::VRID_TO_VNI, MAP_T::VNI_TO_VRID);
        }
        vrf_id = vrf_orch->getVRFid(vrf_name);
    }
    else
    {
        SWSS_LOG_WARN("Vrf '%s' hasn't been created yet", vrf_name.c_str());
        return false;
    }

    const auto tunnel_map_entry_name = request.getKeyString(1);
    vrf_map_entry_t entry;
    try
    {
        /*
         * Create encap and decap mapper
         */
        entry.encap_id = tunnel_obj->addEncapMapperEntry(vrf_id, vni_id);
        entry.decap_id = tunnel_obj->addDecapMapperEntry(vrf_id, vni_id);

        SWSS_LOG_DEBUG("Vxlan tunnel encap entry '%" PRIx64 "' decap entry '0x%" PRIx64 "'",
                entry.encap_id, entry.decap_id);

        vxlan_vrf_table_[full_map_entry_name] = entry;
        vxlan_vrf_tunnel_[vrf_name] = tunnel_obj->getTunnelId();
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error adding tunnel map entry. Tunnel: %s. Entry: %s. Error: %s",
            tunnel_name.c_str(), tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("Vxlan vrf map entry '%s' for tunnel '%s' was created",
                    tunnel_map_entry_name.c_str(), tunnel_name.c_str());
    return true;
}

bool VxlanVrfMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();
    const auto full_map_entry_name = request.getFullKey();

    if (!isVrfMapExists(full_map_entry_name))
    {
        SWSS_LOG_ERROR("VxlanVrfMapOrch Vxlan map '%s' do not exist", full_map_entry_name.c_str());
        return false;
    }

    size_t pos = full_map_entry_name.find("Vrf");
    if (pos == string::npos) {
        SWSS_LOG_ERROR("VxlanVrfMapOrch no VRF in Vxlan map '%s'", full_map_entry_name.c_str());
        return false;
    }
    string vrf_name = full_map_entry_name.substr(pos);

    if (!vrf_orch->isVRFexists(vrf_name))
    {
        SWSS_LOG_ERROR("VxlanVrfMapOrch VRF '%s' not present", vrf_name.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("VxlanVrfMapOrch VRF VNI mapping '%s' remove vrf %s", full_map_entry_name.c_str(), vrf_name.c_str());
    vrf_map_entry_t entry;
    try
    {
        /*
         * Remove encap and decap mapper
         */
        entry = vxlan_vrf_table_[full_map_entry_name];

        SWSS_LOG_NOTICE("VxlanVrfMapOrch Vxlan tunnel VRF encap entry '%lx' decap entry '0x%lx'",
                entry.encap_id, entry.decap_id);

        remove_tunnel_map_entry(entry.encap_id);
        vrf_orch->decreaseVrfRefCount(vrf_name);
        remove_tunnel_map_entry(entry.decap_id);
        vrf_orch->decreaseVrfRefCount(vrf_name);
        vxlan_vrf_table_.erase(full_map_entry_name);
        vxlan_vrf_tunnel_.erase(vrf_name);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("VxlanVrfMapOrch Error removing tunnel map entry. Entry: %s. Error: %s",
            full_map_entry_name.c_str(), error.what());
        return false;
    }

    SWSS_LOG_NOTICE("VxlanVrfMapOrch Vxlan vrf map entry '%s' is removed. VRF Refcnt %d", full_map_entry_name.c_str(),
            vrf_orch->getVrfRefCount(vrf_name));
    return true;
}

//------------------- EVPN_REMOTE_VNI Table --------------------------//

bool EvpnRemoteVniOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    // Extract DIP and tunnel
    auto remote_vtep = request.getKeyString(1);

    // Extract VLAN and VNI
    auto vlan_name = request.getKeyString(0);
    sai_vlan_id_t vlan_id = (sai_vlan_id_t) stoi(vlan_name.substr(4));
    //sai_vlan_id_t vlan_id = (sai_vlan_id_t)request.getAttrVlan("vlan");

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan tunnel map vni id is too big: %d", vni_id);
        return true;
    }

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    Port tunnelPort, vlanPort;

    if(!gPortsOrch->getVlanByVlanId(vlan_id, vlanPort))
    {
        SWSS_LOG_WARN("Vxlan tunnel map vlan id doesn't exist: %d", vlan_id);
        return false;
    }

    if(tunnel_orch->getTunnelPort(remote_vtep,tunnelPort))
    {
        SWSS_LOG_INFO("Vxlan tunnelPort exists: %s", remote_vtep.c_str());

        if(gPortsOrch->isVlanMember(vlanPort, tunnelPort))
        {
           EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
           auto vtep_ptr = evpn_orch->getEVPNVtep();
           if(!vtep_ptr)
           {
             SWSS_LOG_WARN("Remote VNI add: VTEP not found. remote=%s vid=%d",
                           remote_vtep.c_str(),vlan_id);
             return true;
           }
           SWSS_LOG_WARN("tunnelPort %s already member of vid %d", 
                           remote_vtep.c_str(),vlan_id);
           vtep_ptr->increment_spurious_imr_add(remote_vtep);
           return true;
        }
    }

    tunnel_orch->addTunnelUser(remote_vtep, vni_id, vlan_id, TNL_SRC_IMR);

    if(!tunnel_orch->getTunnelPort(remote_vtep,tunnelPort))
    {
      SWSS_LOG_WARN("Vxlan tunnelPort doesn't exist: %s", remote_vtep.c_str());
      return false;
    }

    // SAI Call to add tunnel to the VLAN flood domain

    string tagging_mode = "untagged"; 
    gPortsOrch->addVlanMember(vlanPort, tunnelPort, tagging_mode);

    SWSS_LOG_INFO("remote_vtep=%s vni=%d vlanid=%d ",
                   remote_vtep.c_str(), vni_id, vlan_id);

    return true;
}

bool EvpnRemoteVniOrch::delOperation(const Request& request)
{
    bool ret;
    SWSS_LOG_ENTER();

    // Extract DIP and tunnel
    auto remote_vtep = request.getKeyString(1);

    // Extract VLAN and VNI
    auto vlan_name = request.getKeyString(0);
    sai_vlan_id_t vlan_id = (sai_vlan_id_t)stoi(vlan_name.substr(4));

    auto vni_id  = static_cast<sai_uint32_t>(request.getAttrUint("vni"));
    if (vni_id >= 1<<24)
    {
        SWSS_LOG_ERROR("Vxlan tunnel map vni id is too big: %d", vni_id);
        return true;
    }

    // SAI Call to add tunnel to the VLAN flood domain

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    Port vlanPort, tunnelPort;
    if(!gPortsOrch->getVlanByVlanId(vlan_id, vlanPort))
    {
        SWSS_LOG_WARN("Vxlan tunnel map vlan id doesn't exist: %d", vlan_id);
        return true;
    }

    if(!tunnel_orch->getTunnelPort(remote_vtep,tunnelPort))
    {
        SWSS_LOG_WARN("RemoteVniDel getTunnelPort Fails: %s", remote_vtep.c_str());
        return true;
    }

    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    auto vtep_ptr = evpn_orch->getEVPNVtep();

    if(!vtep_ptr)
      {
        SWSS_LOG_WARN("Remote VNI del: VTEP not found. remote=%s vid=%d",
                       remote_vtep.c_str(),vlan_id);
        return true;
      }

    // If VLAN is not member of the tunnel and 
    // this is not a vlan which is pending delete
    // consider it as spurious.
#if 0
    SWSS_LOG_NOTICE("ismember=%d vlan_pend_del=%d vlanid=%d",gPortsOrch->isVlanMember(vlanPort, tunnelPort), tunnelPort.m_tunnel_vlan_pending_delete, vlan_id);
        (vlan_id != tunnelPort.m_tunnel_vlan_pending_delete))
#endif

    if(!gPortsOrch->isVlanMember(vlanPort, tunnelPort))
    {
       SWSS_LOG_WARN("marking it as spurious tunnelPort %s not a member of vid %d", 
                      remote_vtep.c_str(), vlan_id);
       vtep_ptr->increment_spurious_imr_del(remote_vtep);
       return true;
    }

    if(gPortsOrch->isVlanMember(vlanPort, tunnelPort)) 
    {
       if(!gPortsOrch->removeVlanMember(vlanPort, tunnelPort))
       {
          SWSS_LOG_WARN("RemoteVniDel remove vlan member fails: %s",remote_vtep.c_str());
          return true;
       }
    }

    SWSS_LOG_WARN("imrcount=%d fdbcount=%d ",
                   vtep_ptr->getDipTunnelIMRRefCnt(remote_vtep), 
                   tunnelPort.m_fdb_count );

    ret = tunnel_orch->delTunnelUser(remote_vtep, vni_id, vlan_id, TNL_SRC_IMR);

    SWSS_LOG_INFO("remote_vtep=%s vni=%d vlanid=%d ",
                   remote_vtep.c_str(), vni_id, vlan_id);


    return ret;
}

//------------------- EVPN_NVO Table --------------------------//

bool EvpnNvoOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto nvo_name = request.getKeyString(0);
    auto vtep_name = request.getAttrString("source_vtep");

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();

    // FIXME: Currently assumes only 1 VTEP
    source_vtep_ptr = tunnel_orch->getVxlanTunnel(vtep_name);

    SWSS_LOG_INFO("evpnnvo: %s vtep : %s \n",nvo_name.c_str(), vtep_name.c_str());

    return true;
}

bool EvpnNvoOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto nvo_name = request.getKeyString(0);

    if(!source_vtep_ptr) 
    {
       SWSS_LOG_WARN("NVO Delete failed as VTEP Ptr is NULL");
       return true;
    }

    if(source_vtep_ptr->del_tnl_hw_pending)
    {
      SWSS_LOG_WARN("NVO not deleted as hw delete is pending");
      return false;
    }

    source_vtep_ptr = NULL;

    SWSS_LOG_INFO("NVO: %s \n",nvo_name.c_str());

    return true;
}

