/*
 * Copyright (c) 2020 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if (PELION_BORDER_ROUTER_WITH_NM == 0)
#include "mbed.h"
#include "kv_config.h"
#include "mbed-cloud-client/MbedCloudClient.h" // Required for new MbedCloudClient()
#include "factory_configurator_client.h"       // Required for fcc_* functions and FCC_* defines
#include "m2mresource.h"                       // Required for M2MResource
#include "key_config_manager.h"                // Required for kcm_factory_reset
#include "nanostack_heap_region.h"

#include "mbed-trace/mbed_trace.h"             // Required for mbed_trace_*

#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
#include "ip6string.h"
extern "C" {
    #include "ws_bbr_api.h"
};

#endif

// Pointers to the resources that will be created in main_application().
static MbedCloudClient *cloud_client;
static bool cloud_client_running = true;
static NetworkInterface *network = NULL;
static MeshInterface *mesh_network = NULL;

/* To-Do: Implement APIs to get interface ID */
int8_t backhaul_iface_id = 1;
int8_t mesh_iface_id = 2;

// Fake entropy needed for non-TRNG boards. Suitable only for demo devices.
const uint8_t MBED_CLOUD_DEV_ENTROPY[] = { 0xf6, 0xd6, 0xc0, 0x09, 0x9e, 0x6e, 0xf2, 0x37, 0xdc, 0x29, 0x88, 0xf1, 0x57, 0x32, 0x7d, 0xde, 0xac, 0xb3, 0x99, 0x8c, 0xb9, 0x11, 0x35, 0x18, 0xeb, 0x48, 0x29, 0x03, 0x6a, 0x94, 0x6d, 0xe8, 0x40, 0xc0, 0x28, 0xcc, 0xe4, 0x04, 0xc3, 0x1f, 0x4b, 0xc2, 0xe0, 0x68, 0xa0, 0x93, 0xe6, 0x3a };

#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
static M2MResource* _res_routing;
static M2MResource* _res_iid;
static Thread _thread_routing_update(osPriorityLow);
#endif

static M2MResource* m2m_get_res;
static M2MResource* m2m_put_res;
static M2MResource* m2m_post_res;
static M2MResource* m2m_deregister_res;
static M2MResource* m2m_factory_reset_res;
static SocketAddress sa;

static bool interface_connected = false;
extern "C" int ws_bbr_start(int8_t interface_id, int8_t bb_interface_id);

EventQueue queue(32 * EVENTS_EVENT_SIZE);
Thread t;
Mutex value_increment_mutex;
rtos::Semaphore sync_semaphore;

void print_client_ids(void)
{
    printf("Account ID: %s\n", cloud_client->endpoint_info()->account_id.c_str());
    printf("Endpoint name: %s\n", cloud_client->endpoint_info()->internal_endpoint_name.c_str());
    printf("Device ID: %s\n\n", cloud_client->endpoint_info()->endpoint_name.c_str());
}

void value_increment(void)
{
    value_increment_mutex.lock();
    m2m_get_res->set_value(m2m_get_res->get_value_int() + 1);
    printf("Counter %" PRIu64 "\n", m2m_get_res->get_value_int());
    value_increment_mutex.unlock();
}

void get_res_update(const char* /*object_name*/)
{
    printf("Counter resource set to %d\n", (int)m2m_get_res->get_value_int());
}

void put_res_update(const char* /*object_name*/)
{
    printf("PUT update %s\n", (int)m2m_put_res->get_value_string().c_str());
}

void execute_post(void* /*arguments*/)
{
    printf("POST executed\n");
}

void deregister_client(void)
{
    printf("Unregistering and disconnecting from the network.\n");
    cloud_client->close();
}

void deregister(void* /*arguments*/)
{
    printf("POST deregister executed\n");
    m2m_deregister_res->send_delayed_post_response();

    deregister_client();
}

void client_registered(void)
{
    printf("Client registered.\n");
    print_client_ids();
    sync_semaphore.release();
}

void client_unregistered(void)
{
    printf("Client unregistered.\n");
    (void) network->disconnect();
    cloud_client_running = false;
}

void factory_reset(void*)
{
    printf("POST factory reset executed\n");
    m2m_factory_reset_res->send_delayed_post_response();

    kcm_factory_reset();
}

void client_error(int err)
{
    printf("client_error(%d) -> %s\n", err, cloud_client->error_description());
}

void update_progress(uint32_t progress, uint32_t total)
{
    uint8_t percent = (uint8_t)((uint64_t)progress * 100 / total);
    printf("Update progress = %" PRIu8 "%%\n", percent);
}

