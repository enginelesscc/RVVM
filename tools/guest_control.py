"""Framebuffer capture and HID input over RVVM's localhost control socket."""

import argparse
import socket
import time


class Guest:
    def __init__(self, port=1337):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.stream = self.socket.makefile("rb")

    def close(self):
        self.stream.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, command):
        self.socket.sendall(command.encode("ascii") + b"\n")
        reply = self.stream.readline(256).decode("ascii").strip()
        if not reply or reply.startswith("ERROR"):
            raise RuntimeError(reply or "Control connection closed")
        return reply

    def frame(self, path):
        from PIL import Image

        reply = self.request("frame").split()
        if len(reply) != 6 or reply[0] != "FRAME":
            raise RuntimeError(f"Invalid frame header: {reply}")
        width, height, stride, pixel_format, size = map(int, reply[1:])
        if size > 64 * 1024 * 1024 or size != stride * height:
            raise RuntimeError("Invalid framebuffer size")
        data = self.stream.read(size)
        if len(data) != size:
            raise RuntimeError("Truncated framebuffer")
        formats = {4: "BGRX", 3: "BGR", 0x14: "RGBX", 0x13: "RGB", 2: "BGR;16"}
        image = Image.frombytes("RGB", (width, height), data, "raw", formats[pixel_format], stride)
        image.save(path)
        return width, height

    def key(self, code, down):
        return self.request(f"key {code} {int(down)}")

    def chord(self, *codes):
        try:
            for code in codes:
                self.key(code, True)
            time.sleep(0.12)
        finally:
            for code in reversed(codes):
                self.key(code, False)

    def mouse(self, x, y, buttons=0):
        return self.request(f"mouse {x} {y} {buttons}")

    def click(self, x, y, button=1):
        self.mouse(x, y)
        time.sleep(0.08)
        self.mouse(x, y, button)
        time.sleep(0.12)
        self.mouse(x, y)

    def text(self, value):
        shifted = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
                   "&": "7", "*": "8", "(": "9", ")": "0", "_": "-", "+": "=",
                   "{": "[", "}": "]", "|": "\\", ":": ";", '"': "'", "~": "`",
                   "<": ",", ">": ".", "?": "/"}
        punctuation = dict(zip("\n\x1b\b\t -=[]\\#;'`,./", range(0x28, 0x39)))
        for char in value:
            base = shifted.get(char, char.lower())
            if "a" <= base <= "z":
                key = ord(base) - ord("a") + 4
            elif base in "1234567890":
                key = "1234567890".index(base) + 0x1E
            else:
                key = punctuation[base]
            self.chord(*([0xE1] if char.isupper() or char in shifted else []), key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=1337)
    commands = parser.add_subparsers(dest="command", required=True)
    frame = commands.add_parser("frame")
    frame.add_argument("path")
    frame.add_argument("--delay", type=float, default=0)
    click = commands.add_parser("click")
    click.add_argument("x", type=int)
    click.add_argument("y", type=int)
    click.add_argument("--button", type=int, default=1)
    key = commands.add_parser("chord")
    key.add_argument("codes", nargs="+", type=lambda value: int(value, 0))
    text = commands.add_parser("text")
    text.add_argument("value")
    args = parser.parse_args()
    with Guest(args.port) as guest:
        if args.command == "frame":
            time.sleep(args.delay)
            print(guest.frame(args.path))
        elif args.command == "click":
            guest.click(args.x, args.y, args.button)
        elif args.command == "chord":
            guest.chord(*args.codes)
        elif args.command == "text":
            guest.text(args.value)


if __name__ == "__main__":
    main()
