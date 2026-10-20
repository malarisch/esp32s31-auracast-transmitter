#!/usr/bin/env python3
"""Subscribe a Linux PC (BlueZ + PipeWire) to the Auracast broadcast of the bridge.

Does what `bluetoothctl` does by hand (remove stale device, `scan le`, `transport.select`),
directly over D-Bus, and re-subscribes on its own when the broadcast disappears (board reset).
PipeWire then offers one source per BIS (bluez_input.<MAC>...), which WirePlumber usually
links to the default speakers.

Requires: BlueZ with LE Audio enabled (see README), PipeWire with LC3, python3-dbus, python3-gi.

  tools/auracast_listen.py                        # broadcast name "AES67 Auracast"
  tools/auracast_listen.py --name "Other Name"
  tools/auracast_listen.py --address 1C:29:04:D0:77:CD
  tools/auracast_listen.py --bis 1               # only BIS 1 (e.g. the Standard Quality subgroup)
"""

import argparse
import signal
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

try:
    from gi.repository import GLibUnix
    unix_signal_add = GLibUnix.signal_add
except ImportError:
    unix_signal_add = GLib.unix_signal_add

BLUEZ = "org.bluez"
ADAPTER_IF = "org.bluez.Adapter1"
DEVICE_IF = "org.bluez.Device1"
ENDPOINT_IF = "org.bluez.MediaEndpoint1"
TRANSPORT_IF = "org.bluez.MediaTransport1"
PROPS_IF = "org.freedesktop.DBus.Properties"
OM_IF = "org.freedesktop.DBus.ObjectManager"
BCAST_SINK_UUID = "00001851-0000-1000-8000-00805f9b34fb"  # Basic Audio Announcement

SELECT_DELAY_MS = 500  # wait for all BIS transports of the BIG before selecting
SCAN_TIMEOUT_S = 30  # nothing to select yet -> remove the device and scan again
ACTIVE_TIMEOUT_S = 15  # selected but not streaming -> start over


def bis(path):
    return "/".join(str(path).split("/")[-3:-1])  # .../sid0/bis1/fd0 -> sid0/bis1


def filter_bis(paths, spec):
    """Keep the transports whose BIS number (…/bisN/fd0) is in the comma-separated spec; None = all."""
    if spec is None:
        return list(paths)
    wanted = {int(x) for x in spec.split(",") if x.strip()}
    return [p for p in paths if int(bis(p).split("bis")[-1]) in wanted]


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


