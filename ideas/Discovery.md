# loftail — Device discovery and device types

**Status:** Design note, 2026-09-04. Not a commitment and not an order. `ideas/ideas.md` is the
unranked brainstorm; `FUTURE.md` is the roadmap an item is promoted *to*. This file sits between
them: one idea, worked out far enough that the cost is known and the traps are written down.

---

## The idea

A **device type** is a named template carrying discovery rules, one or more log paths, and
optional credentials. The user configures a type once; thereafter they ask for "devices of type
X", pick one from a live list, and land in its logs over SSH — with config editing (M22) and
*Restart App* (M23) already pointed at the right files, because the type said where they are.

The scope ruling behind it (2026-09-04): loftail is not held to a minimal-tool discipline. Any
functionality that makes reading logs simpler, or that automates the tasks around reading them,
is in scope. `SPEC.md` §11's "strictly a reader" clause has already been amended twice — the
config write (M22) and the restart script (M23) — and device inventory is a third step in the
same direction rather than a new one.

---

## Why it is mostly composition

**A device type is a `HostBookmark` template; a discovered device is one instantiated from it.**
Every field of that struct except `host` is type-level already: `auth`, `keyFile`, `paths`,
`pollMs`, `tailStartBytes`, `compress`, and the password/keychain arrangement. Discovery supplies
the one missing field. So the data layer is a `deviceTypes.json` beside `hosts.json` holding
template plus discovery rules, and an adopted device becoming an ordinary bookmark.

**The type must not carry the log's format, config path or restart script.** Those have a home:
`logMatchTarget()` matches against the whole normalised address, `ssh://` scheme and all, so a
pattern node `ssh://*/var/log/myapp.log` already supplies the format, `configPath` and
`restartScript` for every device of the type — no fourth level, no new `LogProfile` field, and a
user can do half of this by hand today. Keep the split:

> **type = how to reach it; pattern = how to read it.**

The surfaces are precedented too. `CopyHighlightersDialog` established "the window owns the
enumeration and the picker; it holds strings and ints and answers with an index". `WelcomeView`
established "one enumeration, two renderings", "a remote row names its HOST and PATH, never its
`ssh://` address", and "the bookmark is looked up at activation and never captured" — that last
one matters more here than it did there, because a discovery list refreshes on a timer.

Opening several logs on several devices at once is `MainWindow::openFiles()` unchanged: one
funnel, non-blocking per address (M17). And the resulting tab collisions are what
`tabLabelsFor()` was built for — one service across many hosts is exactly its worked example, so
twelve devices' `app.log` come back as `app.log (rack1)`, `app.log (rack2)` with no work.

---

## The one thing that will bite: identity

Everything downstream keys on the address — `logSettingsKey()`, the spool registry,
`HostBookmarkStore::indexOfTarget()`, tab labels, the session. Open a discovered device as
`ssh://10.0.0.47/var/log/app.log` and a DHCP move next week makes it a *different log*: fresh
format, no filters, no highlighters, a fresh slot out of the pool of 500. That is the shape of
`bugs.md` 27 (the `canonicalFilePath` key) reproduced exactly, and on a lease cycle it burns a
slot a day, evicting records somebody did configure.

**The fix is not a second key space.** Give the device a stable name and put the *name* in the
address — `ssh://deploy@rack1-fan-a/var/log/app.log` — resolving name → IP once, at the
transport, beside `RemoteLocation::effectiveUser()`. Then "ONE LOG, ONE SPELLING, and the
spelling is the name as OPENED" holds unchanged, a moved device keeps its format, filters,
highlighters, config path and restart script for free, and the settings tree, the session and the
tab labels need no work at all. mDNS instance names and `~/.ssh/config` aliases already *are*
this; for a device found by bare IP, the name is assigned when the user adopts it.

---

## Discovery mechanisms, per OS

Elevation is per-OS, so it is folded into each cell. "no root" means an ordinary desktop user, no
capabilities, no driver install.

