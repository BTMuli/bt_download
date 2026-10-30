# libtorrent Tracker / Peer observation

`libtorrent/` is the vcpkg libtorrent 2.0.11 port from the pinned baseline,
with `tracker-peer-observer.patch` applied. The original port recipe is from
[Microsoft vcpkg](https://github.com/microsoft/vcpkg/tree/e03dc9b29710050cd1018bc5674688108658d327/ports/libtorrent)
and is licensed under the [MIT license](LICENSE-vcpkg.txt).

The patch adds an observation-only `torrent_plugin::on_tracker_peer(url, endpoint)`
callback for compact IPv4/IPv6 responses and asynchronously resolved non-compact
peer hostnames. It records the original requesting tracker URL and does not issue
additional announces or alter connection selection. I2P destinations are not TCP
endpoints and are outside this engine's Peer view.

This adds a virtual method to the plugin interface. Build and ship the engine and
the patched libtorrent together; an existing unpatched DLL is not ABI compatible.
The normal manifest-mode build automatically selects this overlay. The engine
rejects unpatched headers at compilation rather than inventing tracker attribution.
When upgrading libtorrent, rebase the patch and verify compact, non-compact,
hostname, multiple-tracker and concurrent-response cases.
