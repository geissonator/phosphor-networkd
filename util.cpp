#include "util.hpp"

#include "config_parser.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <experimental/filesystem>
#include <iostream>
#include <list>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <xyz/openbmc_project/Common/error.hpp>

namespace phosphor
{
namespace network
{

namespace
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;
namespace fs = std::experimental::filesystem;

uint8_t toV6Cidr(const std::string& subnetMask)
{
    uint8_t pos = 0;
    uint8_t prevPos = 0;
    uint8_t cidr = 0;
    uint16_t buff{};
    do
    {
        // subnet mask look like ffff:ffff::
        // or ffff:c000::
        pos = subnetMask.find(":", prevPos);
        if (pos == std::string::npos)
        {
            break;
        }

        auto str = subnetMask.substr(prevPos, (pos - prevPos));
        prevPos = pos + 1;

        // String length is 0
        if (!str.length())
        {
            return cidr;
        }
        // converts it into number.
        if (sscanf(str.c_str(), "%hx", &buff) <= 0)
        {
            log<level::ERR>("Invalid Mask",
                            entry("SUBNETMASK=%s", subnetMask.c_str()));

            return 0;
        }

        // convert the number into bitset
        // and check for how many ones are there.
        // if we don't have all the ones then make
        // sure that all the ones should be left justify.

        if (__builtin_popcount(buff) != 16)
        {
            if (((sizeof(buff) * 8) - (__builtin_ctz(buff))) !=
                __builtin_popcount(buff))
            {
                log<level::ERR>("Invalid Mask",
                                entry("SUBNETMASK=%s", subnetMask.c_str()));

                return 0;
            }
            cidr += __builtin_popcount(buff);
            return cidr;
        }

        cidr += 16;
    } while (1);

    return cidr;
}
} // anonymous namespace

uint8_t toCidr(int addressFamily, const std::string& subnetMask)
{
    if (addressFamily == AF_INET6)
    {
        return toV6Cidr(subnetMask);
    }

    uint32_t buff;

    auto rc = inet_pton(addressFamily, subnetMask.c_str(), &buff);
    if (rc <= 0)
    {
        log<level::ERR>("inet_pton failed:",
                        entry("SUBNETMASK=%s", subnetMask.c_str()));
        return 0;
    }

    buff = be32toh(buff);
    // total no of bits - total no of leading zero == total no of ones
    if (((sizeof(buff) * 8) - (__builtin_ctz(buff))) ==
        __builtin_popcount(buff))
    {
        return __builtin_popcount(buff);
    }
    else
    {
        log<level::ERR>("Invalid Mask",
                        entry("SUBNETMASK=%s", subnetMask.c_str()));
        return 0;
    }
}

std::string toMask(int addressFamily, uint8_t prefix)
{
    if (addressFamily == AF_INET6)
    {
        // TODO:- conversion for v6
        return "";
    }

    if (prefix < 1 || prefix > 30)
    {
        log<level::ERR>("Invalid Prefix", entry("PREFIX=%d", prefix));
        return "";
    }
    /* Create the netmask from the number of bits */
    unsigned long mask = 0;
    for (auto i = 0; i < prefix; i++)
    {
        mask |= 1 << (31 - i);
    }
    struct in_addr netmask;
    netmask.s_addr = htonl(mask);
    return inet_ntoa(netmask);
}

InAddrAny addrFromBuf(int addressFamily, std::string_view buf)
{
    if (addressFamily == AF_INET)
    {
        struct in_addr ret;
        if (buf.size() != sizeof(ret))
        {
            throw std::runtime_error("Buf not in_addr sized");
        }
        memcpy(&ret, buf.data(), sizeof(ret));
        return ret;
    }
    else if (addressFamily == AF_INET6)
    {
        struct in6_addr ret;
        if (buf.size() != sizeof(ret))
        {
            throw std::runtime_error("Buf not in6_addr sized");
        }
        memcpy(&ret, buf.data(), sizeof(ret));
        return ret;
    }

    throw std::runtime_error("Unsupported address family");
}

std::string toString(const InAddrAny& addr)
{
    std::string ip;
    if (std::holds_alternative<struct in_addr>(addr))
    {
        const auto& v = std::get<struct in_addr>(addr);
        ip.resize(INET_ADDRSTRLEN);
        if (inet_ntop(AF_INET, &v, ip.data(), ip.size()) == NULL)
        {
            throw std::runtime_error("Failed to convert IP4 to string");
        }
    }
    else if (std::holds_alternative<struct in6_addr>(addr))
    {
        const auto& v = std::get<struct in6_addr>(addr);
        ip.resize(INET6_ADDRSTRLEN);
        if (inet_ntop(AF_INET6, &v, ip.data(), ip.size()) == NULL)
        {
            throw std::runtime_error("Failed to convert IP6 to string");
        }
    }
    else
    {
        throw std::runtime_error("Invalid addr type");
    }
    ip.resize(strlen(ip.c_str()));
    return ip;
}

bool isLinkLocalIP(const std::string& address)
{
    return address.find(IPV4_PREFIX) == 0 || address.find(IPV6_PREFIX) == 0;
}

bool isValidIP(int addressFamily, const std::string& address)
{
    unsigned char buf[sizeof(struct in6_addr)];

    return inet_pton(addressFamily, address.c_str(), buf) > 0;
}

bool isValidPrefix(int addressFamily, uint8_t prefixLength)
{
    if (addressFamily == AF_INET)
    {
        if (prefixLength < IPV4_MIN_PREFIX_LENGTH ||
            prefixLength > IPV4_MAX_PREFIX_LENGTH)
        {
            return false;
        }
    }

    if (addressFamily == AF_INET6)
    {
        if (prefixLength < IPV4_MIN_PREFIX_LENGTH ||
            prefixLength > IPV6_MAX_PREFIX_LENGTH)
        {
            return false;
        }
    }

    return true;
}

IntfAddrMap getInterfaceAddrs()
{
    IntfAddrMap intfMap{};
    struct ifaddrs* ifaddr = nullptr;

    // attempt to fill struct with ifaddrs
    if (getifaddrs(&ifaddr) == -1)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during the getifaddrs call",
                        entry("ERRNO=%s", strerror(error)));
        elog<InternalFailure>();
    }

    AddrPtr ifaddrPtr(ifaddr);
    ifaddr = nullptr;

    std::string intfName{};

    for (ifaddrs* ifa = ifaddrPtr.get(); ifa != nullptr; ifa = ifa->ifa_next)
    {
        // walk interfaces
        if (ifa->ifa_addr == nullptr)
        {
            continue;
        }

        // get only INET interfaces not ipv6
        if (ifa->ifa_addr->sa_family == AF_INET ||
            ifa->ifa_addr->sa_family == AF_INET6)
        {
            // if loopback, or not running ignore
            if ((ifa->ifa_flags & IFF_LOOPBACK) ||
                !(ifa->ifa_flags & IFF_RUNNING))
            {
                continue;
            }
            intfName = ifa->ifa_name;
            AddrInfo info{};
            char ip[INET6_ADDRSTRLEN] = {0};
            char subnetMask[INET6_ADDRSTRLEN] = {0};

            if (ifa->ifa_addr->sa_family == AF_INET)
            {

                inet_ntop(ifa->ifa_addr->sa_family,
                          &(((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr),
                          ip, sizeof(ip));

                inet_ntop(
                    ifa->ifa_addr->sa_family,
                    &(((struct sockaddr_in*)(ifa->ifa_netmask))->sin_addr),
                    subnetMask, sizeof(subnetMask));
            }
            else
            {
                inet_ntop(ifa->ifa_addr->sa_family,
                          &(((struct sockaddr_in6*)(ifa->ifa_addr))->sin6_addr),
                          ip, sizeof(ip));

                inet_ntop(
                    ifa->ifa_addr->sa_family,
                    &(((struct sockaddr_in6*)(ifa->ifa_netmask))->sin6_addr),
                    subnetMask, sizeof(subnetMask));
            }

            info.addrType = ifa->ifa_addr->sa_family;
            info.ipaddress = ip;
            info.prefix = toCidr(info.addrType, std::string(subnetMask));
            intfMap[intfName].push_back(info);
        }
    }
    return intfMap;
}

InterfaceList getInterfaces()
{
    InterfaceList interfaces{};
    struct ifaddrs* ifaddr = nullptr;

    // attempt to fill struct with ifaddrs
    if (getifaddrs(&ifaddr) == -1)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during the getifaddrs call",
                        entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }

    AddrPtr ifaddrPtr(ifaddr);
    ifaddr = nullptr;

    for (ifaddrs* ifa = ifaddrPtr.get(); ifa != nullptr; ifa = ifa->ifa_next)
    {
        // walk interfaces
        // if loopback ignore
        if (ifa->ifa_flags & IFF_LOOPBACK)
        {
            continue;
        }
        interfaces.emplace(ifa->ifa_name);
    }
    return interfaces;
}

