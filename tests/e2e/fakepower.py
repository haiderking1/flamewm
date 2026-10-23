# fakepower.py STATE.json: a fake upower for the tests, on whatever system bus
# DBUS_SYSTEM_BUS_ADDRESS says. its display device is what STATE.json says,
# read again every half second, PropertiesChanged when it changes
import json, sys
from gi.repository import Gio, GLib

STATE = sys.argv[1]
XML = '''<node><interface name="org.freedesktop.UPower.Device">
<property name="IsPresent" type="b" access="read"/><property name="Type" type="u" access="read"/>
<property name="State" type="u" access="read"/><property name="Percentage" type="d" access="read"/>
<property name="TimeToEmpty" type="x" access="read"/><property name="TimeToFull" type="x" access="read"/>
</interface></node>'''
KINDS = {'IsPresent': 'b', 'Type': 'u', 'State': 'u', 'Percentage': 'd', 'TimeToEmpty': 'x', 'TimeToFull': 'x'}
now = {}

def load():
    try:
        return json.load(open(STATE))
    except Exception:
        return now

def get(conn, sender, path, iface, prop):
    return GLib.Variant(KINDS[prop], now.get(prop, 0))

bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
bus.register_object('/org/freedesktop/UPower/devices/DisplayDevice', info, None, get, None)
now = load()
Gio.bus_own_name_on_connection(bus, 'org.freedesktop.UPower', 0, None, None)

def poll():
    global now
    fresh = load()
    if fresh != now:
        now = fresh
        changed = {k: GLib.Variant(KINDS[k], v) for k, v in now.items() if k in KINDS}
        bus.emit_signal(None, '/org/freedesktop/UPower/devices/DisplayDevice',
            'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            GLib.Variant('(sa{sv}as)', ('org.freedesktop.UPower.Device', changed, [])))
    return True

GLib.timeout_add(500, poll)
GLib.MainLoop().run()
