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

#include <boost/asio/spawn.hpp>

#include <fstream>
#include <regex>

extern std::unique_ptr<boost::asio::steady_timer> timer;
extern std::map<std::string, std::shared_ptr<phosphor::service::ServiceConfig>>
    srvMgrObjects;
static bool updateInProgress = false;

namespace phosphor
{
namespace service
{

static constexpr const char* overrideConfFileName = "override.conf";
static constexpr const size_t restartTimeout = 15; // seconds

static constexpr const char* systemd1UnitBasePath =
    "/org/freedesktop/systemd1/unit/";
static constexpr const char* systemdOverrideUnitBasePath =
    "/etc/systemd/system/";

void ServiceConfig::updateSocketProperties(
    const boost::container::flat_map<std::string, VariantType>& propertyMap)
{
    auto listenIt = propertyMap.find("Listen");
    if (listenIt != propertyMap.end())
    {
        auto listenVal =
            std::get<std::vector<std::tuple<std::string, std::string>>>(
                listenIt->second);
        if (listenVal.size())
        {
            protocol = std::get<0>(listenVal[0]);
            std::string port = std::get<1>(listenVal[0]);
            auto tmp = std::stoul(port.substr(port.find_last_of(":") + 1),
                                  nullptr, 10);
            if (tmp > std::numeric_limits<uint16_t>::max())
            {
                throw std::out_of_range("Out of range");
            }
            portNum = tmp;
            if (sockAttrIface && sockAttrIface->is_initialized())
            {
                internalSet = true;
                sockAttrIface->set_property(sockAttrPropPort, portNum);
                internalSet = false;
            }
        }
    }
}

void ServiceConfig::updateServiceProperties(
    const boost::container::flat_map<std::string, VariantType>& propertyMap)
{
    auto stateIt = propertyMap.find("UnitFileState");
    if (stateIt != propertyMap.end())
    {
        stateValue = std::get<std::string>(stateIt->second);
        unitEnabledState = unitMaskedState = false;
        if (stateValue == stateMasked)
        {
            unitMaskedState = true;
        }
        else if (stateValue == stateEnabled)
        {
            unitEnabledState = true;
        }
        if (srvCfgIface && srvCfgIface->is_initialized())
        {
            internalSet = true;
            srvCfgIface->set_property(srvCfgPropMasked, unitMaskedState);
            srvCfgIface->set_property(srvCfgPropEnabled, unitEnabledState);
            internalSet = false;
        }
    }
    auto subStateIt = propertyMap.find("SubState");
    if (subStateIt != propertyMap.end())
    {
        subStateValue = std::get<std::string>(subStateIt->second);
        if (subStateValue == subStateRunning ||
            subStateValue == subStateListening)
        {
            unitRunningState = true;
        }
        if (srvCfgIface && srvCfgIface->is_initialized())
        {
            internalSet = true;
            srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
            internalSet = false;
        }
    }
}

void ServiceConfig::queryAndUpdateProperties()
{
    std::string objectPath =
        isDropBearService ? socketObjectPath : serviceObjectPath;
    if (objectPath.empty())
    {
        return;
    }

    conn->async_method_call(
        [this](boost::system::error_code ec,
               const boost::container::flat_map<std::string, VariantType>&
                   propertyMap) {
            if (ec)
            {
                lg2::error(
                    "async_method_call error: Failed to service unit properties, ec = {EC}",
                    "EC", ec.value());
                return;
            }
            try
            {
                updateServiceProperties(propertyMap);
                if (!socketObjectPath.empty())
                {
                    conn->async_method_call(
                        [this](boost::system::error_code ec,
                               const boost::container::flat_map<
                                   std::string, VariantType>& propertyMap) {
                            if (ec)
                            {
                                lg2::error(
                                    "async_method_call error: Failed to get all property, ec = {EC}",
                                    "EC", ec.value());
                                return;
                            }
                            try
                            {
                                updateSocketProperties(propertyMap);
                                if (!srvCfgIface)
                                {
                                    registerProperties();
                                }
                            }
                            catch (const std::exception& e)
                            {
                                lg2::error(
                                    "Exception in getting socket properties, ERROR = {ERROR}",
                                    "ERROR", e);
                                return;
                            }
                        },
                        sysdService, socketObjectPath, dBusPropIntf,
                        dBusGetAllMethod, sysdSocketIntf);
                }
                else if (!srvCfgIface)
                {
                    registerProperties();
                }
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Exception in getting socket properties, ERROR = {ERROR}",
                    "ERROR", e);
                return;
            }
        },
        sysdService, objectPath, dBusPropIntf, dBusGetAllMethod, sysdUnitIntf);
    return;
}

void ServiceConfig::createSocketOverrideConf()
{
    if (!socketObjectPath.empty())
    {
        namespace fs = std::filesystem;
        std::string socketUnitName(instantiatedUnitName + ".socket");
        /// Check override socket directory exist, if not create it.
        fs::path ovrUnitFileDir(systemdOverrideUnitBasePath);
        ovrUnitFileDir += socketUnitName;
        ovrUnitFileDir += ".d";
        if (!fs::exists(ovrUnitFileDir))
        {
            if (!fs::create_directories(ovrUnitFileDir))
            {
                lg2::error("Unable to create the directory. DIR = {DIR}", "DIR",
                           ovrUnitFileDir);
                phosphor::logging::elog<sdbusplus::xyz::openbmc_project::
                                            Common::Error::InternalFailure>();
            }
        }
        overrideConfDir = std::string(ovrUnitFileDir);
    }
}

ServiceConfig::ServiceConfig(
    sdbusplus::asio::object_server& srv_,
    std::shared_ptr<sdbusplus::asio::connection>& conn_,
    const std::string& objPath_, const std::string& baseUnitName_,
    const std::string& instanceName_, const std::string& serviceObjPath_,
    const std::string& socketObjPath_) :
    conn(conn_),
    server(srv_), objPath(objPath_), baseUnitName(baseUnitName_),
    instanceName(instanceName_), serviceObjectPath(serviceObjPath_),
    socketObjectPath(socketObjPath_)
{
    if (baseUnitName == "dropbear")
    {
        isDropBearService = true;
    }
    instantiatedUnitName = baseUnitName + addInstanceName(instanceName, "@");
    updatedFlag = 0;
    queryAndUpdateProperties();
    return;
}

std::string ServiceConfig::getSocketUnitName()
{
    return instantiatedUnitName + ".socket";
}

std::string ServiceConfig::getServiceUnitName()
{
    return instantiatedUnitName + ".service";
}

bool ServiceConfig::isMaskedOut()
{
    // return true  if state is masked & no request to update the maskedState
    return (
        stateValue == "masked" &&
        !(updatedFlag & (1 << static_cast<uint8_t>(UpdatedProp::maskedState))));
}

void ServiceConfig::stopAndApplyUnitConfig(boost::asio::yield_context yield)
{
    if (!updatedFlag || isMaskedOut())
    {
        // No updates / masked - Just return.
        return;
    }
    lg2::info("Applying new settings. OBJPATH = {OBJPATH}", "OBJPATH", objPath);
    if (subStateValue == subStateRunning || subStateValue == subStateListening)
    {
        if (!socketObjectPath.empty())
        {
            systemdUnitAction(conn, yield, getSocketUnitName(), sysdStopUnit);
        }
        if (!isDropBearService)
        {
            systemdUnitAction(conn, yield, getServiceUnitName(), sysdStopUnit);
        }
        else
        {
            // Get the ListUnits property, find all the services of
            // `dropbear@<ip><port>.service` and stop the service through
            // the systemdUnitAction method
            boost::system::error_code ec;
            auto listUnits =
                conn->yield_method_call<std::vector<ListUnitsType>>(
                    yield, ec, sysdService, sysdObjPath, sysdMgrIntf,
                    "ListUnits");

            checkAndThrowInternalFailure(
                ec, "yield_method_call error: ListUnits failed");

            for (const auto& unit : listUnits)
            {
                const auto& service =
                    std::get<static_cast<int>(ListUnitElements::name)>(unit);
                const auto& status =
                    std::get<static_cast<int>(ListUnitElements::subState)>(
                        unit);
                if (service.find("dropbear@") != std::string::npos &&
                    service.find(".service") != std::string::npos &&
                    status == subStateRunning)
                {
                    systemdUnitAction(conn, yield, service, sysdStopUnit);
                }
            }
        }
    }

    if (updatedFlag & (1 << static_cast<uint8_t>(UpdatedProp::port)))
    {
        createSocketOverrideConf();
        // Create override config file and write data.
        std::string ovrCfgFile{overrideConfDir + "/" + overrideConfFileName};
        std::string tmpFile{ovrCfgFile + "_tmp"};
        std::ofstream cfgFile(tmpFile, std::ios::out);
        if (!cfgFile.good())
        {
            lg2::error(
                "Failed to open override.conf_tmp file, tmpFile = {TMPFILE}",
                "TMPFILE", tmpFile);
            phosphor::logging::elog<sdbusplus::xyz::openbmc_project::Common::
                                        Error::InternalFailure>();
        }

        // Write the socket header
        cfgFile << "[Socket]\n";
        // Listen
        cfgFile << "Listen" << protocol << "="
                << "\n";
        cfgFile << "Listen" << protocol << "=" << portNum << "\n";
        cfgFile.close();

        if (std::rename(tmpFile.c_str(), ovrCfgFile.c_str()) != 0)
        {
            lg2::error(
                "Failed to rename tmp file as override.conf, tmpFile = {TMPFILE}, ovrCfgFile = {OVERCFGFILE}",
                "TMPFILE", tmpFile, "OVERCFGFILE", ovrCfgFile);
            std::remove(tmpFile.c_str());
            phosphor::logging::elog<sdbusplus::xyz::openbmc_project::Common::
                                        Error::InternalFailure>();
        }
    }

    if (updatedFlag & ((1 << static_cast<uint8_t>(UpdatedProp::maskedState)) |
                       (1 << static_cast<uint8_t>(UpdatedProp::enabledState))))
    {
        std::vector<std::string> unitFiles;
        if (socketObjectPath.empty())
        {
            unitFiles = {getServiceUnitName()};
        }
        else if (!socketObjectPath.empty() && isDropBearService)
        {
            unitFiles = {getSocketUnitName()};
        }
        else
        {
            unitFiles = {getSocketUnitName(), getServiceUnitName()};
        }
        systemdUnitFilesStateChange(conn, yield, unitFiles, stateValue,
                                    unitMaskedState, unitEnabledState);
    }
    return;
}
void ServiceConfig::restartUnitConfig(boost::asio::yield_context yield)
{
    if (!updatedFlag || isMaskedOut())
    {
        // No updates. Just return.
        return;
    }

    if (unitRunningState)
    {
        if (!socketObjectPath.empty())
        {
            systemdUnitAction(conn, yield, getSocketUnitName(),
                              sysdRestartUnit);
        }
        if (!isDropBearService)
        {
            systemdUnitAction(conn, yield, getServiceUnitName(),
                              sysdRestartUnit);
        }
    }

    // Reset the flag
    updatedFlag = 0;

    lg2::info("Applied new settings, OBJPATH = {OBJPATH}", "OBJPATH", objPath);

    queryAndUpdateProperties();
    return;
}

void ServiceConfig::startServiceRestartTimer()
{
    timer->expires_after(std::chrono::seconds(restartTimeout));
    timer->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // Timer reset.
            return;
        }
        else if (ec)
        {
            lg2::error("async wait error. ec = {EC}", "EC", ec.value());
            return;
        }
        updateInProgress = true;
        boost::asio::spawn(conn->get_io_context(),
                           [this](boost::asio::yield_context yield) {
                               // Stop and apply configuration for all objects
                               for (auto& srvMgrObj : srvMgrObjects)
                               {
                                   auto& srvObj = srvMgrObj.second;
                                   if (srvObj->updatedFlag)
                                   {
                                       srvObj->stopAndApplyUnitConfig(yield);
                                   }
                               }
                               // Do system reload
                               systemdDaemonReload(conn, yield);
                               // restart unit config.
                               for (auto& srvMgrObj : srvMgrObjects)
                               {
                                   auto& srvObj = srvMgrObj.second;
                                   if (srvObj->updatedFlag)
                                   {
                                       srvObj->restartUnitConfig(yield);
                                   }
                               }
                               updateInProgress = false;
                           });
    });
}