void flush_stdin_buffer(void)
{
    FileHandle *debug_console = mbed::mbed_file_handle(STDIN_FILENO);
    while(debug_console->readable()) {
        char buffer[1];
        debug_console->read(buffer, 1);
    }
}

#define TRACE_GROUP "plat"

static void network_status_callback(nsapi_event_t status, intptr_t param)
{
#ifdef MCC_USE_MBED_EVENTS
    EventQueue *queue = mbed::mbed_event_queue();
#endif
    if (status == NSAPI_EVENT_CONNECTION_STATUS_CHANGE) {
        switch(param) {
            case NSAPI_STATUS_GLOBAL_UP:
#ifdef MCC_USE_MBED_EVENTS
                if (!interface_connected) {
                    queue->break_dispatch();
                }
#endif
                interface_connected = true;

#if MBED_CONF_MBED_TRACE_ENABLE
                tr_info("NSAPI_STATUS_GLOBAL_UP");
#else
                printf("NSAPI_STATUS_GLOBAL_UP\n");
#endif
                break;
            case NSAPI_STATUS_LOCAL_UP:
#if MBED_CONF_MBED_TRACE_ENABLE
                tr_info("NSAPI_STATUS_LOCAL_UP");
#else
                printf("NSAPI_STATUS_LOCAL_UP\n");
#endif
                break;
            case NSAPI_STATUS_DISCONNECTED:
#if MBED_CONF_MBED_TRACE_ENABLE
                tr_info("NSAPI_STATUS_DISCONNECTED");
#else
                printf("NSAPI_STATUS_DISCONNECTED\n");
#endif
                interface_connected = false;
                break;
            case NSAPI_STATUS_CONNECTING:
#if MBED_CONF_MBED_TRACE_ENABLE
                tr_info("NSAPI_STATUS_CONNECTING");
#else
                printf("NSAPI_STATUS_CONNECTING\n");
#endif
                break;
            case NSAPI_STATUS_ERROR_UNSUPPORTED:
            default:
#if MBED_CONF_MBED_TRACE_ENABLE
                tr_info("NSAPI_STATUS_ERROR_UNSUPPORTED");
#else
                printf("NSAPI_STATUS_ERROR_UNSUPPORTED\n");
#endif
                break;
        }
    }
}

/*--------------------------------------------------------------------------*/
static int8_t mesh_interface_up (void)
{
    nsapi_error_t err;

    // Connect with NetworkInterface
    printf("Connect to mesh_network\n");
    mesh_network = MeshInterface::get_default_instance();
    if (mesh_network == NULL) {
        printf("Failed to get default mesh_network Interface\n");
        return -1;
    }

    if (!nsapi_create_stack(mesh_network)) {
        printf("ERROR: nsapi_create_stack() failed!\n");
        return -1;
    }

    // Delete the callback first in case the API is being called multiple times to prevent creation of multiple callbacks.
    mesh_network->remove_event_listener(mbed::callback(&network_status_callback));
    mesh_network->add_event_listener(mbed::callback(&network_status_callback));
    printf("Connecting with interface: MESH\n");
    interface_connected = false;

    err = mesh_network->connect();
    if (err == NSAPI_ERROR_OK || err == NSAPI_ERROR_IS_CONNECTED) {
        ws_bbr_start(mesh_iface_id, backhaul_iface_id);
        printf ("MESH UP\n");
        interface_connected = true;
#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
        // Wait until dodag information is available.
        // This information is needed for lwm2m registration
        int ret = -1;
        bbr_information_t bbr_info = {0};
        char addr[45] = {0};
        int counter = 0;
        do {
            ThisThread::sleep_for(10 * 1000);
            ret = ws_bbr_info_get(mesh_iface_id, &bbr_info);
            counter++;
        } while(ret != 0 && counter < 30);

        if (ret != 0) {
            printf ("Failed to get dodag information!\n");
            return -1;
        }

        ip6tos(bbr_info.dodag_id, addr);
        printf("Mesh up, dodag info: %s\n", addr);
        uint16_t panid;
        ws_bbr_pan_configuration_get(mesh_iface_id, &panid);

#endif
        return 0;
    }
    return -1;
}

#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
void trace_route_info(bbr_route_info_t* ptr)
{
    if((ptr->target==NULL)||(ptr->parent==NULL))
    {
        tr_err("[SMeter] ptr->target/parent==NULL\n");
        return;
    }

    tr_warn("[SMeter] Node:: %s ==> Parent:: %s \n", trace_array(ptr->target, 8), trace_array(ptr->parent, 8));
}