void deleteInterface(const std::string& intf)
{
    pid_t pid = fork();
    int status{};

    if (pid == 0)
    {

        execl("/sbin/ip", "ip", "link", "delete", "dev", intf.c_str(), nullptr);
        auto error = errno;
        log<level::ERR>("Couldn't delete the device", entry("ERRNO=%d", error),
                        entry("INTF=%s", intf.c_str()));
        elog<InternalFailure>();
    }
    else if (pid < 0)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during fork", entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }
    else if (pid > 0)
    {
        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno != EINTR)
            { /* Error other than EINTR */
                status = -1;
                break;
            }
        }

        if (status < 0)
        {
            log<level::ERR>("Unable to delete the interface",
                            entry("INTF=%s", intf.c_str()),
                            entry("STATUS=%d", status));
            elog<InternalFailure>();
        }
    }
}

std::optional<std::string> interfaceToUbootEthAddr(const char* intf)
{
    constexpr char ethPrefix[] = "eth";
    constexpr size_t ethPrefixLen = sizeof(ethPrefix) - 1;
    if (strncmp(ethPrefix, intf, ethPrefixLen) != 0)
    {
        return std::nullopt;
    }
    const auto intfSuffix = intf + ethPrefixLen;
    if (intfSuffix[0] == '\0')
    {
        return std::nullopt;
    }
    char* end;
    unsigned long idx = strtoul(intfSuffix, &end, 10);
    if (end[0] != '\0')
    {
        return std::nullopt;
    }
    if (idx == 0)
    {
        return "ethaddr";
    }
    return "eth" + std::to_string(idx) + "addr";
}

