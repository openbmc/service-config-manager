/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#include "srvcfg_manager.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/unordered_map.hpp>
#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_map>

std::unique_ptr<boost::asio::steady_timer> timer = nullptr;
std::unique_ptr<boost::asio::steady_timer> initTimer = nullptr;
std::map<std::string, std::shared_ptr<phosphor::service::ServiceConfig>>
    srvMgrObjects;
static bool unitQueryStarted = false;

static constexpr const char* srvCfgMgrFile = "/etc/srvcfg-mgr.json";
static constexpr const char* tmpFileBad = "/tmp/srvcfg-mgr.json.bad";

// Base service name list. All instance of these services and
// units(service/socket) will be managed by this daemon.
static std::unordered_map<std::string /* unitName */,
                          bool /* isSocketActivated */>
    managedServices = {{"phosphor-ipmi-net", false}, {"bmcweb", false},
                       {"phosphor-ipmi-kcs", false}, {"obmc-ikvm", false},
                       {"obmc-console", false},      {"dropbear", true},
                       {"obmc-console-ssh", true},   {"ssifbridge", false}};

enum class UnitType
{
    service,
    socket,
    target,
    device,
    invalid
};

using MonitorListMap =
    std::unordered_map<std::string, std::tuple<std::string, std::string,
                                               std::string, std::string>>;
MonitorListMap unitsToMonitor;

enum class monitorElement
{
    unitName,
    instanceName,
    serviceObjPath,
    socketObjPath
};

std::tuple<std::string, UnitType, std::string> getUnitNameTypeAndInstance(
    const std::string& fullUnitName)
{
    UnitType type = UnitType::invalid;
    std::string instanceName;
    std::string unitName;
    // get service type
    auto typePos = fullUnitName.rfind(".");
    if (typePos != std::string::npos)
    {
        const auto& typeStr = fullUnitName.substr(typePos + 1);
        // Ignore types other than service and socket
        if (typeStr == "service")
        {
            type = UnitType::service;
        }
        else if (typeStr == "socket")
        {
            type = UnitType::socket;
        }
        // get instance name if available
        auto instancePos = fullUnitName.rfind("@");
        if (instancePos != std::string::npos)
        {
            instanceName =
                fullUnitName.substr(instancePos + 1, typePos - instancePos - 1);
            unitName = fullUnitName.substr(0, instancePos);
        }
        else
        {
            unitName = fullUnitName.substr(0, typePos);
        }
    }
    return std::make_tuple(unitName, type, instanceName);
}

static inline void handleListUnitsResponse(
    sdbusplus::asio::object_server& server,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::system::error_code /*ec*/,
    const std::vector<ListUnitsType>& listUnits)
{
    // Loop through all units, and mark all units, which has to be
    // managed, irrespective of instance name.
    for (const auto& unit : listUnits)
    {
        // Ignore non-existent units
        if (std::get<static_cast<int>(ListUnitElements::loadState)>(unit) ==
            loadStateNotFound)
        {
            continue;
        }

        const auto& fullUnitName =
            std::get<static_cast<int>(ListUnitElements::name)>(unit);
        auto [unitName, type,
              instanceName] = getUnitNameTypeAndInstance(fullUnitName);
        if (managedServices.count(unitName))
        {
            // For socket-activated units, ignore all its instances
            if (managedServices.at(unitName) == true && !instanceName.empty())
            {
                continue;
            }

            std::string instantiatedUnitName =
                unitName + addInstanceName(instanceName, "@");
            const sdbusplus::message::object_path& objectPath =
                std::get<static_cast<int>(ListUnitElements::objectPath)>(unit);
            // Group the service & socket units together.. Same services
            // are managed together.
            auto it = unitsToMonitor.find(instantiatedUnitName);
            if (it != unitsToMonitor.end())
            {
                auto& value = it->second;
                if (type == UnitType::service)
                {
                    std::get<static_cast<int>(monitorElement::serviceObjPath)>(
                        value) = objectPath.str;
                }
                else if (type == UnitType::socket)
                {
                    std::get<static_cast<int>(monitorElement::socketObjPath)>(
                        value) = objectPath.str;
                }
                continue;
            }
            // If not grouped with any existing entry, create a new one
            if (type == UnitType::service)
            {
                unitsToMonitor.emplace(instantiatedUnitName,
                                       std::make_tuple(unitName, instanceName,
                                                       objectPath.str, ""));
            }
            else if (type == UnitType::socket)
            {
                unitsToMonitor.emplace(instantiatedUnitName,
                                       std::make_tuple(unitName, instanceName,
                                                       "", objectPath.str));
            }
        }
    }

    bool updateRequired = false;
    bool jsonExist = std::filesystem::exists(srvCfgMgrFile);
    if (jsonExist)
    {
        try
        {
            std::ifstream file(srvCfgMgrFile);
            cereal::JSONInputArchive archive(file);
            MonitorListMap savedMonitorList;
            archive(savedMonitorList);

            // compare the unit list read from systemd1 and the save list.
            MonitorListMap diffMap;
            std::set_difference(begin(unitsToMonitor), end(unitsToMonitor),
                                begin(savedMonitorList), end(savedMonitorList),
                                std::inserter(diffMap, begin(diffMap)));
            for (auto& unitIt : diffMap)
            {
                auto it = savedMonitorList.find(unitIt.first);
                if (it == savedMonitorList.end())
                {
                    savedMonitorList.insert(unitIt);
                    updateRequired = true;
                }
            }
            unitsToMonitor = savedMonitorList;
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to load {FILEPATH} file, need to rewrite: {ERROR}.",
                "FILEPATH", srvCfgMgrFile, "ERROR", e);

            // The "bad" files need to be moved to /tmp/ so that we can try to
            // find out the cause of the file corruption. If we encounter this
            // failure multiple times, we will only overwrite it to ensure that
            // we don't accidentally fill up /tmp/.
            std::error_code ec;
            std::filesystem::copy_file(
                srvCfgMgrFile, tmpFileBad,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                lg2::error("Failed to copy {SRCFILE} file to {DSTFILE}.",
                           "SRCFILE", srvCfgMgrFile, "DSTFILE", tmpFileBad);
            }

            updateRequired = true;
        }
    }
    if (!jsonExist || updateRequired)
    {
        std::ofstream file(srvCfgMgrFile);
        cereal::JSONOutputArchive archive(file);
        archive(CEREAL_NVP(unitsToMonitor));
    }

