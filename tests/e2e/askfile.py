# askfile.py METHOD HANDLE TITLE [KEY=VALUE...]: what xdg-desktop-portal
# asks flame's file dialogs (org.freedesktop.impl.portal.FileChooser) on an
# app's behalf. prints the method, the response (0 picked, 1 cancelled) and
# the uris, the folder it's run from cut off them
import os, sys
from gi.repository import Gio, GLib

method, handle, title = sys.argv[1:4]
opts = {}
for a in sys.argv[4:]:
    k, v = a.split('=', 1)
    if k in ('current_folder', 'current_file'):
        opts[k] = GLib.Variant('ay', v.encode() + b'\0')
    elif k == 'files':
        opts[k] = GLib.Variant('aay', [n.encode() + b'\0' for n in v.split(',')])
    elif k in ('multiple', 'directory'):
        opts[k] = GLib.Variant('b', v == '1')
    elif k == 'filter':
        opts['filters'] = GLib.Variant('a(sa(us))', [('These', [(0, p) for p in v.split(',')])])
    else:
        opts[k] = GLib.Variant('s', v)
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
reply = bus.call_sync('org.freedesktop.impl.portal.desktop.flamewm', '/org/freedesktop/portal/desktop',
    'org.freedesktop.impl.portal.FileChooser', method,
    GLib.Variant('(osssa{sv})', (handle, 'test', '', title, opts)), GLib.VariantType('(ua{sv})'),
    Gio.DBusCallFlags.NONE, 20000, None)
code, results = reply.unpack()
here = 'file://' + os.getcwd()
print(method, code, ' '.join(u.replace(here, '') for u in results.get('uris', [])))
