# greetd as far as a greeter sees it (greetd-ipc): a socket at argv[1], the
# password "hunter2" for anyone, what the greeter asked for printed
import json, os, socket, struct, sys

path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(4)

def say(text):
    print(text, flush=True)

def receive(c):
    head = c.recv(4, socket.MSG_WAITALL)
    if len(head) < 4:
        return None
    (n,) = struct.unpack('=I', head)
    return json.loads(c.recv(n, socket.MSG_WAITALL))

def send(c, message):
    data = json.dumps(message).encode()
    c.sendall(struct.pack('=I', len(data)) + data)

while True:
    c, _ = server.accept()
    user = None
    while True:
        m = receive(c)
        if m is None:
            break
        if m['type'] == 'create_session':
            user = m['username']
            say('session for ' + user)
            send(c, {'type': 'auth_message', 'auth_message_type': 'info',
                'auth_message': 'Hello'})
        elif m['type'] == 'post_auth_message_response' and 'response' not in m:
            send(c, {'type': 'auth_message', 'auth_message_type': 'secret',
                'auth_message': 'Password: '})
        elif m['type'] == 'post_auth_message_response':
            if m['response'] == 'hunter2':
                say('right password')
                send(c, {'type': 'success'})
            else:
                say('refused ' + m['response'])
                send(c, {'type': 'error', 'error_type': 'auth_error',
                    'description': 'pam_authenticate: AUTH_ERR'})
        elif m['type'] == 'cancel_session':
            say('cancelled')
            send(c, {'type': 'success'})
        elif m['type'] == 'start_session':
            say('start ' + ' '.join(m['cmd']))
            say('env ' + ' '.join(m['env']))
            send(c, {'type': 'success'})
    c.close()
