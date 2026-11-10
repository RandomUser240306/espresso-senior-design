"""Desktop serial UI for displaying scale readings and requesting tare."""

from __future__ import annotations

import math
import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
TARE_TIMEOUT_MS = 10000


class ScaleUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Kitchen Scale")
        self.root.resizable(False, False)
        self.root.configure(bg="#e5e5e5")

        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.connection: serial.Serial | None = None
        self.stop_reader: threading.Event | None = None
        self.reader_thread: threading.Thread | None = None
        self.tare_timeout: str | None = None
        self.ready = False

        self.port_var = tk.StringVar()
        self.weight_var = tk.StringVar(value="---.--")
        self.status_var = tk.StringVar(value="Disconnected")

        frame = tk.Frame(root, bg="#e5e5e5", padx=20, pady=18)
        frame.grid()

        tk.Label(
            frame, text="PORT", font=("Segoe UI", 9, "bold"), bg="#e5e5e5",
            fg="#555555"
        ).grid(row=0, column=0, sticky="w", padx=(2, 8), pady=(0, 8))
        self.port_box = ttk.Combobox(
            frame, textvariable=self.port_var, state="readonly", width=12,
            postcommand=self.refresh_ports
        )
        self.port_box.grid(row=0, column=1, sticky="ew", pady=(0, 8))

        self.connect_button = ttk.Button(
            frame, text="Connect", command=self.toggle_connection
        )
        self.connect_button.grid(
            row=0, column=2, padx=(8, 0), pady=(0, 8), sticky="ew"
        )

        display = tk.Frame(
            frame, bg="#d9e4d3", padx=18, pady=14,
            highlightbackground="#aeb8a9", highlightthickness=1
        )
        display.grid(row=1, column=0, columnspan=3, sticky="ew", pady=(8, 14))
        self.weight_label = tk.Label(
            display, textvariable=self.weight_var, font=("Consolas", 42, "bold"),
            bg="#d9e4d3", fg="#263326", anchor="e"
        )
        self.weight_label.grid(row=0, column=0, sticky="ew")
        tk.Label(
            display, text="g", font=("Segoe UI", 13, "bold"),
            bg="#d9e4d3", fg="#4b5849"
        ).grid(row=0, column=1, padx=(10, 0), sticky="s")
        display.columnconfigure(0, weight=1)

        self.tare_button = ttk.Button(
            frame, text="TARE / ZERO", command=self.request_tare,
            state="disabled"
        )
        self.tare_button.grid(
            row=2, column=0, columnspan=3, sticky="ew", ipady=7
        )
        tk.Label(
            frame, textvariable=self.status_var, font=("Segoe UI", 9),
            bg="#e5e5e5", fg="#555555"
        ).grid(
            row=3, column=0, columnspan=3, pady=(9, 0)
        )

        self.refresh_ports()
        self.root.after(50, self.process_events)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def refresh_ports(self) -> None:
        ports = sorted(list_ports.comports(), key=lambda port: port.device)
        devices = [port.device for port in ports]
        self.port_box["values"] = devices
        if self.port_var.get() not in devices:
            self.port_var.set(devices[0] if devices else "")

    def toggle_connection(self) -> None:
        if self.connection is not None:
            self.disconnect()
            return

        port_name = self.port_var.get()
        if not port_name:
            messagebox.showerror("No serial port", "Select an ESP32 serial port.")
            return

        try:
            connection = serial.Serial(
                port=port_name, baudrate=BAUD_RATE, timeout=0.2, write_timeout=1
            )
        except serial.SerialException as exc:
            messagebox.showerror("Connection failed", str(exc))
            return

        self.connection = connection
        self.stop_reader = threading.Event()
        self.ready = False
        self.weight_var.set("---.--")
        self.status_var.set(f"Connected to {port_name}; waiting for ESP32")
        self.connect_button.configure(text="Disconnect")
        self.tare_button.configure(state="disabled")
        self.reader_thread = threading.Thread(
            target=self.read_serial,
            args=(connection, self.stop_reader),
            daemon=True,
        )
        self.reader_thread.start()

    def disconnect(self) -> None:
        if self.stop_reader is not None:
            self.stop_reader.set()
        if self.connection is not None:
            try:
                self.connection.close()
            except serial.SerialException:
                pass
        self.connection = None
        self.stop_reader = None
        self.ready = False
        self.cancel_tare_timeout()
        self.connect_button.configure(text="Connect")
        self.tare_button.configure(state="disabled")
        self.status_var.set("Disconnected")

    def read_serial(
        self, connection: serial.Serial, stop_event: threading.Event
    ) -> None:
        try:
            while not stop_event.is_set():
                raw_line = connection.readline()
                if not raw_line:
                    continue
                line = raw_line.decode("utf-8", errors="replace").strip()
                if line.startswith("@"):
                    self.events.put(("protocol", line))
        except (serial.SerialException, OSError) as exc:
            if not stop_event.is_set():
                self.events.put(("disconnected", str(exc)))

    def request_tare(self) -> None:
        if self.connection is None or not self.ready:
            return
        try:
            self.connection.write(b"TARE\n")
        except (serial.SerialException, OSError) as exc:
            self.status_var.set(f"Unable to send tare: {exc}")
            self.disconnect()
            return

        self.tare_button.configure(state="disabled")
        self.status_var.set("Taring; keep the scale unloaded and still...")
        self.cancel_tare_timeout()
        self.tare_timeout = self.root.after(
            TARE_TIMEOUT_MS, self.tare_timed_out
        )

    def tare_timed_out(self) -> None:
        self.tare_timeout = None
        if self.connection is not None:
            self.status_var.set("Tare timed out; check the ESP32 connection.")
            self.tare_button.configure(state="normal" if self.ready else "disabled")

    def cancel_tare_timeout(self) -> None:
        if self.tare_timeout is not None:
            self.root.after_cancel(self.tare_timeout)
            self.tare_timeout = None

    def process_events(self) -> None:
        while True:
            try:
                event, value = self.events.get_nowait()
            except queue.Empty:
                break

            if event == "disconnected":
                self.disconnect()
                self.status_var.set(f"Serial connection lost: {value}")
            elif event == "protocol":
                self.handle_protocol_line(value)

        self.root.after(50, self.process_events)

    def handle_protocol_line(self, line: str) -> None:
        if line == "@READY":
            self.ready = True
            self.status_var.set("ESP32 ready")
            self.tare_button.configure(state="normal")
        elif line.startswith("@DATA,"):
            try:
                weight = float(line.split(",", maxsplit=1)[1])
            except ValueError:
                return
            if math.isfinite(weight):
                self.weight_var.set(f"{weight:.2f} g")
                if not self.ready:
                    self.ready = True
                    self.status_var.set("ESP32 connected")
                    self.tare_button.configure(state="normal")
        elif line.startswith("@TARE,OK,"):
            self.cancel_tare_timeout()
            self.weight_var.set("0.00")
            self.status_var.set("Zeroed")
            self.tare_button.configure(state="normal" if self.ready else "disabled")
        elif line.startswith("@TARE,ERROR,"):
            self.cancel_tare_timeout()
            self.status_var.set(f"Tare failed: {line.split(',', maxsplit=2)[2]}")
            self.tare_button.configure(state="normal" if self.ready else "disabled")
        elif line.startswith("@ERROR,"):
            self.status_var.set(f"ESP32: {line.split(',', maxsplit=1)[1]}")

    def close(self) -> None:
        self.disconnect()
        self.root.destroy()


def main() -> None:
    if not hasattr(serial, "Serial") or not hasattr(serial, "SerialException"):
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror(
            "PySerial installation problem",
            "The imported 'serial' module is not PySerial. In PowerShell, run:\n\n"
            "py -3.13 -m pip uninstall serial\n"
            "py -3.13 -m pip install --force-reinstall pyserial",
        )
        root.destroy()
        return

    root = tk.Tk()
    ScaleUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