void read_routing_table()
{
    bbr_information_t bbr_info = {0};
    int mesh_size = 0;
    int ret = 0;
    int len = 0;

    while(true)
    {
        ThisThread::sleep_for(20*1000);
        tr_warn("[SMeter] Get Routing Table\r\n");
        ret = ws_bbr_info_get(mesh_iface_id, &bbr_info);
        if (ret == 0) {
            tr_warn("[SMeter] Mesh Size - %d\r\n", bbr_info.devices_in_network);
            mesh_size = bbr_info.devices_in_network;
        } else {
            tr_error("read_bbr_info - Failed :%d", ret);
            return;
        }

        if(mesh_size == 0)
        {
            tr_warn("[SMeter] No LN joined yet\r\n");
            continue;
        }

        len = sizeof(bbr_route_info_t) * mesh_size;
        bbr_route_info_t* table_p = (bbr_route_info_t*)malloc(len); 

        ret = ws_bbr_routing_table_get(mesh_iface_id, table_p, len);

        if(ret<0)
        {
            tr_warn("[SMeter] Routing Get Error\r\n");
        }
        else if(ret ==0 )
        {
            tr_warn("[SMeter] No LN joined yet\r\n");
            continue;
        }
        tr_warn("[SMeter] Table Size: %d\r\n", ret);

        bbr_route_info_t * cur = table_p;
        for(int i=0; i<ret; i++)
        {
            // printf("Node::%s || Parent::%s\r\n", cur->target, cur->parent);
            trace_route_info(cur);
            cur++;
        }
        tr_warn("[SMeter] Update value to pelion\r\n");
        _res_routing->set_value_raw((uint8_t*)table_p,ret*sizeof(bbr_route_info_t));
        tr_warn("[SMeter] Get Routing Table Done\r\n");
    }
}
#endif

/*--------------------------------------------------------------------------*/
#ifndef MBED_TEST_MODE
int main(void)
{
    int status;

    status = mbed_trace_init();
    if (status != 0) {
        printf("mbed_trace_init() failed with %d\n", status);
        return -1;
    }

    // Mount default kvstore
    tr_info("Pelion-Border-Router");
    status = kv_init_storage_config();
    if (status != MBED_SUCCESS) {
        printf("kv_init_storage_config() - failed, status %d\n", status);
        return -1;
    }
#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
    //mesh_system_init();
#endif

    nanostack_heap_region_add ();

    // Connect with NetworkInterface
    printf("Connect to network\n");
    network = NetworkInterface::get_default_instance();
    if (network == NULL) {
        printf("Failed to get default NetworkInterface\n");
        return -1;
    }
    status = network->connect();
    if (status != NSAPI_ERROR_OK) {
        printf("NetworkInterface failed to connect with %d\n", status);
        return -1;
    }
    status = network->get_ip_address(&sa);
    if (status!=0) {
        printf("get_ip_address failed with %d\n", status);
        return -2;
    }
    printf("Network initialized, connected with IP %s\n\n", sa.get_ip_address());

    // Run developer flow
    printf("Start developer flow\n");
    status = fcc_init();
    if (status != FCC_STATUS_SUCCESS) {
        printf("fcc_init() failed with %d\n", status);
        return -1;
    }

    // Inject hardcoded entropy for the device. Suitable only for demo devices.
    (void) fcc_entropy_set(MBED_CLOUD_DEV_ENTROPY, sizeof(MBED_CLOUD_DEV_ENTROPY));
    status = fcc_developer_flow();
    if (status != FCC_STATUS_SUCCESS && status != FCC_STATUS_KCM_FILE_EXIST_ERROR && status != FCC_STATUS_CA_ERROR) {
        printf("fcc_developer_flow() failed with %d\n", status);
        return -1;
    }

    printf("Create resources\n");
    M2MObjectList m2m_obj_list;

    // GET resource 3200/0/5501
    // PUT also allowed for resetting the resource
    m2m_get_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3200, 0, 5501, M2MResourceInstance::INTEGER, M2MBase::GET_PUT_ALLOWED);
    if (m2m_get_res->set_value(0) != true) {
        printf("m2m_get_res->set_value() failed\n");
        return -1;
    }
    if (m2m_get_res->set_value_updated_function(get_res_update) != true) {
        printf("m2m_get_res->set_value_updated_function() failed\n");
        return -1;
    }

    // PUT resource 3201/0/5853
    m2m_put_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3201, 0, 5853, M2MResourceInstance::STRING, M2MBase::GET_PUT_ALLOWED);
    if (m2m_put_res->set_value(0) != true) {
        printf("m2m_put_res->set_value() failed\n");
        return -1;
    }
    if (m2m_put_res->set_value_updated_function(put_res_update) != true) {
        printf("m2m_put_res->set_value_updated_function() failed\n");
        return -1;
    }

    // POST resource 3201/0/5850
    m2m_post_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3201, 0, 5850, M2MResourceInstance::STRING, M2MBase::POST_ALLOWED);
    if (m2m_post_res->set_execute_function(execute_post) != true) {
        printf("m2m_post_res->set_execute_function() failed\n");
        return -1;
    }

    // POST resource 5000/0/1 to trigger deregister.
    m2m_deregister_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 5000, 0, 1, M2MResourceInstance::INTEGER, M2MBase::POST_ALLOWED);

    // Use delayed response
    m2m_deregister_res->set_delayed_response(true);

    if (m2m_deregister_res->set_execute_function(deregister) != true) {
        printf("m2m_post_res->set_execute_function() failed\n");
        return -1;
    }

    // optional Device resource for running factory reset for the device. Path of this resource will be: 3/0/6.
    m2m_factory_reset_res = M2MInterfaceFactory::create_device()->create_resource(M2MDevice::FactoryReset);
    if (m2m_factory_reset_res) {
        m2m_factory_reset_res->set_execute_function(factory_reset);
    }

