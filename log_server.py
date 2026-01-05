import socketserver
import datetime

class LogHandler(socketserver.BaseRequestHandler):
    def handle(self):
        print(f'[{datetime.datetime.now().strftime("%H:%M:%S")}] [{self.client_address[0]}] ESP32 Connected!')
        buffer = ""
        while True:
            try:
                data = self.request.recv(1024)
                if not data: 
                    print(f'[{self.client_address[0]}] Disconnected')
                    break
                text = data.decode('utf-8', 'ignore')
                buffer += text
                # Print complete lines
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if line.strip():
                        print(f'[{datetime.datetime.now().strftime("%H:%M:%S")}] {line.strip()}')
            except Exception as e:
                print(f'[{self.client_address[0]}] Error: {e}')
                break

print('Log server running on port 8888...')
print('Waiting for ESP32 to connect...')
socketserver.TCPServer.allow_reuse_address = True
s = socketserver.TCPServer(('', 8888), LogHandler)
s.serve_forever()