bool getDHCPValue(const std::string& confDir, const std::string& intf)
{
    bool dhcp = false;
    // Get the interface mode value from systemd conf
    // using namespace std::string_literals;
    fs::path confPath = confDir;
    std::string fileName = systemd::config::networkFilePrefix + intf +
                           systemd::config::networkFileSuffix;
    confPath /= fileName;

    auto rc = config::ReturnCode::SUCCESS;
    config::ValueList values;
    config::Parser parser(confPath.string());

    std::tie(rc, values) = parser.getValues("Network", "DHCP");
    if (rc != config::ReturnCode::SUCCESS)
    {
        log<level::DEBUG>("Unable to get the value for Network[DHCP]",
                          entry("RC=%d", rc));
        return dhcp;
    }
    // There will be only single value for DHCP key.
    if (values[0] == "true")
    {
        dhcp = true;
    }
    return dhcp;
}

namespace internal
{

void executeCommandinChildProcess(const char* path, char** args)
{
    using namespace std::string_literals;
    pid_t pid = fork();
    int status{};

    if (pid == 0)
    {
        execv(path, args);
        auto error = errno;
        // create the command from var args.
        std::string command = path + " "s;

        for (int i = 0; args[i]; i++)
        {
            command += args[i] + " "s;
        }

        log<level::ERR>("Couldn't exceute the command",
                        entry("ERRNO=%d", error),
                        entry("CMD=%s", command.c_str()));
        elog<InternalFailure>();
    }
    else if (pid < 0)
    {
        auto error = errno;
        log<level::ERR>("Error occurred during fork", entry("ERRNO=%d", error));
        elog<InternalFailure>();
    }
    else if (pid > 0)
    {
        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno != EINTR)
            { // Error other than EINTR
                status = -1;
                break;
            }
        }