| Mechanism | Linux | Windows | macOS | Finds | Persistent ID |
|---|---|---|---|---|---|
| **mDNS / DNS-SD** | Avahi daemon (near-universal); query over D-Bus, no root. Raw UDP 5353 also no root but needs `SO_REUSEPORT` to coexist with Avahi | Native `DnsServiceBrowse` (dnsapi.dll), Win10 1703+, no admin. Or Bonjour SDK | Native `DNSServiceBrowse` in libSystem, always present, no root | Printers, NAS, IP cameras, Chromecast/AirPlay, Home Assistant, Raspberry Pis, most embedded Linux, macOS boxes, anything publishing `_ssh._tcp` | Service instance name + `.local` hostname; TXT records often carry `UUID=`/serial/model |
| **SSDP / UPnP** | Plain UDP, no root, no dependency | Same, no admin (firewall may prompt for inbound) | Same, no root | Routers, smart TVs, DLNA/Plex, IP cameras, printers, consoles, some NAS. Rarely servers | `uuid:` in `USN`/`<UDN>`; `<serialNumber>` in the description XML. Some devices regenerate on firmware update |
| **WS-Discovery** | Hand-rolled SOAP/UDP 3702, no root | Same; native `IWSDiscoveryProvider` also exists, no admin | Same, no root | Windows machines, WSD printers/scanners, **ONVIF cameras**, some NAS | `wsa:Address` = `urn:uuid:…`; ONVIF exposes serial via device-management service |
| **TCP connect probe** (:22, :443) | `connect()`, no root | no admin | no root | Anything with the port open — **the only method that crosses subnets** | **SSH host key fingerprint** from the banner exchange; TLS cert SPKI hash; banner string for OS ID |
| **Neighbour cache read** | `/proc/net/arp` or netlink `RTM_GETNEIGH`, no root | `GetIpNetTable2`, no admin | `sysctl NET_RT_FLAGS`, no root | Only what this machine has recently exchanged traffic with. Zero traffic generated | MAC + OUI vendor. Weak — randomised on phones/laptops; solid for infrastructure |
| **ICMP sweep** | Unprivileged only if `net.ipv4.ping_group_range` covers the user's gid — distro-dependent; else root | `IcmpSendEcho2` (iphlpapi), **no admin** | `SOCK_DGRAM`/`IPPROTO_ICMP` unprivileged | Anything answering ping | **None.** Liveness only; must be joined with the ARP table (same subnet) or a follow-up probe |
| **SSH config / known_hosts** | File read, no root, no dependency | same | same | Exactly what the user already connects to. Perfect precision, zero recall | The `Host` alias (user-assigned — ideal) + host key fingerprint |
| **SNMP** | UDP 161, no root; net-snmp or hand-rolled BER. Needs community/v3 creds | same | same | Switches, routers, printers, UPSes, snmpd hosts. Walking a switch's `dot1dTpFdbTable` yields the whole segment | `entPhysicalSerialNum`, `sysName`, `sysObjectID` (model), v3 engineID |
| **Router / controller API** | HTTPS, no root, needs credentials | same | same | Everything the router has seen — **best recall available without root** | Controller's own client record, usually keyed on MAC |
| **NetBIOS / SMB / LLMNR** | UDP 137 hand-rolled or Samba tools, no root | Native `NetServerEnum`, no admin (LLMNR now off by default in hardened builds) | Via smbd, no root | Windows machines, SMB NAS, some printers | NetBIOS name (weak), SMB server GUID |
| **Container / orchestrator** | Docker socket (needs `docker` group), K8s/Consul API token, no root | Docker Desktop named pipe / API | same | Containers and pods, if the fleet is containerised | Container ID, pod UID, Consul node ID |
| **LLDP / CDP** | Raw capture ⇒ **CAP_NET_RAW**. Or read `lldpd`'s socket (group perm) | Npcap driver + admin | BPF device perms ⇒ root | Switches, APs, VoIP phones, lldpd hosts — plus *which switch port* a device is on | Chassis ID (usually MAC) + port ID |
| **ARP scan (active)** | **root** (raw sockets) | **admin** + Npcap | **root** | Every live host on the segment, fastest and most complete for L2 | MAC |
| **DHCP leases (direct)** | root + on the server host | same | same | Every device that has ever leased | DHCP client-id (opt 61), DHCPv6 DUID, MAC, client hostname |
| **BLE** | BlueZ over D-Bus, no root, needs adapter | WinRT API | CoreBluetooth + **TCC permission prompt** | Sensors, wearables — not a log-bearing fleet | Randomised resolvable addresses; needs the IRK. Effectively no stable ID |

**Available to loftail** on all three platforms with no root: mDNS, SSDP, WS-Discovery, TCP probe,
neighbour-cache read, SSH config/known_hosts, SNMP, router API, NetBIOS, containers.
**Out:** LLDP, ARP scan, direct DHCP. **ICMP** is available on Windows and macOS but not reliably
on Linux, which makes it not worth carrying.

### Four sharp edges

- **`known_hosts` is hashed by default** on Debian/Ubuntu (`HashKnownHosts yes`): the host keys
  are readable, the hostnames are HMAC'd and unrecoverable. `~/.ssh/config` is the readable half
  and the more useful one anyway, its `Host` aliases being exactly the stable names the identity
  ruling above wants in the address.
- **MAC randomisation** has made the neighbour cache and every ARP-derived id unreliable for
  anything portable. Fixed infrastructure still behaves.
