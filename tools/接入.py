import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile
import time

import serial
from serial.tools.list_ports import comports


def detect_port():
    ports = [port.device for port in comports() if (port.vid, port.pid) == (0x303A, 0x1001)]
    if len(ports) != 1:
        raise SystemExit("请用 USB 数据线接入一台 AI Passport，拔掉其他 Espressif USB 设备后重试。")
    return ports[0]


def local_ipv4(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        return ""
    if address.is_unspecified or address.is_loopback or address.is_link_local or address.is_multicast:
        return ""
    return str(address)


def command_output(*args):
    return subprocess.run(args, capture_output=True, text=True, check=False).stdout


def ask_host():
    default_route = command_output("/sbin/route", "-n", "get", "default")
    primary = re.findall(r"^\s*interface:\s*(en\d+)", default_route, re.M)
    ports = command_output("/usr/sbin/networksetup", "-listallhardwareports")
    interfaces = dict.fromkeys(primary + re.findall(r"^Device:\s*(en\d+)\s*$", ports, re.M))
    default = ""
    for interface in interfaces:
        default = local_ipv4(command_output("/usr/sbin/ipconfig", "getifaddr", interface).strip())
        if default:
            break
    while True:
        hint = f" [{default}，回车采用]" if default else ""
        host = local_ipv4(input(f"Mac 的局域网 IPv4{hint}：").strip() or default)
        if host:
            return host
        print("请输入有效的局域网 IPv4，例如 192.168.1.20。")


def send_config(port, message, success, rejected):
    with serial.Serial(baudrate=115200, timeout=0.3, write_timeout=2) as connection:
        connection.port = port
        connection.dtr = False
        connection.rts = False
        connection.open()
        connection.reset_input_buffer()
        deadline = time.monotonic() + 14
        next_write = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_write:
                connection.write(b"\n" + message + b"\n")
                connection.flush()
                next_write = time.monotonic() + 1.5
            line = connection.readline().strip()
            if line == success:
                return
            if line == rejected:
                raise SystemExit("开发板拒绝了配置，请核对固件和输入。")
    raise SystemExit("未收到开发板确认，请核对当前刷入的固件、USB 数据线和串口占用，再重试。")


def configure_wifi(port, host):
    ssid = input("2.4 GHz Wi-Fi 名称：")
    password = getpass.getpass("Wi-Fi 密码（输入不可见）：")
    if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 64:
        raise SystemExit("Wi-Fi 名称应为 1 至 32 字节，密码最长 64 字节。")
    if "\0" in ssid or "\0" in password:
        raise SystemExit("Wi-Fi 名称和密码不能含空字符。")
    # The setup firmware requires the legacy fields. The mirror ignores them.
    settings = {"ssid": ssid, "wifi": password, "host": host, "vnc": "unused"}
    message = b"APVNC " + json.dumps(settings, ensure_ascii=False).encode()
    if len(message) > 767:
        raise SystemExit("Wi-Fi 配置超过开发板的接收长度。")
    send_config(port, message, b"APVNC CONFIG OK", b"APVNC CONFIG ERROR")
    print("Wi-Fi 已写入。请按说明刷入音视频正式版，再执行 pair。")


def pair(port, host):
    folder = Path(__file__).resolve().parents[1] / "work"
    folder.mkdir(mode=0o700, exist_ok=True)
    pending = folder / "mac-screen-pair.pending.json"
    if pending.exists():
        saved = json.loads(pending.read_text(encoding="utf-8"))
        key = saved["key"]
        if not re.fullmatch(r"[0-9a-f]{64}", key):
            raise SystemExit("待确认配对文件已损坏，请移除 work/mac-screen-pair.pending.json 后重试。")
    else:
        key = secrets.token_hex(32)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=folder, delete=False) as file:
        json.dump({"host": host, "key": key}, file)
    os.replace(file.name, pending)
    send_config(port, f"APSCREEN {host} {key}".encode(), b"APSCREEN PAIRED", b"APSCREEN ERROR")
    pending.replace(folder / "mac-screen-pair.json")
    app_name = "MaToy.app" if (folder.parent / "outputs" / "MaToy.app").is_dir() else "AI Passport 实时控制.app"
    print(f"USB 配对成功，开发板正在重启。请打开 outputs 中的 {app_name}。")


def main():
    parser = argparse.ArgumentParser(description="MaToy USB 接入（AI Passport）")
    parser.add_argument("action", choices=("port", "wifi", "pair"))
    args = parser.parse_args()
    port = detect_port()
    if args.action == "port":
        print(port)
        return
    try:
        host = ask_host()
        if args.action == "wifi":
            configure_wifi(port, host)
        else:
            pair(port, host)
    except (EOFError, KeyboardInterrupt):
        raise SystemExit("配置已中止。") from None
    except serial.SerialException:
        raise SystemExit("无法读写 USB 串口，请关闭串口监视器并重新插好数据线后重试。") from None


if __name__ == "__main__":
    main()