class Listener:
    def __init__(self, bus, adapter_path, name, address, keep_scanning, bis_spec=None):
        self.bus = bus
        self.adapter_path = adapter_path
        self.name = name
        self.address = address.upper() if address else None
        self.keep_scanning = keep_scanning
        self.bis_spec = bis_spec
        self.adapter = dbus.Interface(bus.get_object(BLUEZ, adapter_path), ADAPTER_IF)
        self.device_path = None
        self.transports = {}  # path -> state
        self.select_timer = None
        self.watchdog = None
        self.scanning = False
        self.selected = False

    # --- helpers -----------------------------------------------------------------------------

    def objects(self):
        om = dbus.Interface(self.bus.get_object(BLUEZ, "/"), OM_IF)
        return om.GetManagedObjects()

    def is_ours(self, dev_props):
        if self.address:
            return str(dev_props.get("Address", "")).upper() == self.address
        return str(dev_props.get("Name", "")) == self.name

    def device_props(self, path):
        try:
            props = dbus.Interface(self.bus.get_object(BLUEZ, path), PROPS_IF)
            return props.GetAll(DEVICE_IF)
        except dbus.DBusException:
            return {}

    def start_scan(self):
        if self.scanning:
            return
        try:
            self.adapter.SetDiscoveryFilter({"Transport": "le"})
            self.adapter.StartDiscovery()
            self.scanning = True
            log("scanning ...")
            self.watchdog = GLib.timeout_add_seconds(SCAN_TIMEOUT_S, self.restart, "no broadcast found")
        except dbus.DBusException as e:
            log(f"StartDiscovery failed: {e.get_dbus_message()}")

    def stop_scan(self):
        if not self.scanning:
            return
        try:
            self.adapter.StopDiscovery()
        except dbus.DBusException:
            pass
        self.scanning = False

    def cancel_timers(self):
        for t in (self.select_timer, self.watchdog):
            if t is not None:
                GLib.source_remove(t)
        self.select_timer = self.watchdog = None

    # --- lifecycle ---------------------------------------------------------------------------

    def check_sink_endpoint(self):
        for path, ifaces in self.objects().items():
            ep = ifaces.get(ENDPOINT_IF)
            if ep and str(path).startswith(self.adapter_path) and str(ep.get("UUID", "")) == BCAST_SINK_UUID:
                return True
        return False

    def remove_stale(self, quiet=False):
        """BlueZ keeps PA/BIG state of a device across board resets; only a fresh device syncs again."""
        for path, ifaces in self.objects().items():
            dev = ifaces.get(DEVICE_IF)
            if dev and str(path).startswith(self.adapter_path + "/") and self.is_ours(dev):
                if not quiet:
                    log(f"removing stale device {dev.get('Address')}")
                try:
                    self.adapter.RemoveDevice(path)
                except dbus.DBusException as e:
                    if not quiet:
                        log(f"RemoveDevice failed: {e.get_dbus_message()}")

    def restart(self, reason):
        log(f"restart: {reason}")
        self.cancel_timers()
        self.unselect()
        self.transports.clear()
        self.device_path = None
        self.selected = False
        self.stop_scan()
        self.remove_stale()
        self.start_scan()
        return False

    def shutdown(self):
        self.cancel_timers()
        self.unselect()
        self.stop_scan()
        self.remove_stale(quiet=True)  # leaves BlueZ clean for the next run

    # --- transports --------------------------------------------------------------------------

    def on_interfaces_added(self, path, ifaces):
        path = str(path)
        if DEVICE_IF in ifaces and self.device_path is None and self.is_ours(ifaces[DEVICE_IF]):
            dev = ifaces[DEVICE_IF]
            log(f"found {dev.get('Name', '?')} ({dev.get('Address')})")
        if TRANSPORT_IF not in ifaces:
            return
        dev_path = str(ifaces[TRANSPORT_IF].get("Device", ""))
        if self.device_path is None:
            if not self.is_ours(self.device_props(dev_path)):
                return
            self.device_path = dev_path
        elif dev_path != self.device_path:
            return
        self.transports[path] = str(ifaces[TRANSPORT_IF].get("State", "?"))
        log(f"transport {bis(path)} ({self.transports[path]})")
        if self.select_timer is not None:
            GLib.source_remove(self.select_timer)
        self.select_timer = GLib.timeout_add(SELECT_DELAY_MS, self.select)

    def on_interfaces_removed(self, path, ifaces):
        path = str(path)
        if path in self.transports and TRANSPORT_IF in ifaces:
            del self.transports[path]
            if not self.transports:
                self.restart("broadcast lost")
        elif path == self.device_path and DEVICE_IF in ifaces:
            self.restart("device removed")

    def on_properties_changed(self, iface, changed, _invalidated, path=None):
        if iface != TRANSPORT_IF or path not in self.transports or "State" not in changed:
            return
        state = str(changed["State"])
        self.transports[path] = state
        log(f"{bis(path)}: {state}")
        if self.selected and all(s == "active" for s in self.transports.values()):
            if self.watchdog is not None:
                GLib.source_remove(self.watchdog)
                self.watchdog = None
            log(f"receiving {len(self.transports)} BIS -> PipeWire sources bluez_input.*")
            if not self.keep_scanning:
                self.stop_scan()
        elif self.selected and state == "idle":
            self.selected = False  # schedule the restart once, not once per BIS
            GLib.idle_add(self.restart, "transport went idle")

    def select(self):
        self.select_timer = None
        paths = filter_bis(sorted(self.transports), self.bis_spec)
        if not paths:
            log(f"no transport matches --bis {self.bis_spec}" if self.bis_spec else "no transports")
            return False
        tr = lambda p: self.bus.get_object(BLUEZ, p)
        # Same as `transport.select A B ...` in bluetoothctl: link all BIS to the first one, then select.
        if len(paths) > 1:
            try:
                dbus.Interface(tr(paths[0]), PROPS_IF).Set(
                    TRANSPORT_IF, "Links", dbus.Array([dbus.ObjectPath(p) for p in paths[1:]], signature="o"))
            except dbus.DBusException as e:
                log(f"linking transports failed: {e.get_dbus_message()}")
        for p in paths:
            try:
                dbus.Interface(tr(p), TRANSPORT_IF).Select()
            except dbus.DBusException as e:
                log(f"Select {p} failed: {e.get_dbus_message()}")
        self.selected = True
        log(f"selected {len(paths)} BIS")
        if self.watchdog is not None:
            GLib.source_remove(self.watchdog)
        self.watchdog = GLib.timeout_add_seconds(ACTIVE_TIMEOUT_S, self.restart, "no audio after select")
        return False

    def unselect(self):
        if not self.selected:
            return
        for p in self.transports:
            try:
                dbus.Interface(self.bus.get_object(BLUEZ, p), TRANSPORT_IF).Unselect()
            except dbus.DBusException:
                pass
        self.selected = False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--name", default="AES67 Auracast", help="broadcast name (default: %(default)s)")
    ap.add_argument("--address", help="match the broadcaster by MAC instead of by name")
    ap.add_argument("--adapter", default="hci0")
    ap.add_argument("--keep-scanning", action="store_true",
                    help="keep LE discovery running while receiving (default: stop once audio flows)")
    ap.add_argument("--bis", help="comma-separated BIS numbers to select, e.g. 1,2 (default: all)")
    args = ap.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    listener = Listener(bus, f"/org/bluez/{args.adapter}", args.name, args.address, args.keep_scanning, args.bis)

    if not listener.check_sink_endpoint():
        sys.exit("no broadcast sink endpoint registered (BlueZ Experimental/KernelExperimental set? "
                 "PipeWire running? otherwise: sudo systemctl restart bluetooth)")

    bus.add_signal_receiver(listener.on_interfaces_added, "InterfacesAdded", OM_IF, BLUEZ)
    bus.add_signal_receiver(listener.on_interfaces_removed, "InterfacesRemoved", OM_IF, BLUEZ)
    bus.add_signal_receiver(listener.on_properties_changed, "PropertiesChanged", PROPS_IF, BLUEZ,
                            path_keyword="path")

    loop = GLib.MainLoop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: (loop.quit(), False)[1])

    log(f"waiting for {args.address or repr(args.name)} on {args.adapter} (Ctrl-C to stop)")
    listener.remove_stale()
    listener.start_scan()
    loop.run()
    listener.shutdown()
    log("stopped")


if __name__ == "__main__":
    main()