- **Windows Defender Firewall** prompts on first inbound UDP for mDNS/SSDP/WS-D replies. Sending
  to a multicast group and receiving on the ephemeral source port usually passes, but a device
  that answers *to the group* rather than unicast is dropped silently until the user allows it —
  worth a first-run note rather than a mystery empty list.
- **Only the TCP probe crosses a router.** Everything multicast is confined to the broadcast
  domain, so a fleet spread over VLANs needs the probe, SNMP or the controller API. That is the
  practical argument for the loud method existing at all.

The strongest identity in the table is the **SSH host key fingerprint**, and it is free: loftail
obtains it during the connect it was going to make anyway.

---

## Rules the feature has to keep

Each of these is an existing rule of the tree arriving from a new direction, not a new one.

1. **Enumeration is free; probing is a deliberate action.** No connect, no host-key check, no
   presence probe per row while building or painting the list — `WelcomeView`'s rule, and the
   reason it exists (`logSourcePresence()` is optimistic for a remote address by design, and
   anything that really looked would do I/O on a host that may be down). A reachability column is
   fine; it hangs off a Refresh press or a background worker over *adopted* devices, never off
   the enumeration.
2. **Active scanning is named and bounded.** A /24 sweep trips IDS on a corporate network and
   reads as reconnaissance. If it ships, it is a range the user typed and a button they pressed,
   never a background refresh — and it is said out loud in `SPEC.md`, the way the §11 amendments
   were.
3. **Type credentials meet unknown host keys.** Every newly discovered device has a host key not
   in `known_hosts` by definition, so auto-connecting to whatever answered a multicast probe with
   a type-wide credential is a spraying pattern. Explicit adopt-once confirmation per device
   before type credentials are used; no auto-open of discovered devices at launch.
4. **Discovery going quiet is not the link dropping.** A device disappearing from the list while
   its tab is open must leave the tab alone — the waiting (M13) and stale (`⊘`) states already
   answer the real thing correctly and must not be driven from discovery.
5. **Discovery runs off the application thread, and is abandoned rather than joined.** The M17
   and M22 rule: `restoreSession()` runs in the `MainWindow` constructor before `show()`, and a
   worker that can be blocked asking this thread for a secret makes a join a deadlock.

---

## Staging

Two milestones, and the split is delivery risk rather than scope discipline.

**First, with no network code at all:** device types as a template, plus a static device list the
user enters or imports from `~/.ssh/config`. That delivers the entire user story — "show me type
X → pick → logs, config, restart" — and every surface, funnel and settings interaction is
exercised before a packet is sent.

**Then the discovery backends**, behind a seam, one at a time: `~/.ssh/config` and the static
list are backend #1 (zero traffic, no dependency, and for this application's audience they *are*
the inventory), mDNS is #2, the TCP probe #3. The precedent is M8 — manual format entry first,
autodetection afterwards behind `IFormatProvider`, both ending at the same `PatternCompiler` —
and the dependency discipline is libssh2's and libarchive's: optional, auto-detected, and the
build stays green without any of them.

---

## What the scope ruling unlocks, and what it does not

Unlocked, and worth doing:

- **A type names several logs**, not one. A device has an application log, a syslog and a service
  log; activation opens them all through `openFiles()`, and `tabLabelsFor()` handles the naming.
- **A device list as a centre page.** M22 established that the centre area holds more than one
  kind of page and that nothing may assume a tab is a `DocumentView`, so a `DeviceView` is a
  third page kind rather than a special case.
- **Per-device actions, plural.** `RestartRunner` already runs an arbitrary script on the far end
  with `LOGFILE`/`ARCHIVE`/`MEMBER` interpolated, with the values-quoted-script-not rule pinned
  against a real `/bin/sh`. A named list per type — *Restart*, *Disk usage*, *Show units* — adds
  `HOST`/`DEVICE` to the environment and nothing else.
- **Fleet-wide gestures**: open this log on every device of type X.

Not unlocked, and not free: a **merged cross-device view** — one table interleaving several
devices' records — is genuinely new machinery (several indexes, several formats, one ordering),
not a consequence of any of the above. Cost it separately.

---

## Open questions

- Does a device type's identity survive a **rename**? The identity ruling puts the name in the
  address, which is what makes settings follow the device — and it means renaming a device is
  exactly as disruptive as renaming a log file. Probably right; worth stating rather than
  discovering.
- Is the device list a **page**, a **dock**, or an entry on `WelcomeView`? All three are
  defensible; the welcome screen already enumerates remote hosts, and "one enumeration, two
  renderings" would extend to a third.
- Does an adopted device get a `HostBookmark` **eagerly** (at adoption) or **lazily** (at first
  open)? Eager makes the type's settings visible and editable in one place; lazy keeps
  `hosts.json` free of machines nobody has read a log from.