        if (status < 0)
        {
            std::string command = path + " "s;
            for (int i = 0; args[i]; i++)
            {
                command += args[i] + " "s;
            }

            log<level::ERR>("Unable to execute the command",
                            entry("CMD=%s", command.c_str()),
                            entry("STATUS=%d", status));
            elog<InternalFailure>();
        }
    }
}
} // namespace internal

namespace mac_address
{

constexpr auto mapperBus = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperObj = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperIntf = "xyz.openbmc_project.ObjectMapper";
constexpr auto propIntf = "org.freedesktop.DBus.Properties";
constexpr auto methodGet = "Get";

using DbusObjectPath = std::string;
using DbusService = std::string;
using DbusInterface = std::string;
using ObjectTree =
    std::map<DbusObjectPath, std::map<DbusService, std::vector<DbusInterface>>>;

constexpr auto invBus = "xyz.openbmc_project.Inventory.Manager";
constexpr auto invNetworkIntf =
    "xyz.openbmc_project.Inventory.Item.NetworkInterface";
constexpr auto invRoot = "/xyz/openbmc_project/inventory";

ether_addr getfromInventory(sdbusplus::bus::bus& bus)
{
    std::vector<DbusInterface> interfaces;
    interfaces.emplace_back(invNetworkIntf);

    auto depth = 0;

    auto mapperCall =
        bus.new_method_call(mapperBus, mapperObj, mapperIntf, "GetSubTree");

    mapperCall.append(invRoot, depth, interfaces);

    auto mapperReply = bus.call(mapperCall);
    if (mapperReply.is_method_error())
    {
        log<level::ERR>("Error in mapper call");
        elog<InternalFailure>();
    }

    ObjectTree objectTree;
    mapperReply.read(objectTree);

    if (objectTree.empty())
    {
        log<level::ERR>("No Object has implemented the interface",
                        entry("INTERFACE=%s", invNetworkIntf));
        elog<InternalFailure>();
    }

    // It is expected that only one object has implemented this interface.

    auto objPath = objectTree.begin()->first;
    auto service = objectTree.begin()->second.begin()->first;

    auto method = bus.new_method_call(service.c_str(), objPath.c_str(),
                                      propIntf, methodGet);

    method.append(invNetworkIntf, "MACAddress");

    auto reply = bus.call(method);
    if (reply.is_method_error())
    {
        log<level::ERR>("Failed to get MACAddress",
                        entry("PATH=%s", objPath.c_str()),
                        entry("INTERFACE=%s", invNetworkIntf));
        elog<InternalFailure>();
    }

    std::variant<std::string> value;
    reply.read(value);
    return fromString(std::get<std::string>(value));
}

ether_addr fromString(const char* str)
{
    struct ether_addr* mac = ether_aton(str);
    if (mac == nullptr)
    {
        throw std::runtime_error("Invalid mac address string");
    }
    return *mac;
}

std::string toString(const ether_addr& mac)
{
    return ether_ntoa(&mac);
}

bool isEmpty(const ether_addr& mac)
{
    return equal(mac, ether_addr{});
}

bool isMulticast(const ether_addr& mac)
{
    return mac.ether_addr_octet[0] & 0b1;
}

bool isUnicast(const ether_addr& mac)
{
    return !isEmpty(mac) && !isMulticast(mac);
}

bool isLocalAdmin(const ether_addr& mac)
{
    return mac.ether_addr_octet[0] & 0b10;
}

} // namespace mac_address
} // namespace network
} // namespace phosphor