void ServiceConfig::registerProperties()
{
    srvCfgIface = server.add_interface(objPath, serviceConfigIntfName);

    if (!socketObjectPath.empty())
    {
        sockAttrIface = server.add_interface(objPath, sockAttrIntfName);
        sockAttrIface->register_property(
            sockAttrPropPort, portNum,
            [this](const uint16_t& req, uint16_t& res) {
                if (!internalSet)
                {
                    if (req == res)
                    {
                        return 1;
                    }
                    if (updateInProgress)
                    {
                        return 0;
                    }
                    portNum = req;
                    updatedFlag |=
                        (1 << static_cast<uint8_t>(UpdatedProp::port));
                    startServiceRestartTimer();
                }
                res = req;
                return 1;
            });
    }

    srvCfgIface->register_property(
        srvCfgPropMasked, unitMaskedState, [this](const bool& req, bool& res) {
            if (!internalSet)
            {
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                unitMaskedState = req;
                unitEnabledState = !unitMaskedState;
                unitRunningState = !unitMaskedState;
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::maskedState)) |
                    (1 << static_cast<uint8_t>(UpdatedProp::enabledState)) |
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                internalSet = true;
                srvCfgIface->set_property(srvCfgPropEnabled, unitEnabledState);
                srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
                internalSet = false;
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->register_property(
        srvCfgPropEnabled, unitEnabledState,
        [this](const bool& req, bool& res) {
            if (!internalSet)
            {
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                if (unitMaskedState)
                { // block updating if masked
                    lg2::error("Invalid value specified");
                    return -EINVAL;
                }
                unitEnabledState = req;
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::enabledState));
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->register_property(
        srvCfgPropRunning, unitRunningState,
        [this](const bool& req, bool& res) {
            if (!internalSet)
            {
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                if (unitMaskedState)
                { // block updating if masked
                    lg2::error("Invalid value specified");
                    return -EINVAL;
                }
                unitRunningState = req;
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->initialize();
    if (!socketObjectPath.empty())
    {
        sockAttrIface->initialize();
    }
    return;
}

} // namespace service
} // namespace phosphor