#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
    if (mesh_interface_up () < 0)
    {
        tr_err("[SMeter]Failed to Bring Mesh Interface UP\n");
        network->disconnect();
        return -1;
    }

    static SocketAddress meshsa;
    nsapi_error_t err;
    err = mesh_network->get_ip_address(&meshsa);
    static uint8_t IIDs[8];
    uint8_t* ip_t = (uint8_t*)meshsa.get_ip_bytes();

    tr_warn("[SMeter] IPV6: %s", trace_array(ip_t, 16));
    memcpy(IIDs,ip_t+8,8);

    // Create resource for IID . Path of this resource will be: 26241/0/9.
    _res_iid = M2MInterfaceFactory::create_resource(m2m_obj_list, 26241, 0, 9, M2MResourceInstance::OPAQUE, M2MBase::GET_POST_ALLOWED);
    _res_iid->set_value_raw(IIDs,8);

    // GET resource 26241/0/8
    // PUT also allowed for resetting the resource
    _res_routing = M2MInterfaceFactory::create_resource(m2m_obj_list, 26241, 0, 8, M2MResourceInstance::OPAQUE, M2MBase::GET_POST_ALLOWED);
    //_res_routing -> set_resource_read_callback(read_routing_table);
#endif

    printf("Register Pelion Device Management Client\n\n");

#ifdef MBED_CLOUD_CLIENT_SUPPORT_UPDATE
    cloud_client = new MbedCloudClient(client_registered, client_unregistered, client_error, NULL, update_progress);
#else
    cloud_client = new MbedCloudClient(client_registered, client_unregistered, client_error);
#endif // MBED_CLOUD_CLIENT_SUPPORT_UPDATE

    cloud_client->add_objects(m2m_obj_list);
    cloud_client->setup(network);

    t.start(callback(&queue, &EventQueue::dispatch_forever));
    queue.call_every(20000, value_increment);

    sync_semaphore.acquire();

#if defined(PLATFORM_WISUN_SMART_METER) && (PLATFORM_WISUN_SMART_METER == 1)
    _thread_routing_update.start(read_routing_table);
#else
    if (mesh_interface_up() < 0)
    {
        printf ("Failed to Bring Mesh Interface UP\n");
        deregister_client();
        network->disconnect();
        return -1;
    }
#endif
    // Flush the stdin buffer before reading from it
    flush_stdin_buffer();

    while(cloud_client_running) {
        int in_char = getchar();
        if (in_char == 'i') {
            print_client_ids(); // When 'i' is pressed, print endpoint info
            continue;
        } else if (in_char == 'r') {
            (void) fcc_storage_delete(); // When 'r' is pressed, erase storage and reboot the board.
            printf("Storage erased, rebooting the device.\n\n");
            ThisThread::sleep_for(1*1000);
            NVIC_SystemReset();
        } else if (in_char > 0 && in_char != 0x03) { // Ctrl+C is 0x03 in Mbed OS and Linux returns negative number
            value_increment(); // Simulate button press
            continue;
        }
        deregister_client();
        break;
    }
    return 0;
}
#endif /* MBED_TEST_MODE */
#endif /* PELION_BORDER_ROUTER_WITH_NM */
