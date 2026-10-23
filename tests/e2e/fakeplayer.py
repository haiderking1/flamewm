# fakeplayer.py TITLE ART_URL: a fake mpris player for the tests, on
# whatever session bus DBUS_SESSION_BUS_ADDRESS says, playing TITLE with
# ART_URL as its art (a file:// or an http:// one)
import sys
from gi.repository import Gio, GLib

TITLE, ART = sys.argv[1], sys.argv[2]
XML = '''<node>
<interface name="org.mpris.MediaPlayer2">
<property name="Identity" type="s" access="read"/>
</interface>
<interface name="org.mpris.MediaPlayer2.Player">
<method name="PlayPause"/><method name="Next"/><method name="Previous"/>
<property name="PlaybackStatus" type="s" access="read"/>
<property name="Metadata" type="a{sv}" access="read"/>
<property name="CanSeek" type="b" access="read"/>
<property name="Position" type="x" access="read"/>
</interface></node>'''

def get(conn, sender, path, iface, name):
    return {
        'Identity': GLib.Variant('s', 'fake'),
        'PlaybackStatus': GLib.Variant('s', 'Playing'),
        'Metadata': GLib.Variant('a{sv}', {
            'xesam:title': GLib.Variant('s', TITLE),
            'xesam:artist': GLib.Variant('as', ['The Tests']),
            'mpris:artUrl': GLib.Variant('s', ART),
            'mpris:length': GLib.Variant('x', 200 * 1000000),
            'mpris:trackid': GLib.Variant('o', '/fake/track/1'),
        }),
        'CanSeek': GLib.Variant('b', False),
        'Position': GLib.Variant('x', 0),
    }.get(name)

def call(conn, sender, path, iface, method, params, invocation):
    invocation.return_value(None)

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
node = Gio.DBusNodeInfo.new_for_xml(XML)
for iface in node.interfaces:
    bus.register_object('/org/mpris/MediaPlayer2', iface, call, get, None)
Gio.bus_own_name_on_connection(bus, 'org.mpris.MediaPlayer2.fake', 0, None, None)
GLib.MainLoop().run()
