#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>

/// Which machine this is, as opposed to what it can do.
///
/// **Every field here is personal data**, which is why none of it lives in cc::system_info.
/// A hostname is frequently a person's name, and a machine id is a stable identifier for one individual's computer.
/// Keeping the two types apart makes the safe default structural rather than a matter of discipline: a caller holding a
/// cc::system_info cannot put a hostname into a recording, because the field is not reachable from what it is holding.
///
/// The flags argument is required and has no default, so a call site says what it collected and a reader can see it
/// without opening this header.

/// One identifying fact about the machine, requested by name.
enum class cc::identity_field
{
    /// The machine's network name, which is very often a person's name.
    hostname,

    /// The account this process runs as.
    username,

    /// A stable id for this installation — Windows MachineGuid, /etc/machine-id, Darwin's IOPlatformUUID.
    /// Survives reboots and does not change when the network does, which is exactly what makes it identifying.
    machine_id,

    /// The hardware addresses of this machine's network interfaces.
    /// Globally unique per device, and unaffected by reinstalling the OS.
    mac_addresses,

    /// The serial numbers of the attached storage devices.
    /// Unavailable on macOS, where reading one needs IOKit, and empty rather than absent where a device refuses.
    disk_serials,
};

CC_FLAG_ENUM_INDEXED(cc, identity_field, u32);

/// The identifying facts that were asked for, and only those.
///
/// A field that was not requested is absent, and so is one the platform could not answer — the two are deliberately not
/// distinguished, because a caller that did not ask has no use for the difference.
struct cc::system_identifier
{
    cc::optional<cc::string> hostname;
    cc::optional<cc::string> username;
    cc::optional<cc::string> machine_id;

    cc::vector<cc::string> mac_addresses; ///< "00:1a:2b:3c:4d:5e", loopback and virtual adapters excluded
    cc::vector<cc::string> disk_serials;
};

namespace cc
{
/// Gathers exactly the fields named in `which`, and nothing else.
///
/// Unlike cc::get_system_info this is NOT memoized: it is called rarely, and caching personal data for the life of the
/// process is the opposite of what this type is for.
[[nodiscard]] cc::system_identifier query_system_identifier(cc::flags<cc::identity_field> which);
} // namespace cc