#ifdef USB_CODE_UPDATE
    unitsToMonitor.emplace(
        "phosphor-usb-code-update",
        std::make_tuple(
            phosphor::service::usbCodeUpdateUnitName, "",
            "/org/freedesktop/systemd1/unit/usb_2dcode_2dupdate_2eservice",
            ""));
#endif

    // create objects for needed services
    for (auto& it : unitsToMonitor)
    {
        sdbusplus::message::object_path basePath(
            phosphor::service::srcCfgMgrBasePath);
        std::string objPath(basePath / it.first);
        auto srvCfgObj = std::make_unique<phosphor::service::ServiceConfig>(
            server, conn, objPath,
            std::get<static_cast<int>(monitorElement::unitName)>(it.second),
            std::get<static_cast<int>(monitorElement::instanceName)>(it.second),
            std::get<static_cast<int>(monitorElement::serviceObjPath)>(
                it.second),
            std::get<static_cast<int>(monitorElement::socketObjPath)>(
                it.second));
        srvMgrObjects.emplace(
            std::make_pair(std::move(objPath), std::move(srvCfgObj)));
    }
}

void init(sdbusplus::asio::object_server& server,
          std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    // Go through all systemd units, and dynamically detect and manage
    // the service daemons
    conn->async_method_call(
        [&server, &conn](boost::system::error_code ec,
                         const std::vector<ListUnitsType>& listUnits) {
            if (ec)
            {
                lg2::error("async_method_call error: ListUnits failed: {EC}",
                           "EC", ec.value());
                return;
            }
            handleListUnitsResponse(server, conn, ec, listUnits);
        },
        sysdService, sysdObjPath, sysdMgrIntf, "ListUnits");
}

void checkAndInit(sdbusplus::asio::object_server& server,
                  std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    // Check whether systemd completed all the loading before initializing
    conn->async_method_call(
        [&server, &conn](boost::system::error_code ec,
                         const std::variant<uint64_t>& value) {
            if (ec)
            {
                lg2::error("async_method_call error: ListUnits failed: {EC}",
                           "EC", ec.value());
                return;
            }
            if (std::get<uint64_t>(value))
            {
                if (!unitQueryStarted)
                {
                    unitQueryStarted = true;
                    init(server, conn);
                }
            }
            else
            {
                // FIX-ME: Latest up-stream sync caused issue in receiving
                // StartupFinished signal. Unable to get StartupFinished signal
                // from systemd1 hence using poll method too, to trigger it
                // properly.
                constexpr size_t pollTimeout = 10; // seconds
                initTimer->expires_after(std::chrono::seconds(pollTimeout));
                initTimer->async_wait([&server, &conn](
                                          const boost::system::error_code& ec) {
                    if (ec == boost::asio::error::operation_aborted)
                    {
                        // Timer reset.
                        return;
                    }
                    if (ec)
                    {
                        lg2::error(
                            "service config mgr - init - async wait error: {EC}",
                            "EC", ec.value());
                        return;
                    }
                    checkAndInit(server, conn);
                });
            }
        },
        sysdService, sysdObjPath, dBusPropIntf, dBusGetMethod, sysdMgrIntf,
        "FinishTimestamp");
}

int main()
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    timer = std::make_unique<boost::asio::steady_timer>(io);
    initTimer = std::make_unique<boost::asio::steady_timer>(io);
    conn->request_name(phosphor::service::serviceConfigSrvName);
    auto server = sdbusplus::asio::object_server(conn, true);
    server.add_manager(phosphor::service::srcCfgMgrBasePath);
    // Initialize the objects after systemd indicated startup finished.
    auto userUpdatedSignal = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',"
        "member='StartupFinished',path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.systemd1.Manager'",
        [&server, &conn](sdbusplus::message_t& /*msg*/) {
            if (!unitQueryStarted)
            {
                unitQueryStarted = true;
                init(server, conn);
            }
        });
    // this will make sure to initialize the objects, when daemon is
    // restarted.
    checkAndInit(server, conn);

    io.run();

    return 0;
}
