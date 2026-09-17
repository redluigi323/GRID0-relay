"""macOS and Linux launcher lifecycle regression. Uses only local IPC and unprivileged children."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile

helper = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="zll-supervisor-", dir="/tmp") as directory:
    def run(disconnect=False, flood=False):
        path = str(pathlib.Path(directory) / "control")
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        server.listen(1)
        server.settimeout(5)
        child = "import os,signal,time; signal.signal(signal.SIGINT,lambda *_:exit(0)); print('CHILD',os.getpid(),flush=True); "
        child += "print('x'*200000,flush=True); " if flood else ""
        child += "time.sleep(30)"
        process = subprocess.Popen([str(helper), path, str(os.getuid()), sys.executable, "-u", "-c", child], stderr=subprocess.PIPE)
        connection, _ = server.accept()
        connection.settimeout(5)
        buffer = b""
        while b"\n" not in buffer:
            buffer += connection.recv(65536)
        pid = int(buffer.split(b"\n", 1)[0].split()[1])
        if flood:
            while buffer.count(b"x") < 200000:
                buffer += connection.recv(65536)
        if disconnect:
            connection.close()
        else:
            connection.sendall(b"STOP\n")
        code = process.wait(timeout=8)
        assert code == 0, process.stderr.read().decode()
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("Relay child remained alive after supervisor exit")
        connection.close()
        server.close()
        pathlib.Path(path).unlink()
    run()
    run(disconnect=True)
    run(flood=True)
print("PASS: supervisor STOP, GUI disconnect, output streaming, and child reaping")
